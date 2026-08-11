#include "Export.h"

#include "Ingest.h"
#include "ColorManagement.h"
#include "Timeline.h"
#include "Ulid.h"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <poll.h>
#include <spawn.h>
#include <sstream>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace {

std::string Decimal(const RationalTime& time) {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::fixed << std::setprecision(12)
           << static_cast<long double>(time.value) / time.rate;
    std::string value = output.str();
    while (value.size() > 2 && value.back() == '0') value.pop_back();
    if (!value.empty() && value.back() == '.') value.push_back('0');
    return value;
}

std::string FilterSeconds(const RationalTime& time) {
    return "(" + std::to_string(time.value) + "/" +
           std::to_string(time.rate) + ")";
}

int64_t CeilFrames(const RationalTime& duration, MediaRate rate) {
    const __int128 numerator =
        static_cast<__int128>(duration.value) * rate.num;
    const __int128 denominator =
        static_cast<__int128>(duration.rate) * rate.den;
    const __int128 frames = (numerator + denominator - 1) / denominator;
    if (frames > std::numeric_limits<int64_t>::max())
        throw std::overflow_error("export frame count overflows int64_t");
    return static_cast<int64_t>(frames);
}

std::filesystem::path ResolveMediaPath(
    const std::filesystem::path& baseDirectory,
    const DocumentSource& source) {
    std::filesystem::path path(source.path);
    if (path.is_relative()) path = baseDirectory / path;
    return std::filesystem::absolute(path).lexically_normal();
}

std::string TemporaryOutputPath(const std::filesystem::path& destination) {
    const std::string filename = destination.stem().string() + ".cutmachine-" +
                                 GenerateUlid() + ".part" +
                                 destination.extension().string();
    return (destination.parent_path() / filename).string();
}

std::string BuildColorLut(const ColorManagementSettings& settings) {
    constexpr int kSize = 65;
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << "TITLE \"CUTMACHINE display transform\"\n"
           << "LUT_3D_SIZE " << kSize << "\n"
           << "DOMAIN_MIN 0.0 0.0 0.0\n"
           << "DOMAIN_MAX 1.0 1.0 1.0\n"
           << std::fixed << std::setprecision(10);
    // .cube ordering increments red first, then green, then blue.
    for (int blue = 0; blue < kSize; ++blue) {
        for (int green = 0; green < kSize; ++green) {
            for (int red = 0; red < kSize; ++red) {
                const RgbColor transformed = TransformColorForOutput(
                    settings,
                    {static_cast<double>(red) / (kSize - 1),
                     static_cast<double>(green) / (kSize - 1),
                     static_cast<double>(blue) / (kSize - 1)});
                output << transformed.red << ' ' << transformed.green << ' '
                       << transformed.blue << '\n';
            }
        }
    }
    return output.str();
}

void AppendTail(std::string& destination, const char* bytes, size_t count) {
    constexpr size_t kMaximumErrorBytes = 32768;
    destination.append(bytes, count);
    if (destination.size() > kMaximumErrorBytes)
        destination.erase(0, destination.size() - kMaximumErrorBytes);
}

bool ParseProgressLine(std::string_view line, ExportProgress& progress) {
    const size_t separator = line.find('=');
    if (separator == std::string_view::npos) return false;
    const std::string key(line.substr(0, separator));
    const std::string value(line.substr(separator + 1));
    try {
        if (key == "frame")
            progress.rendered_frames = std::stoll(value);
        else if (key == "out_time_us")
            progress.output_time_us = std::stoll(value);
        else if (key == "speed") {
            std::string number = value;
            if (!number.empty() && number.back() == 'x') number.pop_back();
            progress.speed = std::stod(number);
        }
    } catch (const std::exception&) {
        // Progress is advisory. A malformed line must not fail the render.
    }
    if (progress.total_frames > 0) {
        progress.fraction = std::clamp(
            static_cast<double>(progress.rendered_frames) /
                progress.total_frames,
            0.0, 1.0);
    }
    return key == "progress";
}

}  // namespace

