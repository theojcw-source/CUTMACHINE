#include "TimelineStats.h"

#include "Json.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <utility>
#include <vector>

namespace {

using mcp_json::Value;

Value TimeValue(const RationalTime& time) {
    Value value = Value::MakeObject();
    value.Set("value", Value::MakeInt(time.value));
    value.Set("rate", Value::MakeInt(time.rate));
    return value;
}

Value RatioValue(const TimelineStatsRatio& ratio) {
    Value value = Value::MakeObject();
    value.Set("numerator", Value::MakeInt(ratio.numerator));
    value.Set("denominator", Value::MakeInt(ratio.denominator));
    return value;
}

bool CheckClipTimes(const DocumentClip& clip, std::string& error) {
    if (clip.timeline_in.rate <= 0 || clip.duration.rate <= 0) {
        error = "clip '" + clip.id + "' has a non-positive time rate";
        return false;
    }
    if (clip.timeline_in.value < 0 || clip.duration.value < 0) {
        error = "clip '" + clip.id + "' has a negative timeline extent";
        return false;
    }
    return true;
}

bool ClipEnd(const DocumentClip& clip, RationalTime& output,
             std::string& error) {
    if (!CheckClipTimes(clip, error)) return false;
    try {
        output = clip.timeline_in.add(clip.duration);
    } catch (const std::exception&) {
        error = "clip '" + clip.id + "' timeline extent overflows";
        return false;
    }
    return true;
}

bool CheckedRatio(__int128 numerator, __int128 denominator,
                 TimelineStatsRatio& output, std::string& error) {
    if (denominator <= 0) {
        error = "timeline stats ratio has a non-positive denominator";
        return false;
    }
    __int128 left = numerator;
    __int128 right = denominator;
    while (right != 0) {
        const __int128 remainder = left % right;
        left = right;
        right = remainder;
    }
    const __int128 divisor = left;
    numerator /= divisor;
    denominator /= divisor;
    if (numerator < std::numeric_limits<int64_t>::min() ||
        numerator > std::numeric_limits<int64_t>::max() ||
        denominator > std::numeric_limits<int64_t>::max()) {
        error = "timeline stats ratio overflows int64";
        return false;
    }
    output = {static_cast<int64_t>(numerator),
              static_cast<int64_t>(denominator)};
    return true;
}

struct VisibleSegment {
    Ulid clip_id;
    int32_t track_index = 0;
    RationalTime start;
    RationalTime end;
};

}  // namespace

bool CalculateTimelineStats(const Document& document, TimelineStats& output,
                            std::string& error) {
    output = TimelineStats{};
    error.clear();

    RationalTime duration{0, 1};
    std::vector<RationalTime> boundaries;
    int32_t primaryVideoTrack = std::numeric_limits<int32_t>::max();
    for (const DocumentTrack& track : document.sequence.tracks) {
        if (track.kind == "video")
            primaryVideoTrack = std::min(primaryVideoTrack, track.index);
        for (const DocumentClip& clip : track.clips) {
            RationalTime end;
            if (!ClipEnd(clip, end, error)) return false;
            if (end > duration) duration = end;
            if (track.kind == "video" && track.visible &&
                clip.duration.value > 0) {
                boundaries.push_back(clip.timeline_in);
                boundaries.push_back(end);
            }
        }
    }
    output.duration = duration;

    std::sort(boundaries.begin(), boundaries.end(),
              [](const RationalTime& left, const RationalTime& right) {
                  return left < right;
              });
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()),
                     boundaries.end());

    std::vector<VisibleSegment> segments;
    for (size_t index = 0; index + 1 < boundaries.size(); ++index) {
        const RationalTime start = boundaries[index];
        const RationalTime end = boundaries[index + 1];
        if (start >= end) continue;

        const DocumentClip* topClip = nullptr;
        const DocumentTrack* topTrack = nullptr;
        for (const DocumentTrack& track : document.sequence.tracks) {
            if (track.kind != "video" || !track.visible) continue;
            for (const DocumentClip& clip : track.clips) {
                RationalTime clipEnd;
                if (!ClipEnd(clip, clipEnd, error)) return false;
                if (clip.timeline_in <= start && clipEnd > start &&
                    (topTrack == nullptr || track.index > topTrack->index)) {
                    topClip = &clip;
                    topTrack = &track;
                }
            }
        }
        if (topClip == nullptr) continue;
        if (!segments.empty() && segments.back().clip_id == topClip->id &&
            segments.back().end == start) {
            segments.back().end = end;
        } else {
            segments.push_back(
                {topClip->id, topTrack->index, start, end});
        }
    }

    output.visible_shots = static_cast<int64_t>(segments.size());
    for (size_t index = 1; index < segments.size(); ++index) {
        if (segments[index - 1].end == segments[index].start) {
            ++output.visible_cuts;
            if (!output.first_cut)
                output.first_cut = segments[index].start;
        }
    }
    for (const VisibleSegment& segment : segments) {
        if (segment.track_index != primaryVideoTrack) ++output.cutaway_plans;
    }

    if (duration.value == 0) {
        output.plans_per_minute = {0, 1};
    } else if (!CheckedRatio(
                   static_cast<__int128>(output.visible_shots) * 60 *
                       duration.rate,
                   duration.value, output.plans_per_minute, error)) {
        return false;
    }
    if (output.visible_shots == 0) {
        output.cutaway_share = {0, 1};
    } else if (!CheckedRatio(output.cutaway_plans, output.visible_shots,
                             output.cutaway_share, error)) {
        return false;
    }
    return true;
}

std::string SerializeTimelineStats(const TimelineStats& stats) {
    Value root = Value::MakeObject();
    root.Set("duration", TimeValue(stats.duration));
    root.Set("visible_shots", Value::MakeInt(stats.visible_shots));
    root.Set("visible_cuts", Value::MakeInt(stats.visible_cuts));
    root.Set("plans_per_minute", RatioValue(stats.plans_per_minute));
    root.Set("cutaway_plans", Value::MakeInt(stats.cutaway_plans));
    root.Set("cutaway_share", RatioValue(stats.cutaway_share));
    if (stats.first_cut)
        root.Set("first_cut", TimeValue(*stats.first_cut));
    else
        root.Set("first_cut", Value::MakeNull());
    return root.Dump();
}
