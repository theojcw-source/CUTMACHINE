#include "VideoScopes.h"

VideoScopeMode VideoScopeModeFromPreference(int32_t value) {
    if (value < static_cast<int32_t>(VideoScopeMode::Off) ||
        value > static_cast<int32_t>(VideoScopeMode::Vectorscope))
        return VideoScopeMode::Off;
    return static_cast<VideoScopeMode>(value);
}

size_t VideoScopeHistogramBinCount(VideoScopeMode mode) {
    switch (mode) {
        case VideoScopeMode::Waveform:
            return 256u * 128u;
        case VideoScopeMode::ParadeRgb:
            return 3u * 128u * 128u;
        case VideoScopeMode::Vectorscope:
            return 256u * 256u;
        case VideoScopeMode::Off:
            return 1u;
    }
    return 1u;
}
