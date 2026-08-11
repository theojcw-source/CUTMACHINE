#include "Proxy.h"

#include "Ulid.h"

#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

extern char** environ;

namespace {

void AppendTail(std::string& tail, const char* bytes, size_t count) {
    tail.append(bytes, count);
    constexpr size_t kMaximum = 8192;
    if (tail.size() > kMaximum) tail.erase(0, tail.size() - kMaximum);
}

}  // namespace

bool GenerateMediaProxy(const std::string& inputPath,
                        const std::string& outputPath,
                        const RationalTime& duration,
                        const ProxySettings& settings,
                        MediaTaskContext& context, std::string& error) {
    error.clear();
    if (duration.rate <= 0 || duration.value <= 0 || settings.max_width < 64) {
        error = "invalid proxy settings or source duration";
        return false;
    }
    std::error_code filesystemError;
    const std::filesystem::path destination(outputPath);
    std::filesystem::create_directories(destination.parent_path(),
                                        filesystemError);
    if (filesystemError) {
        error =
            "unable to create proxy directory: " + filesystemError.message();
        return false;
    }
    const std::filesystem::path temporary =
        destination.parent_path() /
        (destination.stem().string() + ".partial-" + GenerateUlid() +
         destination.extension().string());
    std::vector<std::string> storage = {
        settings.ffmpeg_path,
        "-hide_banner",
        "-nostdin",
        "-y",
        "-noautorotate",
        "-i",
        inputPath,
        "-map",
        "0:v:0",
        "-an",
        "-vf",
        "scale='min(" + std::to_string(settings.max_width) +
            ",iw)':-2:flags=lanczos",
        "-c:v",
        "prores_ks",
        "-profile:v",
        "0",
        "-pix_fmt",
        "yuv422p10le",
        "-vendor",
        "apl0",
        "-progress",
        "pipe:1",
        "-nostats",
        temporary.string(),
    };
    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (std::string& value : storage) argv.push_back(value.data());
    argv.push_back(nullptr);

    int progressPipe[2] = {-1, -1};
    int errorPipe[2] = {-1, -1};
    if (pipe(progressPipe) != 0 || pipe(errorPipe) != 0) {
        error = "unable to create proxy process pipes: " +
                std::string(std::strerror(errno));
        if (progressPipe[0] >= 0) close(progressPipe[0]);
        if (progressPipe[1] >= 0) close(progressPipe[1]);
        if (errorPipe[0] >= 0) close(errorPipe[0]);
        if (errorPipe[1] >= 0) close(errorPipe[1]);
        return false;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, progressPipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, errorPipe[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, progressPipe[0]);
    posix_spawn_file_actions_addclose(&actions, errorPipe[0]);
    posix_spawn_file_actions_addclose(&actions, progressPipe[1]);
    posix_spawn_file_actions_addclose(&actions, errorPipe[1]);
    pid_t process = 0;
    const int spawnResult =
        posix_spawnp(&process, storage.front().c_str(), &actions, nullptr,
                     argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(progressPipe[1]);
    close(errorPipe[1]);
    if (spawnResult != 0) {
        close(progressPipe[0]);
        close(errorPipe[0]);
        error = "unable to start FFmpeg: " +
                std::string(std::strerror(spawnResult));
        return false;
    }

    const long double durationSeconds =
        static_cast<long double>(duration.value) / duration.rate;
    std::string progressBuffer;
    std::string errorTail;
    bool progressOpen = true;
    bool errorOpen = true;
    bool cancelled = false;
    auto cancellationStarted = std::chrono::steady_clock::time_point{};
    bool forced = false;
    while (progressOpen || errorOpen) {
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
                    constexpr const char* prefix = "out_time_us=";
                    if (line.rfind(prefix, 0) == 0) {
                        try {
                            const int64_t microseconds =
                                std::stoll(line.substr(std::strlen(prefix)));
                            context.SetProgress(
                                durationSeconds <= 0.0L
                                    ? 0.0
                                    : static_cast<double>(microseconds /
                                                          1000000.0L /
                                                          durationSeconds),
                                "Encoding ProRes Proxy");
                        } catch (...) {
                        }
                    }
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
    if (cancelled || context.Cancelled()) {
        error = "proxy cancelled";
        std::filesystem::remove(temporary, filesystemError);
        return false;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        error =
            errorTail.empty() ? "FFmpeg proxy generation failed" : errorTail;
        std::filesystem::remove(temporary, filesystemError);
        return false;
    }
    std::filesystem::rename(temporary, destination, filesystemError);
    if (filesystemError) {
        error = "unable to install proxy: " + filesystemError.message();
        std::filesystem::remove(temporary, filesystemError);
        return false;
    }
    context.SetProgress(1.0, "Proxy ready");
    return true;
}
