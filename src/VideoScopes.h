#pragma once

#include <cstddef>
#include <cstdint>

// UI-2026-08 -- video scopes are a local monitoring preference. They never
// enter Document because changing a diagnostic view must not change the edit.
enum class VideoScopeMode : int32_t {
    Off = 0,
    Waveform = 1,
    ParadeRgb = 2,
    Vectorscope = 3,
};

VideoScopeMode VideoScopeModeFromPreference(int32_t value);
size_t VideoScopeHistogramBinCount(VideoScopeMode mode);
