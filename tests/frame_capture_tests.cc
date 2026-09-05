#include "FrameCapture.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

std::string Quote(const std::filesystem::path& path) {
    std::string result = "'";
    for (char character : path.string())
        result += character == '\'' ? "'\\''" : std::string(1, character);
    return result + "'";
}

}  // namespace

int main() {
    // ---- base64, against the RFC 4648 test vectors -----------------------
    Check(EncodeBase64("") == "", "the empty string encodes to nothing");
    Check(EncodeBase64("f") == "Zg==", "one byte pads with two '='");
    Check(EncodeBase64("fo") == "Zm8=", "two bytes pad with one '='");
    Check(EncodeBase64("foo") == "Zm9v", "three bytes need no padding");
    Check(EncodeBase64("foob") == "Zm9vYg==", "four bytes");
    Check(EncodeBase64("fooba") == "Zm9vYmE=", "five bytes");
    Check(EncodeBase64("foobar") == "Zm9vYmFy", "six bytes");
    // Bytes above 0x7f must not sign-extend: a JPEG is full of them, and
    // this is exactly where a char-vs-unsigned-char slip corrupts an image.
    const std::string high("\xff\xfe\xfd", 3);
    Check(EncodeBase64(high) == "//79",
          "high bytes encode unsigned, so JPEG data survives");

    // ---- a real frame, decoded by FFmpeg ---------------------------------
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "cutmachine-frame-tests";
    std::filesystem::create_directories(root);
    const std::filesystem::path source = root / "vertical.mp4";
    const std::string generate =
        Quote(FFMPEG_EXECUTABLE) +
        " -hide_banner -loglevel error -f lavfi -i "
        "'testsrc2=size=180x320:rate=25:duration=2' -c:v libx264 "
        "-pix_fmt yuv420p -y " +
        Quote(source);
    if (std::system(generate.c_str()) != 0) {
        std::cerr << "FAIL: unable to generate the frame fixture\n";
        std::filesystem::remove_all(root);
        return 1;
    }

    FrameCaptureSettings settings;
    settings.ffmpeg_path = FFMPEG_EXECUTABLE;
    std::string jpeg;
    std::string error;
    Check(CaptureSourceFrame(source.string(), {25, 25}, settings, jpeg, error),
          "a frame renders: " + error);
    // JPEG's own magic. Checked rather than assumed: the whole point of this
    // path is that a model receives a picture, and a truncated or misnamed
    // payload would be silently useless.
    Check(jpeg.size() > 512, "the frame has a plausible size");
    Check(jpeg.size() >= 3 && static_cast<unsigned char>(jpeg[0]) == 0xFF &&
              static_cast<unsigned char>(jpeg[1]) == 0xD8 &&
              static_cast<unsigned char>(jpeg[2]) == 0xFF,
          "the bytes really are a JPEG");

    // Two renders of the same position must be identical, so a frame handed
    // to a model is reproducible like every other measurement here.
    std::string again;
    Check(
        CaptureSourceFrame(source.string(), {25, 25}, settings, again, error) &&
            again == jpeg,
        "rendering the same position twice gives identical bytes");

    // A different position must give a different picture -- this is what
    // catches a seek that silently ignores its argument.
    std::string other;
    Check(
        CaptureSourceFrame(source.string(), {5, 25}, settings, other, error) &&
            other != jpeg,
        "a different position gives a different frame");

    // Exact seconds, not a float: 1/25 must become "0.04", never "0.039999".
    std::string early;
    Check(CaptureSourceFrame(source.string(), {1, 25}, settings, early, error),
          "an awkward rational position renders: " + error);

    Check(!CaptureSourceFrame(source.string(), {-1, 25}, settings, jpeg, error),
          "a negative position is refused");
    Check(!CaptureSourceFrame((root / "missing.mp4").string(), {0, 25},
                              settings, jpeg, error),
          "a missing source is refused rather than returning empty bytes");

    std::filesystem::remove_all(root);
    if (failures == 0) std::cout << "frame capture tests passed\n";
    return failures == 0 ? 0 : 1;
}
