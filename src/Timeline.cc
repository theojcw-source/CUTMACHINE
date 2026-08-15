#include "Timeline.h"

#include <algorithm>
#include <stdexcept>

Timeline::Timeline(const Document& document) : document_(document) {}

std::vector<TrackResolution> Timeline::Resolve(RationalTime position) const {
    if (position.rate <= 0) {
        throw std::invalid_argument("timeline position rate must be positive");
    }
    std::vector<TrackResolution> result;
    result.reserve(document_.sequence.tracks.size());
    for (const DocumentTrack& track : document_.sequence.tracks) {
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

ResolvedFrame Timeline::ResolveClipAt(const DocumentClip& clip,
                                      RationalTime sourceTime) const {
    const DocumentSource* source = document_.FindSource(clip.source_id);
    if (!source)
        throw std::logic_error("validated clip references an unknown source");
    return {source->id,
            sourceTime.to_frames(source->rate.num, source->rate.den),
            clip.id};
}

std::vector<ResolvedLayer> Timeline::ResolveTrackLayers(
    const Ulid& trackId, RationalTime position) const {
    if (position.rate <= 0)
        throw std::invalid_argument("timeline position rate must be positive");
    const DocumentTrack* track = document_.FindTrack(trackId);
    if (!track)
        throw std::invalid_argument("unknown track ID '" + trackId + "'");
    for (const DocumentTransition& transition :
         document_.sequence.transitions) {
        if (transition.track_id != trackId) continue;
        const DocumentClip* left = document_.FindClip(transition.left_clip_id);
        const DocumentClip* right =
            document_.FindClip(transition.right_clip_id);
        if (!left || !right) continue;
        const int64_t frames =
            transition.duration.to_frames(document_.sequence.frame_rate.num,
                                          document_.sequence.frame_rate.den);
        int64_t preFrames = 0;
        int64_t postFrames = 0;
        if (transition.alignment == TransitionAlignment::Center) {
            preFrames = frames / 2;
            postFrames = frames - preFrames;
        } else if (transition.alignment == TransitionAlignment::StartAtCut) {
            postFrames = frames;
        } else {
            preFrames = frames;
        }
        const RationalTime pre{preFrames * document_.sequence.frame_rate.den,
                               document_.sequence.frame_rate.num};
        const RationalTime post{postFrames * document_.sequence.frame_rate.den,
                                document_.sequence.frame_rate.num};
        const RationalTime cut = right->timeline_in;
        const RationalTime start = cut.sub(pre);
        const RationalTime end = cut.add(post);
        if (position < start || position >= end) continue;
        const RationalTime fromCut = position.sub(cut);
        const RationalTime leftTime =
            left->source_in.add(left->duration).add(fromCut);
        const RationalTime rightTime = right->source_in.add(fromCut);
        const long double elapsed =
            static_cast<long double>(position.sub(start).value) /
            position.sub(start).rate;
        const long double total =
            static_cast<long double>(transition.duration.value) /
            transition.duration.rate;
        const float progress = static_cast<float>(
            std::clamp<long double>(elapsed / total, 0.0L, 1.0L));
        return {{ResolveClipAt(*left, leftTime), 1.0f},
                {ResolveClipAt(*right, rightTime), progress}};
    }
    const std::optional<ResolvedFrame> frame = ResolveInTrack(*track, position);
    return frame ? std::vector<ResolvedLayer>{{*frame, 1.0f}}
                 : std::vector<ResolvedLayer>{};
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
    const RationalTime sourceTime = clip.source_in.add(offset);
    return ResolveClipAt(clip, sourceTime);
}

RationalTime Timeline::Duration() const {
    RationalTime duration{0, 1};
    RationalTime timebase{0, 1};
    for (const DocumentTrack& track : document_.sequence.tracks) {
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
