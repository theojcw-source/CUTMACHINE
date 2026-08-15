// Pure-C++ test for the design system's token file (ROADMAP.md F2.1).
// UiTheme.h has no AppKit dependency, so this builds and runs on a plain
// Linux host, the same way tests/fft_tests.cc does.

#include "UiTheme.h"

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace {

using ui::theme::Color;

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

bool InUnitRange(float value) { return value >= 0.0f && value <= 1.0f; }

bool IsValidColor(const Color& color) {
    return InUnitRange(color.r) && InUnitRange(color.g) &&
           InUnitRange(color.b) && InUnitRange(color.a);
}

}  // namespace

int main() {
    Test("every named surface/border/text/accent token is a valid color", [] {
        const std::vector<Color> tokens{
            ui::theme::kSurfaceBase,
            ui::theme::kSurfacePanel,
            ui::theme::kSurfaceRaised,
            ui::theme::kSurfaceRowEven,
            ui::theme::kSurfaceRowOdd,
            ui::theme::kSurfaceControl,
            ui::theme::kSurfaceControlActive,
            ui::theme::kBorderSubtle,
            ui::theme::kBorderStrong,
            ui::theme::kTextPrimary,
            ui::theme::kTextSecondary,
            ui::theme::kTextTertiary,
            ui::theme::kAccentBlue,
            ui::theme::kAccentGreen,
            ui::theme::kAccentOrange,
            ui::theme::kAccentAmber,
            ui::theme::kAccentRed,
            ui::theme::kVideoTrackTint,
            ui::theme::kAudioTrackTint,
        };
        for (const Color& token : tokens)
            Check(IsValidColor(token), "token channel outside [0, 1]");
    });

    Test("surfaces get progressively lighter, darkest to lightest", [] {
        const auto luma = [](const Color& color) {
            return color.r + color.g + color.b;
        };
        Check(luma(ui::theme::kSurfaceBase) < luma(ui::theme::kSurfacePanel),
              "panel should be lighter than the base window surface");
        Check(luma(ui::theme::kSurfacePanel) < luma(ui::theme::kSurfaceRaised),
              "raised surfaces (toolbars/rulers) should read above panels");
        Check(
            luma(ui::theme::kSurfaceRaised) < luma(ui::theme::kSurfaceControl),
            "interactive control wells should stand out from raised chrome");
    });

    Test("text hierarchy goes primary > secondary > tertiary in luminance", [] {
        const auto luma = [](const Color& color) {
            return color.r + color.g + color.b;
        };
        Check(luma(ui::theme::kTextSecondary) < luma(ui::theme::kTextPrimary),
              "secondary text should be dimmer than primary");
        Check(luma(ui::theme::kTextTertiary) < luma(ui::theme::kTextSecondary),
              "tertiary text should be dimmer than secondary");
    });

    Test("WithAlpha changes only the alpha channel", [] {
        const Color faded = ui::theme::WithAlpha(ui::theme::kAccentBlue, 0.3f);
        Check(faded.r == ui::theme::kAccentBlue.r &&
                  faded.g == ui::theme::kAccentBlue.g &&
                  faded.b == ui::theme::kAccentBlue.b,
              "RGB must be unchanged");
        Check(faded.a == 0.3f, "alpha must be replaced");
    });

    Test("TrackTint distinguishes video from audio", [] {
        const Color video = ui::theme::TrackTint(true);
        const Color audio = ui::theme::TrackTint(false);
        Check(video.r == ui::theme::kVideoTrackTint.r &&
                  video.g == ui::theme::kVideoTrackTint.g &&
                  video.b == ui::theme::kVideoTrackTint.b,
              "video tint should match kVideoTrackTint");
        Check(audio.r == ui::theme::kAudioTrackTint.r &&
                  audio.g == ui::theme::kAudioTrackTint.g &&
                  audio.b == ui::theme::kAudioTrackTint.b,
              "audio tint should match kAudioTrackTint");
        Check(!(video.r == audio.r && video.g == audio.g && video.b == audio.b),
              "video and audio tints must be visually distinguishable");
    });

    Test("Mix interpolates and the endpoints match", [] {
        const auto nearlyEqual = [](float a, float b) {
            return std::abs(a - b) < 1e-6f;
        };
        const Color start = ui::theme::kSurfaceBase;
        const Color end = ui::theme::kAccentBlue;
        const Color atStart = ui::theme::Mix(start, end, 0.0f);
        const Color atEnd = ui::theme::Mix(start, end, 1.0f);
        Check(atStart.r == start.r && atStart.g == start.g &&
                  atStart.b == start.b,
              "t=0 should return the start color exactly");
        Check(nearlyEqual(atEnd.r, end.r) && nearlyEqual(atEnd.g, end.g) &&
                  nearlyEqual(atEnd.b, end.b),
              "t=1 should return the end color, within float rounding");
        const Color mid = ui::theme::Mix(start, end, 0.5f);
        Check(mid.r > std::min(start.r, end.r) &&
                  mid.r < std::max(start.r, end.r),
              "t=0.5 should land strictly between the endpoints");
    });

    Test("the spacing scale is strictly increasing", [] {
        const std::vector<double> spacing{
            ui::theme::kSpaceXxs, ui::theme::kSpaceXs, ui::theme::kSpaceS,
            ui::theme::kSpaceM,   ui::theme::kSpaceL,  ui::theme::kSpaceXl,
            ui::theme::kSpaceXxl,
        };
        for (size_t index = 1; index < spacing.size(); ++index)
            Check(spacing[index - 1] < spacing[index],
                  "spacing scale must be strictly increasing");
    });

    Test("the type scale is strictly increasing", [] {
        const std::vector<double> sizes{
            ui::theme::kFontSizeCaption, ui::theme::kFontSizeSmall,
            ui::theme::kFontSizeBody,    ui::theme::kFontSizeSection,
            ui::theme::kFontSizeTitle,
        };
        for (size_t index = 1; index < sizes.size(); ++index)
            Check(sizes[index - 1] < sizes[index],
                  "type scale must be strictly increasing");
    });

    return failures == 0 ? 0 : 1;
}
