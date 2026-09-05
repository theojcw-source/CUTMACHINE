#include "FrameCapture.h"

#include "Ulid.h"

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

extern char** environ;

namespace {

// FFmpeg's -ss takes seconds. Every other time in this project stays
// rational, and this is the one place a RationalTime has to become a decimal
// string -- so it is done exactly, by long division, rather than through a
// double. A frame boundary that would land on 3.9999999 with floating point
// lands on 4.0 here, which is the difference between the frame asked for and
// the one before it.
std::string ExactSecondsString(const RationalTime& time) {
    if (time.rate <= 0) return "0";
    const bool negative = time.value < 0;
    const int64_t magnitude = negative ? -time.value : time.value;
    const int64_t whole = magnitude / time.rate;
    int64_t remainder = magnitude % time.rate;
    std::ostringstream output;
    if (negative) output << '-';
    output << whole;
    if (remainder == 0) return output.str();
    output << '.';
    // Nine digits is past any timebase this project accepts, and the loop
    // stops early once the division terminates.
    for (int digit = 0; digit < 9 && remainder != 0; ++digit) {
        remainder *= 10;
        output << static_cast<char>('0' + remainder / time.rate);
        remainder %= time.rate;
    }
    return output.str();
}

bool RunFfmpeg(const std::vector<std::string>& arguments, std::string& error) {
    std::vector<std::string> storage = arguments;
    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (std::string& value : storage) argv.push_back(value.data());
    argv.push_back(nullptr);
    pid_t process = 0;
    const int spawnResult =
        posix_spawnp(&process, storage.front().c_str(), nullptr, nullptr,
                     argv.data(), environ);
    if (spawnResult != 0) {
        error = "unable to start FFmpeg: " +
                std::string(std::strerror(spawnResult));
        return false;
    }
    int status = 0;
    while (waitpid(process, &status, 0) < 0 && errno == EINTR) {
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        error = "FFmpeg could not render the requested frame";
        return false;
    }
    return true;
}

}  // namespace

bool CaptureSourceFrame(const std::string& inputPath, const RationalTime& time,
                        const FrameCaptureSettings& settings,
                        std::string& jpegBytes, std::string& error) {
    error.clear();
    jpegBytes.clear();
    if (time.rate <= 0 || time.value < 0) {
        error = "frame time must be a non-negative exact position";
        return false;
    }
    if (settings.max_dimension < 16 || settings.jpeg_quality < 2 ||
        settings.jpeg_quality > 31) {
        error = "invalid frame capture settings";
        return false;
    }
    std::error_code filesystemError;
    const std::filesystem::path temporary =
        std::filesystem::temp_directory_path() /
        ("cutmachine-frame-" + GenerateUlid() + ".jpg");

    // `scale` with -1 on one axis keeps the aspect ratio and lets FFmpeg pick
    // the other dimension; `force_original_aspect_ratio=decrease` bounds the
    // long edge whichever way the frame is oriented, so a vertical rush is
    // bounded on its height and a horizontal one on its width.
    const std::string bound = std::to_string(settings.max_dimension);
    const std::vector<std::string> arguments = {
        settings.ffmpeg_path,
        "-hide_banner",
        "-loglevel",
        "error",
        "-nostdin",
        // Before -i: seeks by keyframe first, which is what makes this fast
        // on a long rush. The frame returned is still the one containing the
        // requested time, because -ss before the input is followed by exact
        // decoding up to it in current FFmpeg.
        "-ss",
        ExactSecondsString(time),
        "-i",
        inputPath,
        "-map",
        "0:v:0",
        "-an",
        "-frames:v",
        "1",
        "-vf",
        "scale=w=" + bound + ":h=" + bound +
            ":force_original_aspect_ratio=decrease",
        "-q:v",
        std::to_string(settings.jpeg_quality),
        "-y",
        temporary.string(),
    };
    if (!RunFfmpeg(arguments, error)) {
        std::filesystem::remove(temporary, filesystemError);
        return false;
    }
    std::ifstream input(temporary, std::ios::binary);
    if (!input) {
        std::filesystem::remove(temporary, filesystemError);
        error = "unable to read the rendered frame";
        return false;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    input.close();
    std::filesystem::remove(temporary, filesystemError);
    jpegBytes = buffer.str();
    if (jpegBytes.empty()) {
        error = "the source produced no frame at that position";
        return false;
    }
    return true;
}

std::string EncodeBase64(const std::string& bytes) {
    static const char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve((bytes.size() + 2) / 3 * 4);
    size_t index = 0;
    while (index + 2 < bytes.size()) {
        const uint32_t triple =
            (static_cast<uint32_t>(static_cast<unsigned char>(bytes[index]))
             << 16) |
            (static_cast<uint32_t>(static_cast<unsigned char>(bytes[index + 1]))
             << 8) |
            static_cast<uint32_t>(static_cast<unsigned char>(bytes[index + 2]));
        output.push_back(kAlphabet[(triple >> 18) & 0x3f]);
        output.push_back(kAlphabet[(triple >> 12) & 0x3f]);
        output.push_back(kAlphabet[(triple >> 6) & 0x3f]);
        output.push_back(kAlphabet[triple & 0x3f]);
        index += 3;
    }
    const size_t remaining = bytes.size() - index;
    if (remaining == 1) {
        const uint32_t value =
            static_cast<uint32_t>(static_cast<unsigned char>(bytes[index]));
        output.push_back(kAlphabet[(value >> 2) & 0x3f]);
        output.push_back(kAlphabet[(value << 4) & 0x3f]);
        output.push_back('=');
        output.push_back('=');
    } else if (remaining == 2) {
        const uint32_t value =
            (static_cast<uint32_t>(static_cast<unsigned char>(bytes[index]))
             << 8) |
            static_cast<uint32_t>(static_cast<unsigned char>(bytes[index + 1]));
        output.push_back(kAlphabet[(value >> 10) & 0x3f]);
        output.push_back(kAlphabet[(value >> 4) & 0x3f]);
        output.push_back(kAlphabet[(value << 2) & 0x3f]);
        output.push_back('=');
    }
    return output;
}
