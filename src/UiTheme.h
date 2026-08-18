#pragma once

// F2.1 -- ROADMAP.md: the single source of truth for the design system's
// palette, spacing scale and type scale. Plain C++, no AppKit -- everything
// here is a named constant or a pure function of one, so it is directly
// unit-testable (see tests/ui_theme_tests.cc) without a macOS toolchain.
//
// The named values are the C++ delivery from the 2026-08 ATELIER design
// pass. Keeping them here makes the AppKit chrome and Metal display lists
// projections of one palette instead of independent approximations.
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

// ---- Surfaces (dark, cool industrial grays, darkest to lightest) ----
inline constexpr Color kSurfaceBase{0.047f, 0.051f, 0.055f, 1.0f};
inline constexpr Color kSurfacePanel{0.082f, 0.094f, 0.106f, 1.0f};
inline constexpr Color kSurfaceRaised{0.114f, 0.125f, 0.137f, 1.0f};
inline constexpr Color kSurfaceControl{0.149f, 0.161f, 0.176f, 1.0f};
inline constexpr Color kSurfaceControlHi{0.243f, 0.263f, 0.282f, 1.0f};
inline constexpr Color kSurfaceSunken{0.031f, 0.035f, 0.035f, 1.0f};
inline constexpr Color kSurfaceLane{0.039f, 0.043f, 0.047f, 1.0f};

// Alternating rows remain named because NSTableView consumes them directly.
// Their two-percent separation is deliberately quieter than a border.
inline constexpr Color kSurfaceRowEven = kSurfacePanel;
inline constexpr Color kSurfaceRowOdd{0.086f, 0.098f, 0.110f, 1.0f};
inline constexpr Color kSurfaceControlActive = kSurfaceControlHi;

// ---- Separation and relief ----
inline constexpr Color kSeparator{0.000f, 0.000f, 0.000f, 1.0f};
inline constexpr Color kBorderSubtle{0.133f, 0.149f, 0.165f, 1.0f};
inline constexpr Color kBorderStrong{0.200f, 0.216f, 0.231f, 1.0f};
inline constexpr Color kEdgeLight{0.290f, 0.310f, 0.329f, 1.0f};
inline constexpr Color kLaneGrid{0.078f, 0.094f, 0.106f, 1.0f};

// ---- Text ----
inline constexpr Color kTextPrimary{0.910f, 0.894f, 0.863f, 1.0f};
inline constexpr Color kTextSecondary{0.557f, 0.580f, 0.561f, 1.0f};
inline constexpr Color kTextTertiary{0.486f, 0.514f, 0.533f, 1.0f};
inline constexpr Color kTextOnAccent{0.047f, 0.051f, 0.055f, 1.0f};
inline constexpr Color kTextClip{0.957f, 0.945f, 0.918f, 1.0f};

// ---- One interaction accent ----
inline constexpr Color kAccent{1.000f, 0.353f, 0.122f, 1.0f};
inline constexpr Color kAccentHi{1.000f, 0.580f, 0.400f, 1.0f};
inline constexpr Color kAccentLo{0.902f, 0.290f, 0.071f, 1.0f};

// ---- Semantic colors (information, never generic interaction chrome) ----
inline constexpr Color kMarkIn{0.545f, 0.831f, 0.314f, 1.0f};
inline constexpr Color kMarkOut = kTextPrimary;
inline constexpr Color kRenderCached{0.180f, 0.431f, 0.322f, 1.0f};
inline constexpr Color kRenderStale{0.541f, 0.353f, 0.133f, 1.0f};
inline constexpr Color kError{0.769f, 0.212f, 0.165f, 1.0f};

// ---- Track families ----
inline constexpr Color kTrackVideo{0.200f, 0.220f, 0.239f, 1.0f};
inline constexpr Color kTrackVideoAlt{0.180f, 0.198f, 0.216f, 1.0f};
inline constexpr Color kTrackAudio{0.235f, 0.373f, 0.271f, 1.0f};
inline constexpr Color kTrackAudioAlt{0.200f, 0.310f, 0.259f, 1.0f};
inline constexpr Color kTrackMusic{0.243f, 0.251f, 0.220f, 1.0f};

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
inline constexpr double kFontSizeMonitorTimecode = 32.0;

// ---- Fixed component metrics ----
inline constexpr double kPanelHeaderHeight = 28.0;
inline constexpr double kControlRowHeight = 24.0;
inline constexpr double kTabStripHeight = 26.0;
inline constexpr double kCornerRadius = 4.0;
inline constexpr double kTimelineToolbarHeight = 26.0;
inline constexpr double kTimelineRulerHeight = 28.0;
inline constexpr double kTimelineRenderBandHeight = 5.0;
inline constexpr double kTimelineTrackHeight = 44.0;
inline constexpr double kTimelineTrackHeaderWidth = 96.0;
inline constexpr double kTimelineZoomBarHeight = 14.0;
inline constexpr double kTimelinePlayheadWidth = 1.0;
inline constexpr double kTimelineClipNameMinWidth = 40.0;

// Returns `color` with its alpha replaced by `alpha`; RGB unchanged.
constexpr Color WithAlpha(Color color, float alpha) {
    return Color{color.r, color.g, color.b, alpha};
}

// The tint used to mark a track/clip/badge as video or audio. Centralizes
// the ternary that used to be repeated at every one of its call sites in
// -[AppDelegate timelineRenderData].
constexpr Color TrackTint(bool isVideoTrack) {
    return isVideoTrack ? kTrackVideo : kTrackAudio;
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
