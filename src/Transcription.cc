#include "Transcription.h"

#include "LocalEnv.h"
#include "whisper.h"

#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>

extern char** environ;

namespace {

void AppendTail(std::string& tail, const char* bytes, size_t count) {
    tail.append(bytes, count);
    constexpr size_t kMaximum = 8192;
    if (tail.size() > kMaximum) tail.erase(0, tail.size() - kMaximum);
}

// Decodes `inputPath`'s audio to mono 16 kHz float32 PCM -- whisper.cpp's
// required input format -- using the same FFmpeg subprocess pipeline
// Waveform.cc's GenerateAudioWaveform already uses for its own decode.
bool DecodeMonoPcm16k(const std::string& inputPath,
                      const std::string& ffmpegPath, MediaTaskContext& context,
                      std::vector<float>& samples, std::string& error) {
    std::vector<std::string> storage = {
        ffmpegPath,  "-hide_banner",
        "-loglevel", "error",
        "-nostdin",  "-i",
        inputPath,   "-map",
        "0:a:0",     "-vn",
        "-ac",       "1",
        "-ar",       std::to_string(WHISPER_SAMPLE_RATE),
        "-f",        "f32le",
        "pipe:1",
    };
    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (std::string& value : storage) argv.push_back(value.data());
    argv.push_back(nullptr);

    int audioPipe[2] = {-1, -1};
    int errorPipe[2] = {-1, -1};
    if (pipe(audioPipe) != 0 || pipe(errorPipe) != 0) {
        error = "unable to create transcription process pipes: " +
                std::string(std::strerror(errno));
        for (int descriptor : audioPipe)
            if (descriptor >= 0) close(descriptor);
        for (int descriptor : errorPipe)
            if (descriptor >= 0) close(descriptor);
        return false;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, audioPipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, errorPipe[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, audioPipe[0]);
    posix_spawn_file_actions_addclose(&actions, errorPipe[0]);
    posix_spawn_file_actions_addclose(&actions, audioPipe[1]);
    posix_spawn_file_actions_addclose(&actions, errorPipe[1]);
    pid_t process = 0;
    const int spawnResult =
        posix_spawnp(&process, storage.front().c_str(), &actions, nullptr,
                     argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(audioPipe[1]);
    close(errorPipe[1]);
    if (spawnResult != 0) {
        close(audioPipe[0]);
        close(errorPipe[0]);
        error = "unable to start FFmpeg: " +
                std::string(std::strerror(spawnResult));
        return false;
    }

    std::vector<char> pendingBytes;
    std::string errorTail;
    bool audioOpen = true;
    bool errorOpen = true;
    bool cancelled = false;
    bool forced = false;
    auto cancellationStarted = std::chrono::steady_clock::time_point{};
    while (audioOpen || errorOpen) {
        if (!cancelled && context.Cancelled()) {
            cancelled = true;
            cancellationStarted = std::chrono::steady_clock::now();
            kill(process, SIGTERM);
        }
        if (cancelled && !forced &&
            std::chrono::steady_clock::now() - cancellationStarted >
                std::chrono::seconds(2)) {
            forced = true;
            kill(process, SIGKILL);
        }
        pollfd descriptors[2] = {
            {audioPipe[0], static_cast<short>(audioOpen ? POLLIN : 0), 0},
            {errorPipe[0], static_cast<short>(errorOpen ? POLLIN : 0), 0},
        };
        const int pollResult = poll(descriptors, 2, 100);
        if (pollResult < 0 && errno != EINTR) break;
        char bytes[8192];
        if (audioOpen &&
            (descriptors[0].revents & (POLLIN | POLLHUP | POLLERR))) {
            const ssize_t count = read(audioPipe[0], bytes, sizeof(bytes));
            if (count <= 0) {
                audioOpen = false;
                close(audioPipe[0]);
            } else {
                pendingBytes.insert(pendingBytes.end(), bytes, bytes + count);
                size_t offset = 0;
                while (pendingBytes.size() - offset >= sizeof(float)) {
                    float sample = 0.0f;
                    std::memcpy(&sample, pendingBytes.data() + offset,
                                sizeof(sample));
                    offset += sizeof(sample);
                    samples.push_back(sample);
                }
                pendingBytes.erase(pendingBytes.begin(),
                                   pendingBytes.begin() + offset);
                context.SetProgress(0.0, "Décodage audio");
            }
        }
        if (errorOpen &&
            (descriptors[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            const ssize_t count = read(errorPipe[0], bytes, sizeof(bytes));
            if (count <= 0) {
                errorOpen = false;
                close(errorPipe[0]);
            } else {
                AppendTail(errorTail, bytes, static_cast<size_t>(count));
            }
        }
    }
    int status = 0;
    while (waitpid(process, &status, 0) < 0 && errno == EINTR) {
    }
    if (audioOpen) close(audioPipe[0]);
    if (errorOpen) close(errorPipe[0]);
    if (cancelled || context.Cancelled()) {
        error = "transcription cancelled";
        return false;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        error = errorTail.empty()
                    ? "FFmpeg audio decode for transcription failed"
                    : errorTail;
        return false;
    }
    if (samples.empty()) {
        error = "audio stream produced no samples to transcribe";
        return false;
    }
    return true;
}

// Groups whisper.cpp's per-token output into words and rounds each word's
// boundary onto the source's exact frame grid (Transcription.h's single
// explicit rounding rule). Whisper's tokenizer marks the start of a new word
// with a leading space in the decoded token text (its own convention, not
// ours); a token without one is treated as a continuation of the current
// word -- this is how trailing punctuation ends up attached to the word
// before it, which is the intended behavior for cutting whole words cleanly.
// Special/timestamp tokens (id >= whisper_token_eot) carry no real text and
// are skipped, mirroring whisper.cpp's own examples/cli/cli.cpp filter.
std::vector<TranscriptWord> GroupWordsFromWhisper(whisper_context* ctx,
                                                  const MediaRate& frameRate) {
    std::vector<TranscriptWord> words;
    const whisper_token eot = whisper_token_eot(ctx);
    const int segmentCount = whisper_full_n_segments(ctx);
    std::string currentText;
    int64_t currentStartCentis = 0;
    int64_t currentEndCentis = 0;
    const auto flush = [&]() {
        if (currentText.empty()) return;
        std::string text = currentText;
        if (!text.empty() && text.front() == ' ') text.erase(text.begin());
        if (!text.empty()) {
            const RationalTime start{currentStartCentis, 100};
            const RationalTime end{currentEndCentis, 100};
            words.push_back({text, RoundToSourceFrame(start, frameRate, false),
                             RoundToSourceFrame(end, frameRate, true)});
        }
        currentText.clear();
    };
    for (int segment = 0; segment < segmentCount; ++segment) {
        const int tokenCount = whisper_full_n_tokens(ctx, segment);
        for (int token = 0; token < tokenCount; ++token) {
            const whisper_token_data data =
                whisper_full_get_token_data(ctx, segment, token);
            if (data.id >= eot) continue;
            const char* text = whisper_full_get_token_text(ctx, segment, token);
            if (!text || !*text) continue;
            const bool startsNewWord = currentText.empty() || text[0] == ' ';
            if (startsNewWord && !currentText.empty()) flush();
            if (currentText.empty()) currentStartCentis = data.t0;
            currentText += text;
            currentEndCentis = data.t1;
        }
    }
    flush();
    return words;
}

bool AppendWordText(std::string& destination, const std::string& text) {
    if (text.empty()) return false;
    constexpr std::string_view kPunctuation = ".,!?;:%)]}";
    if (!destination.empty() &&
        kPunctuation.find(text.front()) == std::string_view::npos)
        destination.push_back(' ');
    destination += text;
    return true;
}

// Outward frame rounding can make neighboring words share one source frame,
// and Whisper occasionally emits a zero-centisecond token. Keep the text in
// order while assigning only representable, non-overlapping frame spans.
// Words too short to own a frame join the next representable word (or the
// preceding one at EOF); this is preferable to caching an artifact our exact
// time model must reject later.
// Where the words being normalized come from. whisper.cpp's token
// timestamps are only *locally* monotonic: a temperature fallback re-decodes
// a window and can restart it slightly before the previous window's last
// token. That is documented decoder behaviour, not corruption, so it is
// absorbed at the boundary with whisper.cpp. A stored cache is held to the
// stricter rule -- there, out of order can only mean a damaged or
// hand-edited sidecar, and silently repairing it would hide the damage.
enum class WordSource { Decoder, StoredCache };

bool NormalizeTranscriptWords(std::vector<TranscriptWord>& words,
                              std::string& error,
                              WordSource source = WordSource::StoredCache) {
    std::vector<TranscriptWord> normalized;
    normalized.reserve(words.size());
    std::string pendingText;
    RationalTime previousRawStart{-1, 1};
    bool first = true;
    for (TranscriptWord& word : words) {
        if (word.text.empty()) {
            error = "transcript word is empty";
            return false;
        }
        if (!first && word.start < previousRawStart) {
            if (source == WordSource::StoredCache) {
                error = "transcript words are not in chronological order";
                return false;
            }
            word.start = previousRawStart;
            if (word.end < word.start) word.end = word.start;
        }
        previousRawStart = word.start;
        first = false;
        if (!normalized.empty() && word.start < normalized.back().end)
            word.start = normalized.back().end;
        if (word.end <= word.start) {
            AppendWordText(pendingText, word.text);
            continue;
        }
        if (!pendingText.empty()) {
            AppendWordText(pendingText, word.text);
            word.text = std::move(pendingText);
            pendingText.clear();
        }
        normalized.push_back(std::move(word));
    }
    if (!pendingText.empty()) {
        if (normalized.empty()) {
            error = "transcript contains no frame-representable words";
            return false;
        }
        AppendWordText(normalized.back().text, pendingText);
    }
    words = std::move(normalized);
    return true;
}

void WriteJsonString(std::ostringstream& output, const std::string& value) {
    output << '"';
    for (const char character : value) {
        switch (character) {
            case '"':
                output << "\\\"";
                break;
            case '\\':
                output << "\\\\";
                break;
            case '\n':
                output << "\\n";
                break;
            case '\r':
                output << "\\r";
                break;
            case '\t':
                output << "\\t";
                break;
            default:
                if (static_cast<unsigned char>(character) < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x",
                                  character & 0xff);
                    output << buffer;
                } else {
                    output << character;
                }
        }
    }
    output << '"';
}

void WriteTime(std::ostringstream& output, const RationalTime& time) {
    output << "{\"value\":" << time.value << ",\"rate\":" << time.rate << "}";
}

// One whisper window. Verbatim decoding re-enters whisper.cpp once per
// window so the filler prompt sits in front of each of them.
constexpr int kVerbatimWindowMs = 30000;

constexpr char kVerbatimPrompt[] =
    "Euh, heu, hum, mmh, eh bien, enfin, donc... je, je veux dire... ";

bool SaveTranscript(const std::filesystem::path& destination,
                    const Transcript& transcript, std::string& error) {
    std::error_code filesystemError;
    std::filesystem::create_directories(destination.parent_path(),
                                        filesystemError);
    if (filesystemError) {
        error = "unable to create transcript directory: " +
                filesystemError.message();
        return false;
    }
    const std::filesystem::path temporary =
        destination.parent_path() /
        (destination.filename().string() + ".partial-" + GenerateUlid());
    std::ostringstream output;
    output << "{\"version\":1,\"media_id\":";
    WriteJsonString(output, transcript.media_id);
    output << ",\"whisper_model\":";
    WriteJsonString(output, transcript.whisper_model);
    output << ",\"verbatim\":" << (transcript.verbatim ? "true" : "false");
    // Written only when set, so a transcript that has never been through the
    // alignment pass keeps the exact bytes it had before this field existed.
    if (transcript.speech_aligned) output << ",\"speech_aligned\":true";
    output << ",\"source_rate\":{\"num\":" << transcript.source_rate.num
           << ",\"den\":" << transcript.source_rate.den << "},\"words\":[";
    for (size_t index = 0; index < transcript.words.size(); ++index) {
        if (index) output << ',';
        const TranscriptWord& word = transcript.words[index];
        output << "{\"text\":";
        WriteJsonString(output, word.text);
        output << ",\"start\":";
        WriteTime(output, word.start);
        output << ",\"end\":";
        WriteTime(output, word.end);
        output << '}';
    }
    output << "]}\n";
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    file << output.str();
    file.close();
    if (!file) {
        error = "unable to write transcript cache";
        std::filesystem::remove(temporary, filesystemError);
        return false;
    }
    std::filesystem::rename(temporary, destination, filesystemError);
    if (filesystemError) {
        error =
            "unable to install transcript cache: " + filesystemError.message();
        std::filesystem::remove(temporary, filesystemError);
        return false;
    }
    return true;
}

// Minimal hand-rolled JSON reader for the transcript cache, matching the
// style Operations.cc's Reader and EditLog.cc's LogReader already use in
// this codebase rather than sharing a parser across translation units
// (neither of those is exposed for reuse outside its own file).
class Reader {
public:
    explicit Reader(const std::string& input) : input_(input) {}

    void Expect(const std::string& text) {
        Skip();
        if (input_.compare(position_, text.size(), text) != 0) {
            throw std::runtime_error("expected '" + text + "' at byte " +
                                     std::to_string(position_));
        }
        position_ += text.size();
    }

    bool Consume(const std::string& text) {
        Skip();
        if (input_.compare(position_, text.size(), text) != 0) return false;
        position_ += text.size();
        return true;
    }

    std::string String() {
        Skip();
        if (position_ >= input_.size() || input_[position_++] != '"') {
            throw std::runtime_error("expected string at byte " +
                                     std::to_string(position_));
        }
        std::string output;
        while (position_ < input_.size()) {
            const char character = input_[position_++];
            if (character == '"') return output;
            if (character == '\\') {
                if (position_ >= input_.size())
                    throw std::runtime_error("unterminated string escape");
                const char escaped = input_[position_++];
                switch (escaped) {
                    case '"':
                        output.push_back('"');
                        break;
                    case '\\':
                        output.push_back('\\');
                        break;
                    case 'n':
                        output.push_back('\n');
                        break;
                    case 'r':
                        output.push_back('\r');
                        break;
                    case 't':
                        output.push_back('\t');
                        break;
                    case 'u': {
                        if (position_ + 4 > input_.size())
                            throw std::runtime_error(
                                "truncated unicode escape");
                        const std::string hex = input_.substr(position_, 4);
                        position_ += 4;
                        const long codepoint =
                            std::strtol(hex.c_str(), nullptr, 16);
                        output.push_back(static_cast<char>(codepoint & 0x7f));
                        break;
                    }
                    default:
                        throw std::runtime_error("unsupported string escape");
                }
            } else {
                output.push_back(character);
            }
        }
        throw std::runtime_error("unterminated string");
    }

    int64_t Integer() {
        Skip();
        const size_t start = position_;
        if (position_ < input_.size() && input_[position_] == '-') ++position_;
        while (position_ < input_.size() &&
               std::isdigit(static_cast<unsigned char>(input_[position_]))) {
            ++position_;
        }
        if (position_ == start ||
            (position_ == start + 1 && input_[start] == '-')) {
            throw std::runtime_error("expected integer at byte " +
                                     std::to_string(start));
        }
        char* end = nullptr;
        const std::string text = input_.substr(start, position_ - start);
        const long long value = std::strtoll(text.c_str(), &end, 10);
        if (!end || *end != '\0') throw std::runtime_error("invalid integer");
        return static_cast<int64_t>(value);
    }

    bool Boolean() {
        if (Consume("true")) return true;
        if (Consume("false")) return false;
        throw std::runtime_error("expected boolean at byte " +
                                 std::to_string(position_));
    }

    void Finish() {
        Skip();
        if (position_ != input_.size())
            throw std::runtime_error("unexpected trailing transcript JSON");
    }

private:
    void Skip() {
        while (position_ < input_.size() &&
               std::isspace(static_cast<unsigned char>(input_[position_]))) {
            ++position_;
        }
    }
    const std::string& input_;
    size_t position_ = 0;
};

RationalTime ReadTime(Reader& reader) {
    reader.Expect("{\"value\":");
    const int64_t value = reader.Integer();
    reader.Expect(",\"rate\":");
    const int64_t rate = reader.Integer();
    reader.Expect("}");
    if (rate <= 0 || rate > std::numeric_limits<int32_t>::max()) {
        throw std::runtime_error("transcript RationalTime rate out of range");
    }
    return {value, static_cast<int32_t>(rate)};
}

}  // namespace

RationalTime RoundToSourceFrame(const RationalTime& time,
                                const MediaRate& frameRate, bool roundUp) {
    if (frameRate.num <= 0 || frameRate.den <= 0)
        throw std::invalid_argument("frame rate must be positive");
    const int64_t frameIndex = time.to_frames(frameRate.num, frameRate.den);
    const RationalTime floorTime{
        frameIndex * static_cast<int64_t>(frameRate.den), frameRate.num};
    if (!roundUp || floorTime == time) return floorTime;
    return RationalTime{(frameIndex + 1) * static_cast<int64_t>(frameRate.den),
                        frameRate.num};
}

namespace {

// QC-2026-09 A3 -- one media, against a model already in memory. Split out of
// GenerateAudioTranscript so a batch loads the model once: the load measured
// about 8 s, which on the 43 spoken rushes of one project was nearly six
// minutes spent doing the same thing forty-three times. Nothing else about
// the pass changed when it moved here.
//
// Sharing the context across media does not leak one rush's words into the
// next one's decoding: whisper_full_default_params sets no_context, and
// whisper.cpp clears prompt_past at the top of every whisper_full call. The
// verbatim path's per-window prompt seeding below is unaffected for the same
// reason -- it was always per call, never per context.
bool TranscribeOneWithModel(whisper_context* ctx, const std::string& inputPath,
                            const std::string& outputPath,
                            const std::string& mediaId,
                            const MediaRate& sourceRate,
                            const WhisperSettings& settings,
                            MediaTaskContext& context, std::string& error) {
    error.clear();
    if (sourceRate.num <= 0 || sourceRate.den <= 0) {
        error = "invalid source frame rate";
        return false;
    }
    std::vector<float> samples;
    if (!DecodeMonoPcm16k(inputPath, settings.ffmpeg_path, context, samples,
                          error)) {
        return false;
    }
    if (context.Cancelled()) {
        error = "transcription cancelled";
        return false;
    }

    context.SetProgress(0.5, "Transcription");
    whisper_full_params params =
        whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    params.print_progress = false;
    params.print_realtime = false;
    params.print_special = false;
    params.print_timestamps = false;
    params.translate = false;
    params.token_timestamps = true;
    params.single_segment = false;
    params.initial_prompt = settings.verbatim ? kVerbatimPrompt : nullptr;
    // ALPHA-2026-08 -- whisper.cpp conditions every 30 s window on the text
    // decoded so far (its prompt_past, consulted only while n_max_text_ctx is
    // above zero). On a rush, speech is a minority of the runtime: one
    // caption credit hallucinated over music or room tone is fed back as
    // context and then repeats to the end of the media. Measured on a 7 min
    // 35 interview, large-v3 emitted "Sous-titrage Societe Radio-Canada" 32
    // times and not one real sentence. Cross-window coherence is worth less
    // than never poisoning the remainder of the transcript.
    //
    // The verbatim pass cannot take that shortcut: the filler prompt reaches
    // the decoder through the very same prompt_past, so it needs
    // conditioning switched on. It buys back the safety by walking the media
    // one window per call (see kVerbatimWindowMs below), which bounds any
    // hallucination to the window that produced it.
    params.n_max_text_ctx = settings.verbatim ? 16384 : 0;
    // Caption furniture (">>", musical notes, brackets) is never a spoken
    // word. Suppressing those tokens removes at the source the artifacts the
    // editorial layer would otherwise have to recognise and filter.
    params.suppress_nst = true;
    params.language =
        settings.language.empty() ? "auto" : settings.language.c_str();
    params.n_threads =
        static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));

    // whisper.cpp seeds prompt_past from initial_prompt once per call, then
    // overwrites it with each decoded segment (its own "update prompt_past").
    // A single call therefore biases only the first window toward fillers --
    // measured: identical words and timecodes with and without --verbatim
    // over a 7 min media. Advancing one window per call is what makes the
    // prompt reach every window. Boundaries land on whisper's own segment
    // ends rather than on a fixed grid, so no word is cut in half.
    const int64_t totalCentis =
        static_cast<int64_t>(samples.size()) * 100 / WHISPER_SAMPLE_RATE;
    std::vector<TranscriptWord> words;
    int64_t cursorCentis = 0;
    while (true) {
        params.offset_ms = static_cast<int>(cursorCentis * 10);
        params.duration_ms = settings.verbatim ? kVerbatimWindowMs : 0;
        const int result = whisper_full(ctx, params, samples.data(),
                                        static_cast<int>(samples.size()));
        if (result != 0) {
            error = "local whisper.cpp inference failed with code " +
                    std::to_string(result);
            return false;
        }
        if (context.Cancelled()) {
            error = "transcription cancelled";
            return false;
        }
        std::vector<TranscriptWord> pass =
            GroupWordsFromWhisper(ctx, sourceRate);
        words.insert(words.end(), std::make_move_iterator(pass.begin()),
                     std::make_move_iterator(pass.end()));
        if (!settings.verbatim) break;

        const int segmentCount = whisper_full_n_segments(ctx);
        // Whisper stops a window on the last token it decoded, not on the
        // window's nominal end. Resuming there is what keeps a sentence
        // whole; a window that decoded nothing usable falls back to a fixed
        // step so the loop always terminates.
        const int64_t decodedEnd =
            segmentCount > 0
                ? whisper_full_get_segment_t1(ctx, segmentCount - 1)
                : 0;
        cursorCentis = decodedEnd > cursorCentis
                           ? decodedEnd
                           : cursorCentis + kVerbatimWindowMs / 10;
        if (cursorCentis + 100 >= totalCentis) break;
        if (totalCentis > 0) {
            context.SetProgress(0.5 + 0.5 * static_cast<double>(cursorCentis) /
                                          static_cast<double>(totalCentis),
                                "Transcription");
        }
    }

    Transcript transcript;
    transcript.media_id = mediaId;
    transcript.whisper_model =
        std::filesystem::path(settings.whisper_model_path).filename().string();
    transcript.verbatim = settings.verbatim;
    transcript.source_rate = sourceRate;
    transcript.words = std::move(words);

    if (transcript.words.empty()) {
        error = "transcription produced no words";
        return false;
    }
    if (!NormalizeTranscriptWords(transcript.words, error, WordSource::Decoder))
        return false;
    context.SetProgress(1.0, "Transcript prêt");
    return SaveTranscript(outputPath, transcript, error);
}

}  // namespace

bool GenerateAudioTranscripts(const std::vector<TranscriptionJob>& jobs,
                              const WhisperSettings& settings,
                              MediaTaskContext& context,
                              std::vector<TranscriptionOutcome>& outcomes,
                              std::string& error) {
    error.clear();
    outcomes.clear();
    if (jobs.empty()) {
        error = "no media to transcribe";
        return false;
    }
    if (settings.whisper_model_path.empty()) {
        error = "a local whisper.cpp model path is required";
        return false;
    }
    whisper_context_params contextParams = whisper_context_default_params();
    whisper_context* ctx = whisper_init_from_file_with_params(
        settings.whisper_model_path.c_str(), contextParams);
    if (!ctx) {
        error = "unable to load local whisper.cpp model '" +
                settings.whisper_model_path + "'";
        return false;
    }
    // One media's failure is not the batch's: a rush that will not decode
    // should not cost the model load for the forty that would have. Each
    // outcome carries its own reason, and the caller decides what to do
    // with a partial result.
    for (const TranscriptionJob& job : jobs) {
        TranscriptionOutcome outcome;
        outcome.media_id = job.media_id;
        std::string jobError;
        outcome.ok = TranscribeOneWithModel(
            ctx, job.input_path, job.output_path, job.media_id, job.source_rate,
            settings, context, jobError);
        outcome.error = outcome.ok ? std::string() : jobError;
        if (outcome.ok) {
            Transcript written;
            std::string readError;
            if (LoadAudioTranscript(job.output_path, written, readError))
                outcome.words = written.words.size();
        }
        outcomes.push_back(std::move(outcome));
        if (context.Cancelled()) break;
    }
    whisper_free(ctx);
    return true;
}

bool GenerateAudioTranscript(const std::string& inputPath,
                             const std::string& outputPath,
                             const std::string& mediaId,
                             const MediaRate& sourceRate,
                             const WhisperSettings& settings,
                             MediaTaskContext& context, std::string& error) {
    std::vector<TranscriptionOutcome> outcomes;
    if (!GenerateAudioTranscripts(
            {{mediaId, inputPath, outputPath, sourceRate}}, settings, context,
            outcomes, error))
        return false;
    if (outcomes.empty()) {
        error = "transcription produced no outcome";
        return false;
    }
    error = outcomes.front().error;
    return outcomes.front().ok;
}

bool SaveAudioTranscript(const std::string& path, const Transcript& transcript,
                         std::string& error) {
    error.clear();
    return SaveTranscript(std::filesystem::path(path), transcript, error);
}

bool LoadAudioTranscript(const std::string& path, Transcript& transcript,
                         std::string& error) {
    error.clear();
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "unable to open transcript cache";
        return false;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (!input.good() && !input.eof()) {
        error = "unable to read transcript cache";
        return false;
    }
    try {
        // Reader stores a reference; keep the parsed text alive in a named
        // local for the reader's whole lifetime rather than binding it to
        // buffer.str()'s temporary, which would be destroyed at the end of
        // this statement.
        const std::string json = buffer.str();
        Reader reader(json);
        reader.Expect("{\"version\":1,\"media_id\":");
        Transcript parsed;
        parsed.media_id = reader.String();
        reader.Expect(",\"whisper_model\":");
        parsed.whisper_model = reader.String();
        if (reader.Consume(",\"verbatim\":"))
            parsed.verbatim = reader.Boolean();
        if (reader.Consume(",\"speech_aligned\":"))
            parsed.speech_aligned = reader.Boolean();
        reader.Expect(",\"source_rate\":{\"num\":");
        const int64_t num = reader.Integer();
        reader.Expect(",\"den\":");
        const int64_t den = reader.Integer();
        reader.Expect("},\"words\":[");
        if (num <= 0 || num > std::numeric_limits<int32_t>::max() || den <= 0 ||
            den > std::numeric_limits<int32_t>::max()) {
            throw std::runtime_error("invalid transcript source_rate");
        }
        parsed.source_rate = {static_cast<int32_t>(num),
                              static_cast<int32_t>(den)};
        if (!reader.Consume("]")) {
            while (true) {
                TranscriptWord word;
                reader.Expect("{\"text\":");
                word.text = reader.String();
                reader.Expect(",\"start\":");
                word.start = ReadTime(reader);
                reader.Expect(",\"end\":");
                word.end = ReadTime(reader);
                reader.Expect("}");
                parsed.words.push_back(std::move(word));
                if (reader.Consume("]")) break;
                reader.Expect(",");
            }
        }
        reader.Expect("}");
        reader.Finish();
        if (!NormalizeTranscriptWords(parsed.words, error)) return false;
        transcript = std::move(parsed);
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

namespace {

// Folds one transcript token down to the bare letters that identify it:
// lowercase, unaccented, punctuation and spacing dropped. Whisper glues
// punctuation to the word it follows ("partiel,") and now and then emits two
// words inside one token ("? Donc"), so comparing raw text would miss most
// repetitions. Only the Latin-1 accented letters French actually uses are
// folded; anything else is skipped rather than guessed at.
std::string FoldWordText(const std::string& text) {
    std::string folded;
    folded.reserve(text.size());
    for (size_t index = 0; index < text.size(); ++index) {
        const unsigned char byte = static_cast<unsigned char>(text[index]);
        if (byte >= 'A' && byte <= 'Z') {
            folded.push_back(static_cast<char>(byte - 'A' + 'a'));
            continue;
        }
        if (byte >= 'a' && byte <= 'z') {
            folded.push_back(static_cast<char>(byte));
            continue;
        }
        if (byte != 0xC3 || index + 1 >= text.size()) continue;
        const unsigned char accented =
            static_cast<unsigned char>(text[index + 1]) | 0x20;
        ++index;
        switch (accented) {
            case 0xA0:
            case 0xA1:
            case 0xA2:
            case 0xA3:
            case 0xA4:
            case 0xA5:
                folded.push_back('a');
                break;
            case 0xA7:
                folded.push_back('c');
                break;
            case 0xA8:
            case 0xA9:
            case 0xAA:
            case 0xAB:
                folded.push_back('e');
                break;
            case 0xAC:
            case 0xAD:
            case 0xAE:
            case 0xAF:
                folded.push_back('i');
                break;
            case 0xB2:
            case 0xB3:
            case 0xB4:
            case 0xB5:
            case 0xB6:
                folded.push_back('o');
                break;
            case 0xB9:
            case 0xBA:
            case 0xBB:
            case 0xBC:
                folded.push_back('u');
                break;
            default:
                break;
        }
    }
    return folded;
}

// "euuuh" and "euh" are the same hesitation held for different lengths, and
// whisper spells them both ways, so runs of one letter collapse before the
// lexicon is consulted.
std::string CollapseRepeatedLetters(const std::string& folded) {
    std::string collapsed;
    collapsed.reserve(folded.size());
    for (const char letter : folded)
        if (collapsed.empty() || collapsed.back() != letter)
            collapsed.push_back(letter);
    return collapsed;
}

// Deliberately short. Every entry here is a syllable that is not a French
// word, so removing it can never remove meaning. "eu" is absent on purpose:
// collapsing "euu" lands on it, but it is also the past participle of
// "avoir" -- the kind of false positive that would cut a real word out of a
// sentence.
bool IsFillerWord(const std::string& folded) {
    static const std::set<std::string> kFillers = {
        "euh", "heu", "hum", "hm", "mh", "ben", "bah", "beh", "hein"};
    return kFillers.count(CollapseRepeatedLetters(folded)) != 0;
}

}  // namespace

std::vector<Disfluency> FindDisfluencies(
    const std::vector<TranscriptWord>& words) {
    std::vector<std::string> folded;
    folded.reserve(words.size());
    for (const TranscriptWord& word : words)
        folded.push_back(FoldWordText(word.text));

    const auto textOf = [&](size_t from, size_t to) {
        std::string text;
        for (size_t index = from; index <= to; ++index) {
            if (!text.empty()) text.push_back(' ');
            text += words[index].text;
        }
        return text;
    };

    std::vector<Disfluency> found;
    size_t index = 0;
    while (index < words.size()) {
        if (folded[index].empty()) {
            ++index;
            continue;
        }
        if (IsFillerWord(folded[index])) {
            size_t last = index;
            while (last + 1 < words.size() && IsFillerWord(folded[last + 1]))
                ++last;
            found.push_back(
                {{index, last}, DisfluencyKind::Filler, textOf(index, last)});
            index = last + 1;
            continue;
        }
        // A stutter is the same word said twice in a row. The last occurrence
        // is the one kept: it is the one that runs into the word that
        // follows, so closing the cut before it leaves the sentence's own
        // rhythm intact.
        size_t last = index;
        while (last + 1 < words.size() && folded[last + 1] == folded[index])
            ++last;
        if (last > index) {
            found.push_back({{index, last - 1},
                             DisfluencyKind::Repetition,
                             textOf(index, last - 1)});
            index = last;
            continue;
        }
        ++index;
    }
    return found;
}

std::vector<Disfluency> FindDisfluenciesInClip(const DocumentClip& clip,
                                               const Transcript& transcript) {
    const RationalTime clipEnd = clip.source_in.add(clip.duration);
    std::vector<Disfluency> kept;
    for (Disfluency& item : FindDisfluencies(transcript.words)) {
        const TranscriptWord& first =
            transcript.words[item.range.start_word_index];
        const TranscriptWord& last =
            transcript.words[item.range.end_word_index];
        if (first.start < clip.source_in || clipEnd < last.end) continue;
        kept.push_back(std::move(item));
    }
    return kept;
}

bool ResolveWordRemoval(const DocumentClip& clip, const Transcript& transcript,
                        const std::vector<WordRange>& wordRanges,
                        const RationalTime& gapPadding,
                        const std::vector<Ulid>& syncTrackIds,
                        RemoveWordsOperation& operation, std::string& error) {
    error.clear();
    if (transcript.media_id != clip.source_id) {
        error = "transcript media_id does not match the clip's source_id";
        return false;
    }
    if (wordRanges.empty()) {
        error = "RemoveWords requires at least one word range";
        return false;
    }
    std::vector<WordRemovalRange> ranges;
    ranges.reserve(wordRanges.size());
    bool first = true;
    size_t previousEndIndex = 0;
    for (const WordRange& range : wordRanges) {
        if (range.start_word_index > range.end_word_index ||
            range.end_word_index >= transcript.words.size()) {
            error = "word range indices are out of bounds";
            return false;
        }
        if (!first && range.start_word_index <= previousEndIndex) {
            error = "word ranges must be sorted and non-overlapping";
            return false;
        }
        const TranscriptWord& firstWord =
            transcript.words[range.start_word_index];
        const TranscriptWord& lastWord = transcript.words[range.end_word_index];
        ranges.push_back({firstWord.start, lastWord.end});
        previousEndIndex = range.end_word_index;
        first = false;
    }
    // linked_clip_ids stays empty here: this function resolves *which
    // frames*, and which other clips share the cut is a document-shape
    // question its caller answers (see McpTools.cc's clean_disfluencies,
    // which fills it from the clip's link group).
    operation = RemoveWordsOperation{clip.id, std::move(ranges), gapPadding,
                                     {},      syncTrackIds,      {}};
    return true;
}

bool ResolveConfiguredWhisperModel(std::string& path, std::string& reason) {
    const std::string configured = local_env::Value("CUTMACHINE_WHISPER_MODEL");
    const std::string file = local_env::LocalEnvFilePath();
    if (configured.empty()) {
        reason =
            "no Whisper model configured: set CUTMACHINE_WHISPER_MODEL "
            "to a local ggml model file, either in the environment or in "
            "'" +
            file + "'";
        return false;
    }
    if (!std::filesystem::is_regular_file(configured)) {
        reason = "CUTMACHINE_WHISPER_MODEL does not name a regular file: '" +
                 configured + "' (configured in '" + file + "')";
        return false;
    }
    path = configured;
    reason.clear();
    return true;
}
