#include "Transcription.h"

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
#include <limits>
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
bool NormalizeTranscriptWords(std::vector<TranscriptWord>& words,
                              std::string& error) {
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
            error = "transcript words are not in chronological order";
            return false;
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

bool GenerateAudioTranscript(const std::string& inputPath,
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
    if (settings.whisper_model_path.empty()) {
        error = "a local whisper.cpp model path is required";
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

    whisper_context_params contextParams = whisper_context_default_params();
    whisper_context* ctx = whisper_init_from_file_with_params(
        settings.whisper_model_path.c_str(), contextParams);
    if (!ctx) {
        error = "unable to load local whisper.cpp model '" +
                settings.whisper_model_path + "'";
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
    params.language =
        settings.language.empty() ? "auto" : settings.language.c_str();
    params.n_threads =
        static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));

    const int result = whisper_full(ctx, params, samples.data(),
                                    static_cast<int>(samples.size()));
    if (result != 0) {
        whisper_free(ctx);
        error = "local whisper.cpp inference failed with code " +
                std::to_string(result);
        return false;
    }
    if (context.Cancelled()) {
        whisper_free(ctx);
        error = "transcription cancelled";
        return false;
    }

    Transcript transcript;
    transcript.media_id = mediaId;
    transcript.whisper_model =
        std::filesystem::path(settings.whisper_model_path).filename().string();
    transcript.source_rate = sourceRate;
    transcript.words = GroupWordsFromWhisper(ctx, sourceRate);
    whisper_free(ctx);

    if (transcript.words.empty()) {
        error = "transcription produced no words";
        return false;
    }
    if (!NormalizeTranscriptWords(transcript.words, error)) return false;
    context.SetProgress(1.0, "Transcript prêt");
    return SaveTranscript(outputPath, transcript, error);
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
    operation = RemoveWordsOperation{
        clip.id, std::move(ranges), gapPadding, syncTrackIds, {}};
    return true;
}
