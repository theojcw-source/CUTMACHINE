#include "Thumbnail.h"

#include "Ulid.h"

#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <vector>

extern char** environ;

namespace {

void AppendTail(std::string& tail, const char* bytes, size_t count) {
    tail.append(bytes, count);
    constexpr size_t kMaximum = 8192;
    if (tail.size() > kMaximum) tail.erase(0, tail.size() - kMaximum);
}

}  // namespace

bool GenerateMediaThumbnail(const std::string& inputPath,
                            const std::string& outputPath,
                            const RationalTime& duration,
                            const ThumbnailSettings& settings,
                            MediaTaskContext& context, std::string& error) {
    error.clear();
    if (duration.rate <= 0 || duration.value <= 0 || settings.width < 64 ||
        settings.height < 36) {
        error = "invalid thumbnail settings or source duration";
        return false;
    }
    std::error_code filesystemError;
    const std::filesystem::path destination(outputPath);
    std::filesystem::create_directories(destination.parent_path(),
                                        filesystemError);
    if (filesystemError) {
        error = "unable to create thumbnail directory: " +
                filesystemError.message();
        return false;
    }
    const std::filesystem::path temporary =
        destination.parent_path() /
        (destination.stem().string() + ".partial-" + GenerateUlid() +
         destination.extension().string());
    const long double seconds =
        static_cast<long double>(duration.value) / duration.rate;
    const double seek = static_cast<double>(std::min<long double>(
        10.0L, std::max<long double>(0.0L, seconds * 0.1L)));
    const std::string filter =
        "scale=" + std::to_string(settings.width) + ":" +
        std::to_string(settings.height) +
        ":force_original_aspect_ratio=decrease:flags=lanczos,pad=" +
        std::to_string(settings.width) + ":" + std::to_string(settings.height) +
        ":(ow-iw)/2:(oh-ih)/2:color=0x202124";
    std::vector<std::string> storage = {
        settings.ffmpeg_path,
        "-hide_banner",
        "-loglevel",
        "error",
        "-nostdin",
        "-y",
        "-ss",
        std::to_string(seek),
        "-i",
        inputPath,
        "-map",
        "0:v:0",
        "-an",
        "-frames:v",
        "1",
        "-vf",
        filter,
        temporary.string(),
    };
    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (std::string& value : storage) argv.push_back(value.data());
    argv.push_back(nullptr);

    int errorPipe[2] = {-1, -1};
    if (pipe(errorPipe) != 0) {
        error = "unable to create thumbnail process pipe: " +
                std::string(std::strerror(errno));
        return false;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, errorPipe[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, errorPipe[0]);
    posix_spawn_file_actions_addclose(&actions, errorPipe[1]);
    pid_t process = 0;
    const int spawnResult =
        posix_spawnp(&process, storage.front().c_str(), &actions, nullptr,
                     argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(errorPipe[1]);
    if (spawnResult != 0) {
        close(errorPipe[0]);
        error = "unable to start FFmpeg: " +
                std::string(std::strerror(spawnResult));
        return false;
    }

    context.SetProgress(0.1, "Extraction de l'image");
    std::string errorTail;
    bool errorOpen = true;
    bool processRunning = true;
    bool cancelled = false;
    bool forced = false;
    int status = 0;
    auto cancellationStarted = std::chrono::steady_clock::time_point{};
    while (processRunning || errorOpen) {
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
        pollfd descriptor = {errorPipe[0],
                             static_cast<short>(errorOpen ? POLLIN : 0), 0};
        const int pollResult = poll(&descriptor, 1, 100);
        if (pollResult < 0 && errno != EINTR) break;
        if (errorOpen && (descriptor.revents & (POLLIN | POLLHUP | POLLERR))) {
            char bytes[4096];
            const ssize_t count = read(errorPipe[0], bytes, sizeof(bytes));
            if (count <= 0) {
                errorOpen = false;
                close(errorPipe[0]);
            } else {
                AppendTail(errorTail, bytes, static_cast<size_t>(count));
            }
        }
        if (processRunning) {
            const pid_t result = waitpid(process, &status, WNOHANG);
            if (result == process) processRunning = false;
        }
    }
    if (processRunning)
        while (waitpid(process, &status, 0) < 0 && errno == EINTR) {
        }
    if (errorOpen) close(errorPipe[0]);
    if (cancelled || context.Cancelled()) {
        error = "thumbnail cancelled";
        std::filesystem::remove(temporary, filesystemError);
        return false;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        error = errorTail.empty() ? "FFmpeg thumbnail generation failed"
                                  : errorTail;
        std::filesystem::remove(temporary, filesystemError);
        return false;
    }
    std::filesystem::rename(temporary, destination, filesystemError);
    if (filesystemError) {
        error = "unable to install thumbnail: " + filesystemError.message();
        std::filesystem::remove(temporary, filesystemError);
        return false;
    }
    context.SetProgress(1.0, "Thumbnail prête");
    return true;
}
