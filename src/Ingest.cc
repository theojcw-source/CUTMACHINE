#include "Ingest.h"

#include "Document.h"
#include "ProjectStorage.h"
#include "SpeechOnset.h"
#include "Ulid.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavformat/version_major.h>
#include <libavutil/display.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixdesc.h>
}

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

extern char** environ;

namespace {

// 8 kHz mono is enough for a level: the measurement is an energy average,
// not a spectrum, and a lower rate is a shorter decode. The rate does not
// change the answer beyond rounding, so it is not recorded anywhere.
constexpr uint32_t kLevelSampleRate = 8000;

struct IngestError {
    std::string file;
    std::string reason;
};

std::string EscapeJson(const std::string& input) {
    std::ostringstream output;
    for (const unsigned char character : input) {
        switch (character) {
            case '"':
                output << "\\\"";
                break;
            case '\\':
                output << "\\\\";
                break;
            case '\b':
                output << "\\b";
                break;
            case '\f':
                output << "\\f";
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
                if (character < 0x20) {
                    const char digits[] = "0123456789abcdef";
                    output << "\\u00" << digits[character >> 4]
                           << digits[character & 0xf];
                } else {
                    output << static_cast<char>(character);
                }
        }
    }
    return output.str();
}

std::string AvError(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

std::filesystem::path Resolved(const std::filesystem::path& path,
                               std::error_code& error) {
    std::filesystem::path absolute = std::filesystem::absolute(path, error);
    if (error) return {};
    return std::filesystem::weakly_canonical(absolute, error);
}

bool ProbeImpl(const std::filesystem::path& absolutePath, LibraryMedia& media,
               std::string& reason) {
    AVFormatContext* rawContext = nullptr;
    int result = avformat_open_input(&rawContext, absolutePath.c_str(), nullptr,
                                     nullptr);
    if (result < 0) {
        reason = "unable to open media: " + AvError(result);
        return false;
    }
    struct ContextCloser {
        void operator()(AVFormatContext* context) const {
            avformat_close_input(&context);
        }
    };
    std::unique_ptr<AVFormatContext, ContextCloser> context(rawContext);
    result = avformat_find_stream_info(context.get(), nullptr);
    if (result < 0) {
        reason = "unable to read stream headers: " + AvError(result);
        return false;
    }

    const int videoIndex = av_find_best_stream(
        context.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (videoIndex < 0) {
        reason = "no video stream";
        return false;
    }
    AVStream* video = context->streams[videoIndex];
    const AVCodecParameters* parameters = video->codecpar;
    if (parameters->width <= 0 || parameters->height <= 0) {
        reason = "video stream has invalid dimensions";
        return false;
    }
    const AVRational frameRate = video->avg_frame_rate;
    if (frameRate.num <= 0 || frameRate.den <= 0) {
        reason = "video stream has no valid avg_frame_rate";
        return false;
    }

    int64_t duration = 0;
    if (video->duration != AV_NOPTS_VALUE && video->duration > 0) {
        duration = av_rescale_q_rnd(
            video->duration, video->time_base, AVRational{1, frameRate.num},
            static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
    } else if (context->duration != AV_NOPTS_VALUE && context->duration > 0) {
        duration = av_rescale_q_rnd(
            context->duration, AVRational{1, AV_TIME_BASE},
            AVRational{1, frameRate.num},
            static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
    }
    if (duration <= 0) {
        reason = "video stream has no positive duration";
        return false;
    }

    double rotation = 0.0;
    const uint8_t* displayMatrix = nullptr;
    size_t displayMatrixSize = 0;
#if LIBAVFORMAT_VERSION_MAJOR >= 62
    const AVPacketSideData* matrixSideData = av_packet_side_data_get(
        parameters->coded_side_data, parameters->nb_coded_side_data,
        AV_PKT_DATA_DISPLAYMATRIX);
    if (matrixSideData) {
        displayMatrix = matrixSideData->data;
        displayMatrixSize = matrixSideData->size;
    }
#else
    displayMatrix = av_stream_get_side_data(video, AV_PKT_DATA_DISPLAYMATRIX,
                                            &displayMatrixSize);
#endif
    if (displayMatrix && displayMatrixSize >= 9 * sizeof(int32_t)) {
        const double value = av_display_rotation_get(
            reinterpret_cast<const int32_t*>(displayMatrix));
        if (!std::isnan(value)) rotation = value;
    }
    media.rotation_degrees = static_cast<int32_t>(std::lround(rotation));
    const double radians =
        media.rotation_degrees * 3.14159265358979323846 / 180.0;
    const double displayedWidth =
        std::abs(parameters->width * std::cos(radians)) +
        std::abs(parameters->height * std::sin(radians));
    const double displayedHeight =
        std::abs(parameters->width * std::sin(radians)) +
        std::abs(parameters->height * std::cos(radians));

    media.codec = avcodec_get_name(parameters->codec_id);
    media.width = parameters->width;
    media.height = parameters->height;
    const char* pixelFormat =
        av_get_pix_fmt_name(static_cast<AVPixelFormat>(parameters->format));
    const char* colorRange = av_color_range_name(parameters->color_range);
    const char* colorSpace = av_color_space_name(parameters->color_space);
    const char* colorTransfer = av_color_transfer_name(parameters->color_trc);
    const char* colorPrimaries =
        av_color_primaries_name(parameters->color_primaries);
    media.pixel_format = pixelFormat ? pixelFormat : "unknown";
    media.color_range = colorRange ? colorRange : "unknown";
    media.color_space = colorSpace ? colorSpace : "unknown";
    media.color_transfer = colorTransfer ? colorTransfer : "unknown";
    media.color_primaries = colorPrimaries ? colorPrimaries : "unknown";
    media.rate = {frameRate.num, frameRate.den};
    media.duration = {duration, frameRate.num};
    const double scale = std::max(displayedWidth, displayedHeight);
    if (std::abs(displayedWidth - displayedHeight) <= scale * 1e-9) {
        media.orientation = "square";
    } else {
        media.orientation =
            displayedWidth > displayedHeight ? "landscape" : "portrait";
    }

    const int audioIndex = av_find_best_stream(
        context.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audioIndex >= 0) {
        const AVCodecParameters* audio = context->streams[audioIndex]->codecpar;
        if (audio->sample_rate > 0 && audio->ch_layout.nb_channels > 0) {
            media.has_audio = true;
            media.audio_rate = audio->sample_rate;
            media.audio_channels = audio->ch_layout.nb_channels;
        }
    }
    return true;
}

// Decodes the first audio stream to mono 16-bit PCM on a pipe and folds it
// into one RMS figure. The same posix_spawnp shape Waveform.cc and
// SpeechOnset.cc use, without their cancellation plumbing: an ingest is a
// short synchronous command, not a background task a user can interrupt.
bool MeasureAudioLevelImpl(const std::filesystem::path& path,
                           const std::string& ffmpegPath, int64_t& level,
                           std::string& reason) {
    level = 0;
    reason.clear();
    const std::string rate = std::to_string(kLevelSampleRate);
    std::vector<std::string> storage = {
        ffmpegPath, "-hide_banner", "-loglevel", "error", "-nostdin",
        "-i",       path.string(),  "-map",      "0:a:0", "-vn",
        "-sn",      "-ac",          "1",         "-ar",   rate,
        "-f",       "s16le",        "pipe:1",
    };
    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (std::string& value : storage) argv.push_back(value.data());
    argv.push_back(nullptr);

    int audioPipe[2] = {-1, -1};
    if (pipe(audioPipe) != 0) {
        reason = "unable to create audio level pipe: " +
                 std::string(std::strerror(errno));
        return false;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, audioPipe[1], STDOUT_FILENO);
    // FFmpeg's diagnostics are not this function's business: a media with no
    // audio stream is an expected outcome, not an error to relay.
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null",
                                     O_WRONLY, 0);
    posix_spawn_file_actions_addclose(&actions, audioPipe[0]);
    posix_spawn_file_actions_addclose(&actions, audioPipe[1]);
    pid_t process = 0;
    const int spawnResult =
        posix_spawnp(&process, storage.front().c_str(), &actions, nullptr,
                     argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(audioPipe[1]);
    if (spawnResult != 0) {
        close(audioPipe[0]);
        reason = "unable to start FFmpeg: " +
                 std::string(std::strerror(spawnResult));
        return false;
    }

    RunningRmsLevel running;
    std::vector<char> pending;
    char buffer[65536];
    ssize_t got = 0;
    while ((got = read(audioPipe[0], buffer, sizeof(buffer))) > 0) {
        pending.insert(pending.end(), buffer, buffer + got);
        // Whole samples only: a read can split one in half, and interpreting
        // the halves would make the figure depend on buffer boundaries.
        const size_t whole =
            (pending.size() / sizeof(int16_t)) * sizeof(int16_t);
        if (whole > 0) {
            running.Add(reinterpret_cast<const int16_t*>(pending.data()),
                        whole / sizeof(int16_t));
            pending.erase(pending.begin(),
                          pending.begin() + static_cast<ptrdiff_t>(whole));
        }
    }
    close(audioPipe[0]);
    int status = 0;
    while (waitpid(process, &status, 0) < 0 && errno == EINTR) {
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        reason = "FFmpeg could not decode an audio stream";
        return false;
    }
    if (running.Samples() == 0) {
        reason = "media decoded no audio samples";
        return false;
    }
    level = running.Level();
    return true;
}

bool CollectFiles(const std::filesystem::path& directory, bool recursive,
                  std::vector<std::filesystem::path>& files,
                  std::string& reason) {
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error) || error) {
        reason = error ? error.message() : "path is not a directory";
        return false;
    }
    const auto options = std::filesystem::directory_options::none;
    if (recursive) {
        std::filesystem::recursive_directory_iterator iterator(directory,
                                                               options, error),
            end;
        if (error) {
            reason = error.message();
            return false;
        }
        for (; iterator != end; iterator.increment(error)) {
            if (error) {
                reason = error.message();
                return false;
            }
            if (iterator->is_regular_file(error))
                files.push_back(iterator->path());
            if (error) {
                reason = error.message();
                return false;
            }
        }
    } else {
        std::filesystem::directory_iterator iterator(directory, options, error),
            end;
        if (error) {
            reason = error.message();
            return false;
        }
        for (; iterator != end; iterator.increment(error)) {
            if (error) {
                reason = error.message();
                return false;
            }
            if (iterator->is_regular_file(error))
                files.push_back(iterator->path());
            if (error) {
                reason = error.message();
                return false;
            }
        }
    }
    std::sort(files.begin(), files.end());
    return true;
}

// Fills in the mean level of an entry that has just been probed. A failure
// to measure is deliberately not an ingest failure and not even an error
// entry: the file is perfectly usable without the figure, and
// `audio_level_measured` already says the level is unknown. Re-ingesting the
// same path fills it in later, exactly as it does for a v1 entry with no
// technical metadata at all.
void MeasureIngestedAudioLevel(const std::filesystem::path& absolute,
                               LibraryMedia& media) {
    if (!media.has_audio) return;
    int64_t level = 0;
    std::string levelReason;
    if (!MeasureMediaAudioLevel(absolute.string(), "ffmpeg", level,
                                levelReason))
        return;
    media.audio_level_measured = true;
    media.audio_level = level;
}

std::string ResultJson(bool ok, size_t added, size_t skipped,
                       const std::vector<IngestError>& errors) {
    std::ostringstream output;
    output << "{\"ok\":" << (ok ? "true" : "false") << ",\"added\":" << added
           << ",\"skipped\":" << skipped << ",\"errors\":[";
    for (size_t index = 0; index < errors.size(); ++index) {
        if (index) output << ',';
        output << "{\"file\":\"" << EscapeJson(errors[index].file)
               << "\",\"reason\":\"" << EscapeJson(errors[index].reason)
               << "\"}";
    }
    output << "]}\n";
    return output.str();
}

}  // namespace

bool ProbeMediaMetadata(const std::string& path, LibraryMedia& media,
                        std::string& reason) {
    av_log_set_level(AV_LOG_ERROR);
    return ProbeImpl(std::filesystem::path(path), media, reason);
}

bool MeasureMediaAudioLevel(const std::string& path,
                            const std::string& ffmpegPath, int64_t& level,
                            std::string& reason) {
    return MeasureAudioLevelImpl(std::filesystem::path(path), ffmpegPath, level,
                                 reason);
}

int IngestCommand(const std::string& documentPath,
                  const std::string& directoryPath, bool recursive,
                  std::string& output) {
    av_log_set_level(AV_LOG_ERROR);
    Project project;
    std::string reason;
    if (!LoadStoredProject(documentPath, project, reason)) {
        output = ResultJson(false, 0, 0, {{documentPath, reason}});
        return 1;
    }
    Document document = project.MakeActiveDocument();

    std::error_code pathError;
    const std::filesystem::path resolvedDocument =
        Resolved(documentPath, pathError);
    if (pathError) {
        output = ResultJson(false, 0, 0, {{documentPath, pathError.message()}});
        return 1;
    }
    const std::filesystem::path resolvedDirectory =
        Resolved(directoryPath, pathError);
    if (pathError) {
        output =
            ResultJson(false, 0, 0, {{directoryPath, pathError.message()}});
        return 1;
    }
    std::vector<std::filesystem::path> files;
    if (!CollectFiles(resolvedDirectory, recursive, files, reason)) {
        output = ResultJson(false, 0, 0, {{directoryPath, reason}});
        return 1;
    }

    std::map<std::string, size_t> knownPaths;
    for (size_t index = 0; index < document.library.size(); ++index) {
        const LibraryMedia& media = document.library[index];
        std::filesystem::path stored(media.path);
        if (stored.is_relative())
            stored = resolvedDocument.parent_path() / stored;
        std::error_code error;
        const std::filesystem::path resolved = Resolved(stored, error);
        if (!error) knownPaths.emplace(resolved.string(), index);
    }

    size_t added = 0;
    size_t skipped = 0;
    bool changed = false;
    std::vector<IngestError> errors;
    for (const std::filesystem::path& candidate : files) {
        std::error_code error;
        const std::filesystem::path absolute = Resolved(candidate, error);
        if (error) {
            ++skipped;
            errors.push_back({candidate.filename().string(), error.message()});
            continue;
        }
        const auto known = knownPaths.find(absolute.string());
        if (known != knownPaths.end()) {
            ++skipped;
            LibraryMedia& existing = document.library[known->second];
            if (!existing.metadata_complete) {
                LibraryMedia enriched;
                enriched.id = existing.id;
                enriched.path = existing.path;
                enriched.filename = absolute.filename().string();
                if (ProbeMediaMetadata(absolute.string(), enriched, reason)) {
                    MeasureIngestedAudioLevel(absolute, enriched);
                    existing = std::move(enriched);
                    changed = true;
                } else {
                    errors.push_back({absolute.filename().string(), reason});
                }
            }
            if (existing.metadata_complete &&
                !document.FindSource(existing.id)) {
                document.sources.push_back({existing.id, existing.path,
                                            existing.rate, existing.duration});
                changed = true;
            }
            continue;
        }
        LibraryMedia media;
        media.id = GenerateUlid();
        media.filename = absolute.filename().string();
        media.path = std::filesystem::relative(
                         absolute, resolvedDocument.parent_path(), error)
                         .lexically_normal()
                         .string();
        if (error) media.path = absolute.string();
        if (!ProbeMediaMetadata(absolute.string(), media, reason)) {
            ++skipped;
            errors.push_back({media.filename, reason});
            continue;
        }
        MeasureIngestedAudioLevel(absolute, media);
        document.library.push_back(std::move(media));
        const LibraryMedia& addedMedia = document.library.back();
        if (!document.FindSource(addedMedia.id)) {
            document.sources.push_back({addedMedia.id, addedMedia.path,
                                        addedMedia.rate, addedMedia.duration});
        }
        knownPaths.emplace(absolute.string(), document.library.size() - 1);
        ++added;
        changed = true;
    }

    if (!document.Validate(reason)) {
        output = ResultJson(false, added, skipped, {{documentPath, reason}});
        return 1;
    }
    if (changed) {
        if (!project.CommitActiveDocument(document, reason) ||
            !CommitStoredProject(documentPath, project, reason)) {
            output =
                ResultJson(false, added, skipped, {{documentPath, reason}});
            return 1;
        }
    }
    output = ResultJson(true, added, skipped, errors);
    return 0;
}
