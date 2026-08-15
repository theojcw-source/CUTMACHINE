#pragma once

// F2.1 -- ROADMAP.md: the single source of truth for the design system's
// palette, spacing scale and type scale. Plain C++, no AppKit -- everything
// here is a named constant or a pure function of one, so it is directly
// unit-testable (see tests/ui_theme_tests.cc) without a macOS toolchain.
//
// The color values are not invented: they are read out of the one place
// this project already committed to a visual language, the Metal-drawn
// timeline built in -[AppDelegate timelineRenderData] (src/main.mm). That
// function still owns the timeline's one-off glyph geometry, but its
// *palette* -- surfaces, borders, text, accents -- now comes from here, so
// the timeline and every AppKit panel built on top of UiComponents.h read
// the same numbers instead of two palettes that quietly drift apart.
//
// Cocoa-facing code converts these into NSColor/NSFont through
// UiThemeAppKit.h rather than re-deriving literals.

namespace ui::theme {

struct Color {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;
};

// ---- Surfaces (dark, low-key desaturated grays, darkest to lightest) ----
inline constexpr Color kSurfaceBase{0.055f, 0.055f, 0.058f, 1.0f};
inline constexpr Color kSurfacePanel{0.075f, 0.075f, 0.078f, 1.0f};
inline constexpr Color kSurfaceRaised{0.095f, 0.098f, 0.102f, 1.0f};
inline constexpr Color kSurfaceRowEven{0.070f, 0.072f, 0.076f, 1.0f};
inline constexpr Color kSurfaceRowOdd{0.078f, 0.080f, 0.084f, 1.0f};
inline constexpr Color kSurfaceControl{0.130f, 0.135f, 0.140f, 1.0f};
inline constexpr Color kSurfaceControlActive{0.160f, 0.340f, 0.460f, 1.0f};

// ---- Borders ----
inline constexpr Color kBorderSubtle{0.130f, 0.132f, 0.135f, 1.0f};
inline constexpr Color kBorderStrong{0.260f, 0.260f, 0.280f, 1.0f};

// ---- Text ----
inline constexpr Color kTextPrimary{0.900f, 0.900f, 0.920f, 1.0f};
inline constexpr Color kTextSecondary{0.600f, 0.610f, 0.630f, 1.0f};
inline constexpr Color kTextTertiary{0.420f, 0.430f, 0.450f, 1.0f};

// ---- Accents ----
// Same numbers as the timeline's in/out markers, snap guide and selection
// highlight, so a panel's "selected" or "recording" state reads as the same
// color the timeline already uses for the same meaning.
inline constexpr Color kAccentBlue{0.240f, 0.820f, 1.000f,
                                   1.0f};  // focus / snap / active tab
inline constexpr Color kAccentGreen{0.200f, 0.880f, 0.520f,
                                    1.0f};  // in-point / positive
inline constexpr Color kAccentOrange{1.000f, 0.420f, 0.240f,
                                     1.0f};  // out-point
inline constexpr Color kAccentAmber{0.950f, 0.780f, 0.180f,
                                    1.0f};  // valid selection
inline constexpr Color kAccentRed{0.860f, 0.160f, 0.120f,
                                  1.0f};  // moving / error

inline constexpr Color kVideoTrackTint{0.120f, 0.430f, 0.670f, 1.0f};
inline constexpr Color kAudioTrackTint{0.130f, 0.480f, 0.280f, 1.0f};

// ---- Spacing scale (points) ----
inline constexpr double kSpaceXxs = 2.0;
inline constexpr double kSpaceXs = 4.0;
inline constexpr double kSpaceS = 8.0;
inline constexpr double kSpaceM = 12.0;
inline constexpr double kSpaceL = 16.0;
inline constexpr double kSpaceXl = 24.0;
inline constexpr double kSpaceXxl = 32.0;

// ---- Type scale (point sizes) ----
inline constexpr double kFontSizeCaption = 10.0;
inline constexpr double kFontSizeSmall = 11.0;
inline constexpr double kFontSizeBody = 12.0;
inline constexpr double kFontSizeSection = 13.0;
inline constexpr double kFontSizeTitle = 15.0;

// ---- Fixed component metrics ----
inline constexpr double kPanelHeaderHeight = 28.0;
inline constexpr double kControlRowHeight = 24.0;
inline constexpr double kTabStripHeight = 26.0;
inline constexpr double kCornerRadius = 4.0;

// Returns `color` with its alpha replaced by `alpha`; RGB unchanged.
constexpr Color WithAlpha(Color color, float alpha) {
    return Color{color.r, color.g, color.b, alpha};
}

// The tint used to mark a track/clip/badge as video or audio. Centralizes
// the ternary that used to be repeated at every one of its call sites in
// -[AppDelegate timelineRenderData].
constexpr Color TrackTint(bool isVideoTrack) {
    return isVideoTrack ? kVideoTrackTint : kAudioTrackTint;
}

// Linear interpolation between two colors, componentwise. `t` is not
// clamped -- callers pass values already known to be in [0, 1].
constexpr Color Mix(Color from, Color to, float t) {
    return Color{
        from.r + (to.r - from.r) * t,
        from.g + (to.g - from.g) * t,
        from.b + (to.b - from.b) * t,
        from.a + (to.a - from.a) * t,
    };
}

}  // namespace ui::theme