const std::vector<ExportPresetDescriptor>& Exporter::Presets() {
    static const std::vector<ExportPresetDescriptor> presets = {
        {ExportPresetId::HevcHighQuality, "H.265 Main10 — Haute qualité",
         "Format de séquence, encodage matériel rapide"},
        {ExportPresetId::HevcMaster, "H.265 Main10 — Master x265",
         "Format de séquence, qualité constante CRF 18"},
        {ExportPresetId::HevcWeb1080, "H.265 Main10 — Web 1080p",
         "1920×1080, cadence de séquence, 20 Mbit/s"},
        {ExportPresetId::HevcUhdDelivery, "H.265 Main10 — Livraison UHD",
         "3840×2160, cadence de séquence, 80 Mbit/s"},
    };
    return presets;
}

ExportSettings Exporter::SettingsForPreset(ExportPresetId preset,
                                            const std::string& outputPath,
                                            int32_t sourceWidth,
                                            int32_t sourceHeight,
                                            MediaRate sourceRate) {
    ExportSettings settings;
    settings.output_path = outputPath;
    settings.width = sourceWidth > 0 ? sourceWidth : 1920;
    settings.height = sourceHeight > 0 ? sourceHeight : 1080;
    // Main10 4:2:0 requires even output dimensions.
    settings.width += settings.width % 2;
    settings.height += settings.height % 2;
    if (sourceRate.num > 0 && sourceRate.den > 0)
        settings.frame_rate = sourceRate;
    settings.encoder = ExportEncoder::HevcVideoToolbox;
    settings.video_bitrate = 45000000;
    settings.main10 = true;

    switch (preset) {
        case ExportPresetId::HevcHighQuality: {
            const long double relativeLoad =
                static_cast<long double>(settings.width) * settings.height *
                settings.frame_rate.num /
                (static_cast<long double>(1920) * 1080 * 25 *
                 settings.frame_rate.den);
            settings.video_bitrate = static_cast<int64_t>(std::clamp(
                45000000.0L * relativeLoad, 12000000.0L, 120000000.0L));
            break;
        }
        case ExportPresetId::HevcMaster:
            settings.encoder = ExportEncoder::HevcSoftware;
            break;
        case ExportPresetId::HevcWeb1080:
            settings.width = 1920;
            settings.height = 1080;
            settings.video_bitrate = 20000000;
            break;
        case ExportPresetId::HevcUhdDelivery:
            settings.width = 3840;
            settings.height = 2160;
            settings.video_bitrate = 80000000;
            break;
    }
    return settings;
}

ExportSettings Exporter::HevcMain10Preset(const std::string& outputPath) {
    return SettingsForPreset(ExportPresetId::HevcHighQuality, outputPath, 1920,
                             1080, {25, 1});
}

ExportSettings Exporter::HevcMain10SoftwarePreset(
    const std::string& outputPath) {
    ExportSettings settings = HevcMain10Preset(outputPath);
    settings.encoder = ExportEncoder::HevcSoftware;
    return settings;
}

