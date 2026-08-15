// Pure-C++ test for HH:MM:SS:FF timecode formatting (ROADMAP.md F2.5).
// Timecode.h has no AppKit dependency, so this builds and runs on a plain
// Linux host, the same way tests/ui_theme_tests.cc does.

#include "Timecode.h"

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

template <typename Function>
void Test(const std::string& name, Function function) {
    const int before = failures;
    function();
    if (before == failures) std::cout << "PASS: " << name << '\n';
}

}  // namespace

int main() {
    Test("time zero formats as 00:00:00:00", [] {
        Check(FormatTimecode({0, 25}, {25, 1}) == "00:00:00:00",
              "zero should format to all-zero timecode");
    });

    Test("frame count at an integer frame rate", [] {
        // 1 second and 3 frames at 25fps.
        Check(FormatTimecode({28, 25}, {25, 1}) == "00:00:01:03",
              "28/25 @25fps should be 1s 3f");
    });

    Test("hours, minutes and seconds roll over correctly", [] {
        // 1h 02m 03s 04f at 25fps.
        const int64_t frames = ((3600 + 2 * 60 + 3) * 25) + 4;
        Check(FormatTimecode({frames, 25}, {25, 1}) == "01:02:03:04",
              "H:M:S:F should each roll over at their own modulus");
    });

    Test("a fractional (NTSC) rate displays against its nominal fps", [] {
        // A RationalTime for frame index N at MediaRate{num, den} is
        // {N * den, num} -- the same convention TimelineView.cc's
        // QuantizePlayheadPosition uses to build one.
        const MediaRate ntsc{30000, 1001};
        Check(FormatTimecode({29 * ntsc.den, ntsc.num}, ntsc) == "00:00:00:29",
              "frame 29 of 30 should still read as second 0, frame 29");
        Check(FormatTimecode({30 * ntsc.den, ntsc.num}, ntsc) == "00:00:01:00",
              "frame 30 (the nominal fps count) should roll into second 1, "
              "frame 0");
    });

    Test("a value at a different rate than the display rate rescales first",
         [] {
             // 2 seconds' worth of 48kHz samples, displayed at 25fps.
             Check(FormatTimecode({96000, 48000}, {25, 1}) == "00:00:02:00",
                   "2s of samples at 48kHz should display as 2s at 25fps");
         });

    Test("negative time clamps to zero rather than formatting garbage", [] {
        Check(FormatTimecode({-25, 25}, {25, 1}) == "00:00:00:00",
              "a negative time must not underflow into a huge value");
    });

    Test("an invalid display rate falls back to the zero timecode", [] {
        Check(FormatTimecode({100, 25}, {0, 1}) == "00:00:00:00",
              "rate.num <= 0 must not divide by zero");
        Check(FormatTimecode({100, 25}, {25, 0}) == "00:00:00:00",
              "rate.den <= 0 must not divide by zero");
    });

    return failures == 0 ? 0 : 1;
}
