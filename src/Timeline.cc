#include "Timeline.h"

#include <algorithm>
#include <stdexcept>

Timeline::Timeline(const Document& document) : document_(document) {}

std::vector<TrackResolution> Timeline::Resolve(RationalTime position) const {
    if (position.rate <= 0) {
        throw std::invalid_argument("timeline position rate must be positive");
    }
    std::vector<TrackResolution> result;
    result.reserve(document_.tracks.size());
    for (const DocumentTrack& track : document_.tracks) {
        result.push_back({track.id, ResolveInTrack(track, position)});
    }
    return result;
}

std::optional<ResolvedFrame> Timeline::ResolveTrack(
    const Ulid& trackId, RationalTime position) const {
    if (position.rate <= 0) {
        throw std::invalid_argument("timeline position rate must be positive");
    }
    const DocumentTrack* track = document_.FindTrack(trackId);
    if (!track) {
        throw std::invalid_argument("unknown track ID '" + trackId + "'");
    }
    return ResolveInTrack(*track, position);
}

std::optional<ResolvedFrame> Timeline::ResolveInTrack(
    const DocumentTrack& track, RationalTime position) const {
    // upper_bound finds the last clip whose start is <= position. Validation
    // guarantees ordering and no overlap, so only that clip can contain it.
    const auto after = std::upper_bound(
        track.clips.begin(), track.clips.end(), position,
        [](const RationalTime& value, const DocumentClip& clip) {
            return value < clip.timeline_in;
        });
    if (after == track.clips.begin()) {
        return std::nullopt;
    }
    const DocumentClip& clip = *std::prev(after);
    const RationalTime offset = position.sub(clip.timeline_in);
    if (offset.value < 0 || offset >= clip.duration) {
        return std::nullopt;
    }
    const DocumentSource* source = document_.FindSource(clip.source_id);
    if (!source) {
        throw std::logic_error("validated clip references an unknown source");
    }
    const RationalTime sourceTime = clip.source_in.add(offset);
    return ResolvedFrame{
        source->id,
        sourceTime.to_frames(source->rate.num, source->rate.den),
    };
}

RationalTime Timeline::Duration() const {
    RationalTime duration{0, 1};
    RationalTime timebase{0, 1};
    for (const DocumentTrack& track : document_.tracks) {
        for (const DocumentClip& clip : track.clips) {
            timebase = timebase.add(RationalTime{0, clip.timeline_in.rate});
            timebase = timebase.add(RationalTime{0, clip.duration.rate});
            const RationalTime end = clip.timeline_in.add(clip.duration);
            if (end > duration) {
                duration = end;
            }
        }
    }
    return duration.rescale(timebase.rate);
}
