#pragma once

#include "Document.h"

#include <cstdint>
#include <optional>
#include <string>

// QC-2026-09 (A8) -- editorial density is a property of the composited
// timeline, not of the number of rectangles in its tracks.  Keep this pure so
// the CLI, MCP and a future UI all ask the same question of the document.
struct TimelineStatsRatio {
    int64_t numerator = 0;
    int64_t denominator = 1;
};

struct TimelineStats {
    RationalTime duration;
    int64_t visible_shots = 0;
    int64_t visible_cuts = 0;
    TimelineStatsRatio plans_per_minute;
    int64_t cutaway_plans = 0;
    TimelineStatsRatio cutaway_share;
    // The first visible cutaway, not merely the first edit on the primary
    // track. This is the editorial "première coupe" measured by A8.
    std::optional<RationalTime> first_cut;
};

// Computes statistics from the active timeline.  Video is composited from
// low to high track index, so an interval covered by a higher visible track
// is attributed only to that top clip.  Invalid clip times are refused with a
// stable message rather than being repaired or rounded.
bool CalculateTimelineStats(const Document& document, TimelineStats& output,
                            std::string& error);

// Compact canonical JSON used by both headless surfaces.
std::string SerializeTimelineStats(const TimelineStats& stats);
