#pragma once

// F2.5 -- ROADMAP.md. Factored out of main.mm's TimelineTimecode() so the
// existing timeline-overlay timecode label and integrated transport toolbar
// format HH:MM:SS:FF from the exact same rule instead of two copies quietly
// drifting apart. Plain C++, no AppKit -- this is a pure
// function of a RationalTime and a MediaRate, so it builds and is tested on
// a plain Linux host the same way UiTheme.h/PanelLayout.h are (see
// tests/timecode_tests.cc).
//
// This does not round or pick a frame boundary -- `time` is presumed
// already exact (typically the output of TimelineView.h's
// QuantizePlayheadPosition). It only formats.

#include "Document.h"
#include "RationalTime.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>

inline std::string FormatTimecode(const RationalTime& time, MediaRate rate) {
    if (rate.num <= 0 || rate.den <= 0) return "00:00:00:00";
    const int64_t absoluteFrames =
        std::max<int64_t>(0, time.to_frames(rate.num, rate.den));
    // Nominal (rounded-up) frame rate, matching main.mm's original
    // TimelineTimecode -- e.g. 30000/1001 displays as a 30fps clock even
    // though the underlying rate is fractional.
    const int64_t nominalFps =
        std::max<int64_t>(1, (rate.num + rate.den - 1) / rate.den);
    const int64_t frames = absoluteFrames % nominalFps;
    const int64_t totalSeconds = absoluteFrames / nominalFps;
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%02lld:%02lld:%02lld:%02lld",
                  static_cast<long long>(totalSeconds / 3600),
                  static_cast<long long>((totalSeconds / 60) % 60),
                  static_cast<long long>(totalSeconds % 60),
                  static_cast<long long>(frames));
    return std::string(buffer);
}
