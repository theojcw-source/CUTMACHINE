#include "Timeline.h"

#include <algorithm>
#include <stdexcept>

Timeline::Timeline(const Document& document) : document_(document) {}

namespace {

float ClipOpacity(const DocumentClip& clip) {
    return static_cast<float>(clip.opacity.num) /
           static_cast<float>(clip.opacity.den);
}

}  // namespace

std::vector<TrackResolution> Timeline::Resolve(RationalTime position) const {
    if (position.rate <= 0) {
        throw std::invalid_argument("timeline position rate must be positive");
    }
    std::vector<TrackResolution> result;
    result.reserve(document_.sequence.tracks.size());
    for (const DocumentTrack& track : document_.sequence.tracks)
        result.push_back({track.id, track.kind == "caption"
                                        ? std::optional<ResolvedFrame>{}
                                        : ResolveInTrack(track, position)});
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
    return track->kind == "caption" ? std::optional<ResolvedFrame>{}
                                    : ResolveInTrack(*track, position);
}

ResolvedFrame Timeline::ResolveClipAt(const DocumentClip& clip,
                                      RationalTime sourceTime) const {
    const DocumentSource* source = document_.FindSource(clip.source_id);
    if (!source)
        throw std::logic_error("validated clip references an unknown source");
    return {source->id,
            sourceTime.to_frames(source->rate.num, source->rate.den), clip.id};
}

std::vector<ResolvedLayer> Timeline::ResolveTrackLayers(
    const Ulid& trackId, RationalTime position) const {
    if (position.rate <= 0)
        throw std::invalid_argument("timeline position rate must be positive");
    const DocumentTrack* track = document_.FindTrack(trackId);
    if (!track)
        throw std::invalid_argument("unknown track ID '" + trackId + "'");
    if (track->kind == "caption") return {};
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
        return {
            {ResolveClipAt(*left, leftTime), ClipOpacity(*left)},
            {ResolveClipAt(*right, rightTime), progress * ClipOpacity(*right)}};
    }
    const std::optional<ResolvedFrame> frame = ResolveInTrack(*track, position);
    if (!frame) return {};
    const DocumentClip* clip = document_.FindClip(frame->clip_id);
    return clip ? std::vector<ResolvedLayer>{{*frame, ClipOpacity(*clip)}}
                : std::vector<ResolvedLayer>{};
}

std::vector<ResolvedFrame> Timeline::ResolveUpcoming(
    RationalTime position, int direction, RationalTime lookahead) const {
    if (position.rate <= 0)
        throw std::invalid_argument("timeline position rate must be positive");
    if (lookahead.rate <= 0)
        throw std::invalid_argument("lookahead rate must be positive");
    std::vector<ResolvedFrame> upcoming;
    if (direction == 0 || lookahead.value <= 0) return upcoming;
    for (const DocumentTrack& track : document_.sequence.tracks) {
        // Only the tracks that actually feed the composite, which is what
        // main.mm builds its video slots from: warming the decoder of a
        // hidden or audio track would spend a seek on an image nothing is
        // going to draw.
        if (track.kind != "video" || !track.visible) continue;
        const std::optional<ResolvedFrame> frame =
            ResolveUpcomingInTrack(track, position, direction, lookahead);
        if (frame) upcoming.push_back(*frame);
    }
    return upcoming;
}

std::optional<ResolvedFrame> Timeline::ResolveUpcomingInTrack(
    const DocumentTrack& track, RationalTime position, int direction,
    RationalTime lookahead) const {
    // Clips are ordered and never overlap (Document::Validate), so the first
    // one found in the direction of travel is the next one entered.
    if (direction > 0) {
        const RationalTime limit = position.add(lookahead);
        for (const DocumentClip& clip : track.clips) {
            if (clip.timeline_in <= position) continue;
            if (clip.timeline_in > limit) break;
            return ResolveClipAt(clip, clip.source_in);
        }
        return std::nullopt;
    }
    const RationalTime limit = position.sub(lookahead);
    for (auto clip = track.clips.rbegin(); clip != track.clips.rend(); ++clip) {
        const RationalTime end = clip->timeline_in.add(clip->duration);
        if (end > position) continue;
        if (end < limit) break;
        // Played backward, a clip is entered by its last frame.
        ResolvedFrame frame = ResolveClipAt(*clip, end);
        frame.source_frame = LastSourceFrame(*clip);
        return frame;
    }
    return std::nullopt;
}

int64_t Timeline::LastSourceFrame(const DocumentClip& clip) const {
    const DocumentSource* source = document_.FindSource(clip.source_id);
    if (!source)
        throw std::logic_error("validated clip references an unknown source");
    const RationalTime end = clip.source_in.add(clip.duration);
    const int64_t frame = end.to_frames(source->rate.num, source->rate.den);
    // Ranges are half-open here as everywhere else: an end landing exactly on
    // a source frame boundary opens that frame rather than containing it, so
    // the clip's last frame is the one before.
    const RationalTime frameStart{
        frame * static_cast<int64_t>(source->rate.den), source->rate.num};
    return frameStart == end ? frame - 1 : frame;
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