bool Exporter::BuildPlan(const Document& document,
                         const std::string& documentPath,
                         const ExportSettings& settings, ExportPlan& output,
                         std::string& error) {
    error.clear();
    output = {};
    if (settings.output_path.empty()) {
        error = "export destination is empty";
        return false;
    }
    if (settings.width < 2 || settings.height < 2 || settings.width > 16384 ||
        settings.height > 16384 || settings.width % 2 || settings.height % 2) {
        error = "export dimensions must be even and between 2 and 16384";
        return false;
    }
    if (settings.frame_rate.num <= 0 || settings.frame_rate.den <= 0 ||
        settings.frame_rate.num > 240000 || settings.frame_rate.den > 10000) {
        error = "export frame rate is invalid";
        return false;
    }
    if (settings.video_bitrate <= 0 || settings.audio_bitrate <= 0 ||
        settings.audio_sample_rate <= 0) {
        error = "export bitrate and audio sample rate must be positive";
        return false;
    }
    const std::filesystem::path destination =
        std::filesystem::absolute(settings.output_path).lexically_normal();
    const std::string extension = destination.extension().string();
    if (extension != ".mp4" && extension != ".MP4" && extension != ".mov" &&
        extension != ".MOV") {
        error = "HEVC export destination must use .mp4 or .mov";
        return false;
    }
    std::error_code filesystemError;
    if (!std::filesystem::exists(destination.parent_path(), filesystemError) ||
        filesystemError) {
        error = "export destination directory does not exist";
        return false;
    }
    if (!settings.overwrite &&
        std::filesystem::exists(destination, filesystemError)) {
        error = "export destination already exists";
        return false;
    }
    if (filesystemError) {
        error = "unable to inspect export destination: " +
                filesystemError.message();
        return false;
    }

    RationalTime duration;
    try {
        duration = Timeline(document).Duration();
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
    if (duration.value <= 0) {
        error = "cannot export an empty timeline";
        return false;
    }

    const std::filesystem::path baseDirectory =
        std::filesystem::absolute(documentPath).parent_path();
    for (const DocumentSource& source : document.sources) {
        const std::filesystem::path path = ResolveMediaPath(baseDirectory, source);
        if (!std::filesystem::is_regular_file(path, filesystemError) ||
            filesystemError) {
            error = "source media is missing: " + path.string();
            return false;
        }
        if (path == destination) {
            error = "export destination cannot replace source media";
            return false;
        }
    }

    std::map<Ulid, bool> sourceHasAudio;
    for (const DocumentSource& source : document.sources) {
        const LibraryMedia* media = document.FindLibraryMedia(source.id);
        if (media && media->metadata_complete) {
            sourceHasAudio[source.id] = media->has_audio;
            continue;
        }
        LibraryMedia detected;
        std::string probeError;
        if (!ProbeMediaMetadata(
                ResolveMediaPath(baseDirectory, source).string(), detected,
                probeError)) {
            error = "unable to probe source media for export: " + probeError;
            return false;
        }
        sourceHasAudio[source.id] = detected.has_audio;
    }

    struct InputClip {
        const DocumentTrack* track = nullptr;
        const DocumentClip* clip = nullptr;
        const DocumentSource* source = nullptr;
        int input_index = -1;
        bool video = false;
        bool audio = false;
    };
    std::vector<const DocumentTrack*> tracks;
    for (const DocumentTrack& track : document.sequence.tracks) tracks.push_back(&track);
    std::stable_sort(tracks.begin(), tracks.end(),
                     [](const DocumentTrack* left,
                        const DocumentTrack* right) {
                         return left->index < right->index;
                     });

    std::vector<InputClip> inputs;
    for (const DocumentTrack* track : tracks) {
        for (const DocumentClip& clip : track->clips) {
            const DocumentSource* source = document.FindSource(clip.source_id);
            if (!source) {
                error = "clip references an unknown source";
                return false;
            }
            const bool video = track->kind == "video";
            const bool audio =
                sourceHasAudio[clip.source_id] &&
                (track->kind == "audio" || clip.include_audio);
            if (!video && !audio) continue;
            inputs.push_back(
                {track, &clip, source, static_cast<int>(inputs.size()), video,
                 audio});
        }
    }

    std::vector<std::string> arguments = {
        "-hide_banner", "-nostdin", "-y", "-progress", "pipe:1",
        "-nostats",
    };
    for (const InputClip& input : inputs) {
        arguments.push_back("-ss");
        arguments.push_back(Decimal(input.clip->source_in));
        arguments.push_back("-t");
        arguments.push_back(Decimal(input.clip->duration));
        arguments.push_back("-i");
        arguments.push_back(
            ResolveMediaPath(baseDirectory, *input.source).string());
    }

    const std::string rate = std::to_string(settings.frame_rate.num) + "/" +
                             std::to_string(settings.frame_rate.den);
    std::ostringstream graph;
    graph.imbue(std::locale::classic());
    graph << "color=c=black:s=" << settings.width << 'x' << settings.height
          << ":r=" << rate << ":d=" << Decimal(duration)
          << ",format=yuv444p10le[base0];";

    size_t videoOrdinal = 0;
    for (const InputClip& input : inputs) {
        if (!input.video) continue;
        const std::string position = FilterSeconds(input.clip->timeline_in);
        const std::string clipDuration = FilterSeconds(input.clip->duration);
        graph << '[' << input.input_index << ":v:0]fps=" << rate
              << ",scale=" << settings.width << ':' << settings.height
              << ":force_original_aspect_ratio=decrease:flags=lanczos"
              << ",setsar=1,setpts=PTS-STARTPTS+" << position
              << "/TB[v" << videoOrdinal << "];"
              << "[base" << videoOrdinal << "][v" << videoOrdinal
              << "]overlay=x=(W-w)/2:y=(H-h)/2:eof_action=pass:repeatlast=0:"
                 "shortest=0:enable=between(t\\,"
              << position << "\\," << position << '+' << clipDuration
              << ")[base" << (videoOrdinal + 1) << "];";
        ++videoOrdinal;
    }
    graph << "[base" << videoOrdinal << ']';
    if (document.color_management.enabled) {
        output.temporary_lut_path =
            (std::filesystem::temp_directory_path() /
             ("cutmachine-" + GenerateUlid() + ".cube"))
                .string();
        output.color_lut = BuildColorLut(document.color_management);
        graph << "scale=";
        bool hasInputOption = false;
        if (document.color_management.input_range != "auto") {
            graph << "in_range="
                  << (document.color_management.input_range == "full" ? "full"
                                                                       : "tv");
            hasInputOption = true;
        }
        if (document.color_management.input_ycbcr_matrix != "auto") {
            if (hasInputOption) graph << ':';
            graph << "in_color_matrix="
                  << (document.color_management.input_ycbcr_matrix ==
                              "bt2020_ncl"
                          ? "bt2020"
                          : "bt709");
            hasInputOption = true;
        }
        if (hasInputOption) graph << ':';
        graph << "out_range=full,format=gbrp16le,lut3d=file='"
              << output.temporary_lut_path
              << "':interp=tetrahedral,scale=out_range=tv:out_color_matrix="
              << (document.color_management.output_gamut == "rec2020"
                      ? "bt2020"
                      : "bt709")
              << ',';
    }
    graph << "format=" << (settings.main10 ? "yuv420p10le" : "yuv420p")
          << ",setparams=range=limited:color_primaries="
          << (document.color_management.enabled &&
                      document.color_management.output_gamut == "rec2020"
                  ? "bt2020"
                  : "bt709")
          << ":color_trc="
          << (document.color_management.enabled &&
                      document.color_management.output_transfer == "hlg"
                  ? "arib-std-b67"
                  : "bt709")
          << ":colorspace="
          << (document.color_management.enabled &&
                      document.color_management.output_gamut == "rec2020"
                  ? "bt2020nc"
                  : "bt709")
          << "[video];";

    size_t audioOrdinal = 0;
    for (const InputClip& input : inputs) {
        if (!input.audio) continue;
        const __int128 sampleNumerator =
            static_cast<__int128>(input.clip->timeline_in.value) *
            settings.audio_sample_rate;
        const __int128 sampleDenominator = input.clip->timeline_in.rate;
        const int64_t delaySamples = static_cast<int64_t>(
            (sampleNumerator + sampleDenominator / 2) / sampleDenominator);
        graph << '[' << input.input_index << ":a:0]atrim=duration="
              << Decimal(input.clip->duration)
              << ",asetpts=PTS-STARTPTS,aresample="
              << settings.audio_sample_rate << ",aformat=sample_fmts=fltp:"
                 "channel_layouts=stereo,adelay="
              << delaySamples << "S:all=1[a" << audioOrdinal << "];";
        ++audioOrdinal;
    }
    if (audioOrdinal == 0) {
        graph << "anullsrc=r=" << settings.audio_sample_rate
              << ":cl=stereo:d=" << Decimal(duration) << "[audio]";
    } else {
        for (size_t index = 0; index < audioOrdinal; ++index)
            graph << "[a" << index << ']';
        graph << "amix=inputs=" << audioOrdinal
              << ":duration=longest:normalize=0,alimiter=limit=1:latency=1,"
                 "apad,atrim="
              << "duration=" << Decimal(duration) << "[audio]";
    }

    arguments.insert(arguments.end(), {"-filter_complex", graph.str(),
                                       "-map", "[video]", "-map", "[audio]"});
    if (settings.encoder == ExportEncoder::HevcVideoToolbox) {
        arguments.insert(arguments.end(),
                         {"-c:v", "hevc_videotoolbox", "-profile:v",
                          settings.main10 ? "main10" : "main", "-b:v",
                          std::to_string(settings.video_bitrate), "-allow_sw",
                          "1"});
    } else {
        arguments.insert(arguments.end(),
                         {"-c:v", "libx265", "-profile:v",
                          settings.main10 ? "main10" : "main", "-preset",
                          "medium", "-crf", "18"});
    }
    arguments.insert(arguments.end(),
                     {"-pix_fmt", settings.main10 ? "yuv420p10le" : "yuv420p",
                      "-tag:v", "hvc1", "-color_primaries",
                      document.color_management.enabled &&
                              document.color_management.output_gamut == "rec2020"
                          ? "bt2020"
                          : "bt709",
                      "-color_trc",
                      document.color_management.enabled &&
                              document.color_management.output_transfer == "hlg"
                          ? "arib-std-b67"
                          : "bt709",
                      "-colorspace",
                      document.color_management.enabled &&
                              document.color_management.output_gamut == "rec2020"
                          ? "bt2020nc"
                          : "bt709",
                      "-color_range", "tv", "-c:a", "aac", "-b:a",
                      std::to_string(settings.audio_bitrate), "-ar",
                      std::to_string(settings.audio_sample_rate), "-movflags",
                      "+faststart"});

    output.settings = settings;
    output.settings.output_path = destination.string();
    output.duration = duration;
    try {
        output.total_frames = CeilFrames(duration, settings.frame_rate);
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
    output.filter_graph = graph.str();
    output.temporary_output_path = TemporaryOutputPath(destination);
    arguments.push_back(output.temporary_output_path);
    output.arguments = std::move(arguments);
    return true;
}

bool Exporter::Run(const ExportPlan& plan,
                   const ExportProgressCallback& callback,
                   const std::atomic_bool* cancel, std::string& error) {
    error.clear();
    const auto removeTemporaryFiles = [&]() {
        std::error_code ignored;
        std::filesystem::remove(plan.temporary_output_path, ignored);
        if (!plan.temporary_lut_path.empty())
            std::filesystem::remove(plan.temporary_lut_path, ignored);
    };
    if (!plan.temporary_lut_path.empty()) {
        std::ofstream lut(plan.temporary_lut_path,
                          std::ios::binary | std::ios::trunc);
        lut << plan.color_lut;
        lut.close();
        if (!lut) {
            error = "unable to create the export color transform";
            removeTemporaryFiles();
            return false;
        }
    }
    int progressPipe[2] = {-1, -1};
    int errorPipe[2] = {-1, -1};
    if (pipe(progressPipe) != 0 || pipe(errorPipe) != 0) {
        error = "unable to create export process pipes: " +
                std::string(std::strerror(errno));
        if (progressPipe[0] >= 0) close(progressPipe[0]);
        if (progressPipe[1] >= 0) close(progressPipe[1]);
        if (errorPipe[0] >= 0) close(errorPipe[0]);
        if (errorPipe[1] >= 0) close(errorPipe[1]);
        return false;
    }

    std::vector<std::string> storage;
    storage.reserve(plan.arguments.size() + 1);
    storage.push_back(plan.settings.ffmpeg_path);
    storage.insert(storage.end(), plan.arguments.begin(), plan.arguments.end());
    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (std::string& value : storage) argv.push_back(value.data());
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, progressPipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, errorPipe[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, progressPipe[0]);
    posix_spawn_file_actions_addclose(&actions, errorPipe[0]);
    posix_spawn_file_actions_addclose(&actions, progressPipe[1]);
    posix_spawn_file_actions_addclose(&actions, errorPipe[1]);
    pid_t process = 0;
    const int spawnResult = posix_spawnp(
        &process, storage.front().c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(progressPipe[1]);
    close(errorPipe[1]);
    if (spawnResult != 0) {
        close(progressPipe[0]);
        close(errorPipe[0]);
        error = "unable to start FFmpeg: " +
                std::string(std::strerror(spawnResult));
        removeTemporaryFiles();
        return false;
    }

    ExportProgress current;
    current.total_frames = plan.total_frames;
    std::string progressBuffer;
    std::string errorTail;
    bool progressOpen = true;
    bool errorOpen = true;
    bool cancelled = false;
    std::chrono::steady_clock::time_point cancellationStarted;
    bool forced = false;
    while (progressOpen || errorOpen) {
        if (!cancelled && cancel && cancel->load()) {
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
            {progressPipe[0], static_cast<short>(progressOpen ? POLLIN : 0), 0},
            {errorPipe[0], static_cast<short>(errorOpen ? POLLIN : 0), 0},
        };
        const int pollResult = poll(descriptors, 2, 100);
        if (pollResult < 0 && errno != EINTR) break;
        char bytes[4096];
        if (progressOpen &&
            (descriptors[0].revents & (POLLIN | POLLHUP | POLLERR))) {
            const ssize_t count = read(progressPipe[0], bytes, sizeof(bytes));
            if (count <= 0) {
                progressOpen = false;
                close(progressPipe[0]);
            } else {
                progressBuffer.append(bytes, static_cast<size_t>(count));
                size_t newline = 0;
                while ((newline = progressBuffer.find('\n')) !=
                       std::string::npos) {
                    const std::string line = progressBuffer.substr(0, newline);
                    progressBuffer.erase(0, newline + 1);
                    if (ParseProgressLine(line, current) && callback)
                        callback(current);
                }
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
    if (progressOpen) close(progressPipe[0]);
    if (errorOpen) close(errorPipe[0]);
    if (cancelled || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        removeTemporaryFiles();
        if (cancelled)
            error = "export cancelled";
        else
            error = errorTail.empty() ? "FFmpeg export failed" : errorTail;
        return false;
    }

    std::error_code renameError;
    if (plan.settings.overwrite) {
        // POSIX rename is atomic and replaces a regular destination on macOS.
        std::filesystem::rename(plan.temporary_output_path,
                                plan.settings.output_path, renameError);
    } else {
        // link(2) atomically refuses an existing destination. Both paths are
        // siblings, so the hard-link commit cannot cross filesystems.
        if (link(plan.temporary_output_path.c_str(),
                 plan.settings.output_path.c_str()) != 0) {
            renameError = std::error_code(errno, std::generic_category());
        } else {
            std::error_code ignored;
            std::filesystem::remove(plan.temporary_output_path, ignored);
        }
    }
    if (renameError) {
        removeTemporaryFiles();
        error = "unable to commit exported file: " + renameError.message();
        return false;
    }
    if (!plan.temporary_lut_path.empty()) {
        std::error_code ignored;
        std::filesystem::remove(plan.temporary_lut_path, ignored);
    }
    current.rendered_frames = plan.total_frames;
    current.fraction = 1.0;
    if (callback) callback(current);
    return true;
}
