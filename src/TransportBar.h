#pragma once

// F2.5 -- ROADMAP.md. The Transport panel's scrub bar is, like
// TimelineView.h's TimelineViewport, the one place a UI slider position is
// allowed to cross into exact playhead time -- see the README's principle
// 4 ("le temps est exact") and TimelineViewport's own file comment, which
// this mirrors for a simpler [0, duration] range instead of a scrolling,
// zoomable one. Plain C++, no AppKit, so it builds and is tested on a
// plain Linux host the same way TimelineViewport is (see
// tests/timeline_view_tests.cc and this file's own
// tests/transport_bar_tests.cc).
//
// FractionToTime only crosses the fraction -> time boundary; it does not
// itself snap to a frame or sample grid. Callers (TransportView.mm, wired
// up in main.mm) still run the result through TimelineView.h's
// QuantizePlayheadPosition before it becomes the authoritative playhead --
// exactly as the main timeline's ruler-drag scrub already does for
// TimelineViewport::XToTime. No raw pixel/fraction value is ever stored as
// playhead state; only the RationalTime this produces is.

#include "RationalTime.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

struct ScrubBarRange {
    // The sequence's total duration. A scrub fraction of 0.0 is time zero;
    // 1.0 is `duration`.
    RationalTime duration{0, 1};

    // Maps a playhead position to a slider fraction in [0, 1]. Positions
    // outside [0, duration] clamp rather than extrapolate -- a scrub knob
    // position is always a valid fraction, never off the end of the bar.
    double TimeToFraction(RationalTime time) const;

    // Maps a slider fraction (clamped to [0, 1] first) to an exact
    // RationalTime at `rate`. See the file comment for the quantization
    // this deliberately leaves to the caller.
    RationalTime FractionToTime(double fraction, int32_t rate) const;
};

inline double ScrubBarRange::TimeToFraction(RationalTime time) const {
    if (duration.rate <= 0)
        throw std::invalid_argument("scrub bar duration rate must be positive");
    if (time.rate <= 0)
        throw std::invalid_argument("time rate must be positive");
    if (duration.value <= 0) return 0.0;
    const long double positionSeconds =
        static_cast<long double>(time.value) / time.rate;
    const long double durationSeconds =
        static_cast<long double>(duration.value) / duration.rate;
    const long double fraction = positionSeconds / durationSeconds;
    return static_cast<double>(std::clamp(fraction, 0.0L, 1.0L));
}

inline RationalTime ScrubBarRange::FractionToTime(double fraction,
                                                  int32_t rate) const {
    if (duration.rate <= 0)
        throw std::invalid_argument("scrub bar duration rate must be positive");
    if (rate <= 0)
        throw std::invalid_argument("requested rate must be positive");
    if (!std::isfinite(fraction))
        throw std::invalid_argument("scrub fraction must be finite");
    const long double clamped =
        std::clamp(static_cast<long double>(fraction), 0.0L, 1.0L);
    const long double durationSeconds =
        static_cast<long double>(duration.value) / duration.rate;
    const long double value = durationSeconds * clamped * rate;
    if (!std::isfinite(value) ||
        value < static_cast<long double>(std::numeric_limits<int64_t>::min()) ||
        value > static_cast<long double>(std::numeric_limits<int64_t>::max())) {
        throw std::overflow_error("scrub bar position exceeds int64 range");
    }
    // Round to nearest frame/tick at `rate`, halfway away from zero -- same
    // policy as TimelineViewport::XToTime's RoundedInt64.
    return {static_cast<int64_t>(std::round(value)), rate};
}
