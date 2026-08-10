#include "TimelineView.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace {

void CheckViewport(const TimelineViewport& viewport) {
    if (viewport.view_start.rate <= 0 ||
        !std::isfinite(viewport.pixels_per_second) ||
        viewport.pixels_per_second <= 0.0 ||
        !std::isfinite(viewport.track_height) || viewport.track_height <= 0.0 ||
        !std::isfinite(viewport.header_width)) {
        throw std::invalid_argument("invalid timeline viewport");
    }
}

int64_t RoundedInt64(long double value) {
    if (!std::isfinite(value) ||
        value < static_cast<long double>(std::numeric_limits<int64_t>::min()) ||
        value > static_cast<long double>(std::numeric_limits<int64_t>::max())) {
        throw std::overflow_error("timeline coordinate is outside int64 range");
    }
    // Round to nearest frame/tick. Halfway values are rounded away from zero,
    // matching std::round and making the policy explicit for negative views.
    return static_cast<int64_t>(std::round(value));
}

int64_t RoundNonNegativeRatio(__int128 numerator, __int128 denominator) {
    if (numerator < 0 || denominator <= 0)
        throw std::invalid_argument("invalid playhead quantization ratio");
    const __int128 quotient = numerator / denominator;
    const __int128 remainder = numerator % denominator;
    const __int128 rounded = quotient + (remainder * 2 >= denominator ? 1 : 0);
    if (rounded > std::numeric_limits<int64_t>::max())
        throw std::overflow_error("playhead position exceeds int64 range");
    return static_cast<int64_t>(rounded);
}

ExactClipTimes ProposedTimes(const DocumentClip& clip, TrimEdge edge,
                             const RationalTime& delta) {
    ExactClipTimes result{clip.source_in, clip.duration, clip.timeline_in};
    if (edge == TrimEdge::Head) {
        result.source_in = result.source_in.add(delta);
        result.duration = result.duration.sub(delta);
        result.timeline_in = result.timeline_in.add(delta);
    } else {
        result.duration = result.duration.add(delta);
    }
    return result;
}

RationalTime MaxTime(const RationalTime& left, const RationalTime& right) {
    return left < right ? right : left;
}

RationalTime MinTime(const RationalTime& left, const RationalTime& right) {
    return left > right ? right : left;
}

}  // namespace

RationalTime QuantizePlayheadPosition(RationalTime position,
                                      PlayheadResolution resolution,
                                      MediaRate frameRate, int32_t sampleRate) {
    if (position.rate <= 0 || position.value < 0)
        throw std::invalid_argument("playhead position must be non-negative");
    if (resolution == PlayheadResolution::Sample) {
        if (sampleRate <= 0)
            throw std::invalid_argument("sample rate must be positive");
        const int64_t sample = RoundNonNegativeRatio(
            static_cast<__int128>(position.value) * sampleRate, position.rate);
        return {sample, sampleRate};
    }
    if (frameRate.num <= 0 || frameRate.den <= 0)
        throw std::invalid_argument("frame rate must be positive");
    const int64_t frame = RoundNonNegativeRatio(
        static_cast<__int128>(position.value) * frameRate.num,
        static_cast<__int128>(position.rate) * frameRate.den);
    if (static_cast<__int128>(frame) * frameRate.den >
        std::numeric_limits<int64_t>::max())
        throw std::overflow_error("frame position exceeds int64 range");
    return {frame * frameRate.den, frameRate.num};
}

double TimelineViewport::TimeToX(RationalTime time) const {
    CheckViewport(*this);
    if (time.rate <= 0)
        throw std::invalid_argument("time rate must be positive");
    const long double seconds =
        static_cast<long double>(time.value) / time.rate -
        static_cast<long double>(view_start.value) / view_start.rate;
    return header_width + static_cast<double>(seconds * pixels_per_second);
}

RationalTime TimelineViewport::XToTime(double x, int32_t rate) const {
    CheckViewport(*this);
    if (!std::isfinite(x) || rate <= 0)
        throw std::invalid_argument("invalid coordinate or requested rate");
    const long double seconds =
        static_cast<long double>(view_start.value) / view_start.rate +
        (static_cast<long double>(x) - header_width) / pixels_per_second;
    return {RoundedInt64(seconds * rate), rate};
}

void TimelineViewport::ScrollByPixels(double delta_x, int32_t rate) {
    view_start = XToTime(header_width + delta_x, rate);
}

void TimelineViewport::ZoomAroundX(double x, double factor, int32_t rate) {
    if (!std::isfinite(factor) || factor <= 0.0)
        throw std::invalid_argument("zoom factor must be positive");
    const RationalTime anchor = XToTime(x, rate);
    pixels_per_second = std::clamp(pixels_per_second * factor, 4.0, 4000.0);
    const RationalTime displaced = XToTime(x, rate);
    view_start = view_start.add(anchor.sub(displaced));
}

void TimelineViewport::FitDuration(RationalTime duration, double width) {
    CheckViewport(*this);
    if (duration.rate <= 0 || !std::isfinite(width) || width <= header_width) {
        throw std::invalid_argument("invalid duration or viewport width");
    }
    view_start = {0, duration.rate};
    if (duration.value <= 0) {
        pixels_per_second = 100.0;
        return;
    }
    const long double seconds =
        static_cast<long double>(duration.value) / duration.rate;
    const long double available = std::max(1.0, width - header_width - 24.0);
    pixels_per_second =
        std::clamp(static_cast<double>(available / seconds), 4.0, 4000.0);
}

std::vector<double> TimelineViewport::TickXs(double width) const {
    CheckViewport(*this);
    struct Step {
        int64_t value;
        int32_t rate;
    };
    static constexpr Step steps[] = {
        {1, 120}, {1, 60},  {1, 30},  {1, 24},  {1, 10},   {1, 5},
        {1, 2},   {1, 1},   {2, 1},   {5, 1},   {10, 1},   {30, 1},
        {60, 1},  {120, 1}, {300, 1}, {600, 1}, {1800, 1}, {3600, 1}};
    Step step = steps[sizeof(steps) / sizeof(steps[0]) - 1];
    for (const Step candidate : steps) {
        if ((static_cast<double>(candidate.value) / candidate.rate) *
                pixels_per_second >=
            56.0) {
            step = candidate;
            break;
        }
    }
    const __int128 numerator =
        static_cast<__int128>(view_start.value) * step.rate;
    const __int128 denominator =
        static_cast<__int128>(view_start.rate) * step.value;
    __int128 ordinal = numerator / denominator;
    if (numerator > 0 && numerator % denominator != 0) ++ordinal;
    std::vector<double> result;
    for (size_t count = 0; count < 10000; ++count, ++ordinal) {
        const __int128 tickValue = ordinal * step.value;
        if (tickValue < std::numeric_limits<int64_t>::min() ||
            tickValue > std::numeric_limits<int64_t>::max())
            break;
        const double x = TimeToX({static_cast<int64_t>(tickValue), step.rate});
        if (x > width) break;
        if (x >= header_width) result.push_back(x);
    }
    return result;
}

std::vector<const DocumentTrack*> TimelineTracksInDisplayOrder(
    const Document& document) {
    std::vector<const DocumentTrack*> tracks;
    tracks.reserve(document.tracks.size());
    for (const DocumentTrack& track : document.tracks) tracks.push_back(&track);
    std::stable_sort(tracks.begin(), tracks.end(),
                     [](const DocumentTrack* a, const DocumentTrack* b) {
                         return a->index < b->index;
                     });
    return tracks;
}

std::optional<TimelineHit> HitTestTimeline(const Document& document,
                                           const TimelineViewport& viewport,
                                           double x, double y, double width) {
    if (x < viewport.header_width || x > width || y < kTimelineRulerHeight)
        return std::nullopt;
    const auto tracks = TimelineTracksInDisplayOrder(document);
    const size_t trackIndex = static_cast<size_t>(
        std::floor((y - kTimelineRulerHeight) / viewport.track_height));
    if (trackIndex >= tracks.size()) return std::nullopt;
    const DocumentTrack& track = *tracks[trackIndex];
    for (const DocumentClip& clip : track.clips) {
        const double left = viewport.TimeToX(clip.timeline_in);
        const double right =
            viewport.TimeToX(clip.timeline_in.add(clip.duration));
        if (right < viewport.header_width || left > width) continue;
        if (x >= left && x <= right) {
            const double headDistance = std::abs(x - left);
            const double tailDistance = std::abs(x - right);
            TimelineHitEdge edge = TimelineHitEdge::None;
            if (std::min(headDistance, tailDistance) <= kTimelineEdgeHitWidth) {
                edge = headDistance <= tailDistance ? TimelineHitEdge::Head
                                                    : TimelineHitEdge::Tail;
            }
            return TimelineHit{clip.id, track.id, edge};
        }
    }
    return std::nullopt;
}

std::optional<TimelineGapSelection> HitTestTimelineGap(
    const Document& document, const TimelineViewport& viewport, double x,
    double y, double width, int32_t timeRate) {
    if (x < viewport.header_width || x > width || y < kTimelineRulerHeight)
        return std::nullopt;
    const auto tracks = TimelineTracksInDisplayOrder(document);
    const size_t trackIndex = static_cast<size_t>(
        std::floor((y - kTimelineRulerHeight) / viewport.track_height));
    if (trackIndex >= tracks.size()) return std::nullopt;
    const DocumentTrack& track = *tracks[trackIndex];
    const RationalTime position = viewport.XToTime(x, timeRate);
    RationalTime cursor{0, timeRate};
    for (const DocumentClip& clip : track.clips) {
        if (cursor < clip.timeline_in && position >= cursor &&
            position < clip.timeline_in) {
            return TimelineGapSelection{track.id, cursor,
                                        clip.timeline_in.sub(cursor)};
        }
        cursor = clip.timeline_in.add(clip.duration);
    }
    return std::nullopt;
}

std::vector<Ulid> LassoHitTestTimeline(const Document& document,
                                       const TimelineViewport& viewport,
                                       double startX, double startY,
                                       double endX, double endY, double width) {
    const double left = std::max(viewport.header_width, std::min(startX, endX));
    const double right = std::min(width, std::max(startX, endX));
    const double top = std::max(kTimelineRulerHeight, std::min(startY, endY));
    const double bottom = std::max(startY, endY);
    std::vector<Ulid> result;
    if (right < left || bottom < top) return result;
    const auto tracks = TimelineTracksInDisplayOrder(document);
    for (size_t trackIndex = 0; trackIndex < tracks.size(); ++trackIndex) {
        const double clipTop =
            kTimelineRulerHeight + trackIndex * viewport.track_height + 3.0;
        const double clipBottom = clipTop + viewport.track_height - 6.0;
        if (clipBottom < top || clipTop > bottom) continue;
        for (const DocumentClip& clip : tracks[trackIndex]->clips) {
            const double clipLeft = viewport.TimeToX(clip.timeline_in);
            const double clipRight =
                viewport.TimeToX(clip.timeline_in.add(clip.duration));
            if (clipRight >= left && clipLeft <= right)
                result.push_back(clip.id);
        }
    }
    return result;
}

std::vector<Ulid> ExpandLinkedClipSelection(const Document& document,
                                            const std::vector<Ulid>& clipIds) {
    std::vector<Ulid> result;
    std::vector<Ulid> groups;
    for (const Ulid& id : clipIds) {
        const DocumentClip* clip = document.FindClip(id);
        if (!clip ||
            std::find(result.begin(), result.end(), id) != result.end())
            continue;
        result.push_back(id);
        if (!clip->link_group_id.empty() &&
            std::find(groups.begin(), groups.end(), clip->link_group_id) ==
                groups.end())
            groups.push_back(clip->link_group_id);
    }
    for (const DocumentTrack& track : document.tracks) {
        for (const DocumentClip& clip : track.clips) {
            if (clip.link_group_id.empty() ||
                std::find(groups.begin(), groups.end(), clip.link_group_id) ==
                    groups.end() ||
                std::find(result.begin(), result.end(), clip.id) !=
                    result.end())
                continue;
            result.push_back(clip.id);
        }
    }
    return result;
}

std::optional<RationalTime> ClipSyncDrift(const Document& document,
                                          const Ulid& clipId) {
    const DocumentClip* clip = document.FindClip(clipId);
    if (!clip || clip->sync_anchor_clip_id.empty()) return std::nullopt;
    const DocumentClip* anchor = document.FindClip(clip->sync_anchor_clip_id);
    if (!anchor || anchor->link_group_id != clip->link_group_id)
        return std::nullopt;
    try {
        const RationalTime clipPhase = clip->timeline_in.sub(clip->source_in);
        const RationalTime anchorPhase =
            anchor->timeline_in.sub(anchor->source_in);
        return clipPhase.sub(anchorPhase).sub(clip->sync_reference_delta);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

TimelineInteraction::TimelineInteraction(Document& document, EditLog& editLog,
                                         TimelineViewport& viewport)
    : document_(document), edit_log_(editLog), viewport_(viewport) {}

void TimelineInteraction::PointerDown(double x, double y, double width,
                                      int32_t timeRate) {
    requested_playhead_.reset();
    preview_.reset();
    move_preview_.reset();
    snap_guide_time_.reset();
    drag_hit_.reset();
    drag_start_time_.reset();
    const auto hit = HitTestTimeline(document_, viewport_, x, y, width);
    if (!hit) {
        selected_gap_ =
            HitTestTimelineGap(document_, viewport_, x, y, width, timeRate);
        if (x >= viewport_.header_width && y >= 0.0) {
            requested_playhead_ = viewport_.XToTime(x, timeRate);
        }
        selected_clip_id_.clear();
        selected_clip_ids_.clear();
        return;
    }
    selected_gap_.reset();
    selected_clip_id_ = hit->clip_id;
    selected_clip_ids_ = {hit->clip_id};
    if (linked_selection_enabled_)
        selected_clip_ids_ =
            ExpandLinkedClipSelection(document_, selected_clip_ids_);
    const DocumentClip* clip = document_.FindClip(hit->clip_id);
    drag_rate_ = clip ? clip->duration.rate : timeRate;
    drag_x_ = x;
    drag_start_time_ = viewport_.XToTime(x, drag_rate_);
    drag_hit_ = hit;
    UpdatePreview(x, y, width);
}

void TimelineInteraction::PointerDrag(double x, double y, double width) {
    if (drag_hit_) UpdatePreview(x, y, width);
}

void TimelineInteraction::UpdatePreview(double x, double y, double width) {
    (void)width;
    const DocumentClip* clip = document_.FindClip(drag_hit_->clip_id);
    if (!clip || !drag_start_time_) {
        preview_.reset();
        return;
    }
    RationalTime delta =
        viewport_.XToTime(x, drag_rate_).sub(*drag_start_time_);
    drag_x_ = x;
    snap_guide_time_.reset();
    if (drag_hit_->edge == TimelineHitEdge::None) {
        TimelineMovePreview preview;
        preview.clip_id = clip->id;
        preview.timeline_in = clip->timeline_in.add(delta);
        if (preview.timeline_in < RationalTime{0, 1})
            preview.timeline_in = {0, preview.timeline_in.rate};
        const auto tracks = TimelineTracksInDisplayOrder(document_);
        if (y >= kTimelineRulerHeight) {
            const size_t index = static_cast<size_t>(std::floor(
                (y - kTimelineRulerHeight) / viewport_.track_height));
            if (index < tracks.size())
                preview.target_track_id = tracks[index]->id;
        }
        if (!preview.target_track_id.empty()) {
            const DocumentTrack* targetTrack =
                document_.FindTrack(preview.target_track_id);
            RationalTime unsnapped = preview.timeline_in;
            std::optional<RationalTime> snappedBoundary;
            if (snapping_enabled_ && targetTrack) {
                const auto startSnap =
                    FindSnapTime(unsnapped, *targetTrack, clip->id, true);
                const RationalTime end = unsnapped.add(clip->duration);
                const auto endSnap = FindSnapTime(end, *targetTrack, clip->id);
                if (startSnap && endSnap) {
                    const double startDistance =
                        std::abs(viewport_.TimeToX(*startSnap) -
                                 viewport_.TimeToX(unsnapped));
                    const double endDistance = std::abs(
                        viewport_.TimeToX(*endSnap) - viewport_.TimeToX(end));
                    if (startDistance <= endDistance) {
                        preview.timeline_in = *startSnap;
                        snappedBoundary = startSnap;
                    } else {
                        preview.timeline_in = endSnap->sub(clip->duration);
                        snappedBoundary = endSnap;
                    }
                } else if (startSnap) {
                    preview.timeline_in = *startSnap;
                    snappedBoundary = startSnap;
                } else if (endSnap) {
                    preview.timeline_in = endSnap->sub(clip->duration);
                    snappedBoundary = endSnap;
                }
            }
            const auto buildOperation = [&](RationalTime anchorTimeline) {
                preview.linked_moves.clear();
                preview.link_group_id.clear();
                const bool linked = linked_selection_enabled_ &&
                                    selected_clip_ids_.size() > 1 &&
                                    !clip->link_group_id.empty();
                if (linked) {
                    const RationalTime groupDelta =
                        anchorTimeline.sub(clip->timeline_in);
                    for (const Ulid& id : selected_clip_ids_) {
                        const DocumentClip* member = document_.FindClip(id);
                        const DocumentTrack* memberTrack =
                            document_.FindTrackForClip(id);
                        if (!member || !memberTrack ||
                            member->link_group_id != clip->link_group_id)
                            continue;
                        preview.linked_moves.push_back(
                            {id,
                             id == clip->id ? preview.target_track_id
                                            : memberTrack->id,
                             member->timeline_in.add(groupDelta)});
                    }
                    if (preview.linked_moves.size() > 1) {
                        preview.link_group_id = clip->link_group_id;
                        return Operation{MoveLinkedClipsOperation{
                            clip->link_group_id, preview.linked_moves, {}}};
                    }
                }
                return Operation{MoveClipOperation{
                    clip->id, preview.target_track_id, anchorTimeline, {}}};
            };
            Document candidate = document_;
            Operation operation = buildOperation(preview.timeline_in);
            Operation inverse = RemoveClipOperation{};
            EditError error = EditError::None;
            std::string message;
            preview.valid =
                ApplyOperation(candidate, operation, inverse, error, message);
            if (!preview.valid && snappedBoundary) {
                // Never let magnetism turn an otherwise legal free drop into
                // an invalid one.
                candidate = document_;
                operation = buildOperation(unsnapped);
                preview.valid = ApplyOperation(candidate, operation, inverse,
                                               error, message);
                preview.timeline_in = unsnapped;
                snappedBoundary.reset();
            }
            if (preview.valid && snappedBoundary)
                snap_guide_time_ = snappedBoundary;
        }
        move_preview_ = preview;
        preview_.reset();
        return;
    }
    const TrimEdge edge = drag_hit_->edge == TimelineHitEdge::Head
                              ? TrimEdge::Head
                              : TrimEdge::Tail;
    const DocumentTrack* track = document_.FindTrackForClip(clip->id);
    const RationalTime originalBoundary =
        edge == TrimEdge::Head ? clip->timeline_in
                               : clip->timeline_in.add(clip->duration);
    RationalTime candidateBoundary = originalBoundary.add(delta);
    std::optional<RationalTime> snappedBoundary;
    if (snapping_enabled_ && track) {
        snappedBoundary = FindSnapTime(candidateBoundary, *track, clip->id);
        if (snappedBoundary) delta = snappedBoundary->sub(originalBoundary);
    }
    delta = ConstrainTrimDelta(*clip, edge, delta);
    candidateBoundary = originalBoundary.add(delta);
    if (snappedBoundary && candidateBoundary == *snappedBoundary)
        snap_guide_time_ = snappedBoundary;
    TimelineTrimPreview preview;
    preview.clip_id = clip->id;
    try {
        preview.times = ProposedTimes(*clip, edge, delta);
        Document candidate = document_;
        Operation operation =
            TrimClipOperation{clip->id, edge, delta, std::nullopt};
        if (linked_selection_enabled_ && !clip->link_group_id.empty()) {
            std::vector<LinkedClipTrim> trims;
            for (const Ulid& id : selected_clip_ids_) {
                const DocumentClip* member = document_.FindClip(id);
                if (member && member->link_group_id == clip->link_group_id)
                    trims.push_back({id, edge, delta});
            }
            if (trims.size() > 1) {
                preview.link_group_id = clip->link_group_id;
                operation = TrimLinkedClipsOperation{
                    clip->link_group_id, std::move(trims), {}};
            }
        }
        Operation inverse = RemoveClipOperation{};
        EditError error = EditError::None;
        std::string message;
        preview.valid =
            ApplyOperation(candidate, operation, inverse, error, message);
        if (!preview.link_group_id.empty()) {
            for (const Ulid& id : selected_clip_ids_) {
                const DocumentClip* member = preview.valid
                                                 ? candidate.FindClip(id)
                                                 : document_.FindClip(id);
                if (member && member->link_group_id == preview.link_group_id)
                    preview.linked_times.push_back(
                        {id, preview.valid
                                 ? ExactClipTimes{member->source_in,
                                                  member->duration,
                                                  member->timeline_in}
                                 : ProposedTimes(*member, edge, delta)});
            }
        }
    } catch (const std::exception&) {
        preview.times = {clip->source_in, clip->duration, clip->timeline_in};
        preview.valid = false;
    }
    preview_ = preview;
    move_preview_.reset();
}

RationalTime TimelineInteraction::ConstrainTrimDelta(const DocumentClip& clip,
                                                     TrimEdge edge,
                                                     RationalTime delta) const {
    const DocumentSource* source = document_.FindSource(clip.source_id);
    const DocumentTrack* track = document_.FindTrackForClip(clip.id);
    if (!source || !track) return delta;
    const RationalTime zero{0, 1};
    const RationalTime minimumDuration{1, drag_rate_};
    const auto found = std::find_if(
        track->clips.begin(), track->clips.end(),
        [&](const DocumentClip& candidate) { return candidate.id == clip.id; });
    const size_t index =
        static_cast<size_t>(std::distance(track->clips.begin(), found));

    RationalTime lower;
    RationalTime upper;
    if (edge == TrimEdge::Head) {
        lower = MaxTime(zero.sub(clip.source_in), zero.sub(clip.timeline_in));
        if (index > 0) {
            const DocumentClip& previous = track->clips[index - 1];
            lower = MaxTime(lower, previous.timeline_in.add(previous.duration)
                                       .sub(clip.timeline_in));
        }
        upper = clip.duration.sub(minimumDuration);
    } else {
        lower = minimumDuration.sub(clip.duration);
        upper = source->duration.sub(clip.source_in.add(clip.duration));
        if (index + 1 < track->clips.size()) {
            const DocumentClip& next = track->clips[index + 1];
            upper = MinTime(upper, next.timeline_in.sub(
                                       clip.timeline_in.add(clip.duration)));
        }
    }
    if (delta < lower) return lower;
    if (delta > upper) return upper;
    return delta;
}

std::optional<RationalTime> TimelineInteraction::FindSnapTime(
    RationalTime candidate, const DocumentTrack& track,
    const Ulid& excludedClipId, bool includeSyncTarget) const {
    std::optional<RationalTime> best;
    double bestDistance = kTimelineSnapDistance + 1.0;
    const auto consider = [&](RationalTime target) {
        const double distance =
            std::abs(viewport_.TimeToX(target) - viewport_.TimeToX(candidate));
        if (distance <= kTimelineSnapDistance && distance < bestDistance) {
            best = target;
            bestDistance = distance;
        }
    };
    consider({0, candidate.rate});
    if (includeSyncTarget && !linked_selection_enabled_) {
        const DocumentClip* clip = document_.FindClip(excludedClipId);
        const DocumentClip* anchor =
            clip ? document_.FindClip(clip->sync_anchor_clip_id) : nullptr;
        if (clip && anchor && anchor->id != clip->id &&
            anchor->link_group_id == clip->link_group_id) {
            try {
                const RationalTime anchorPhase =
                    anchor->timeline_in.sub(anchor->source_in);
                consider(clip->source_in.add(anchorPhase)
                             .add(clip->sync_reference_delta));
            } catch (const std::exception&) {
            }
        }
    }
    for (const DocumentClip& other : track.clips) {
        if (other.id == excludedClipId) continue;
        consider(other.timeline_in);
        consider(other.timeline_in.add(other.duration));
    }
    return best;
}

std::optional<Operation> TimelineInteraction::PendingOperation() const {
    if (!drag_hit_ || !drag_start_time_) return std::nullopt;
    if (drag_hit_->edge == TimelineHitEdge::None) {
        if (!move_preview_ || move_preview_->target_track_id.empty())
            return std::nullopt;
        const DocumentTrack* originalTrack =
            document_.FindTrackForClip(drag_hit_->clip_id);
        const DocumentClip* clip = document_.FindClip(drag_hit_->clip_id);
        if (!originalTrack || !clip) return std::nullopt;
        if (move_preview_->target_track_id == originalTrack->id &&
            move_preview_->timeline_in == clip->timeline_in)
            return std::nullopt;
        if (move_preview_->linked_moves.size() > 1)
            return Operation{MoveLinkedClipsOperation{
                move_preview_->link_group_id, move_preview_->linked_moves, {}}};
        return Operation{MoveClipOperation{drag_hit_->clip_id,
                                           move_preview_->target_track_id,
                                           move_preview_->timeline_in}};
    }
    const DocumentClip* clip = document_.FindClip(drag_hit_->clip_id);
    if (!clip || !preview_) return std::nullopt;
    const RationalTime delta =
        drag_hit_->edge == TimelineHitEdge::Head
            ? preview_->times.timeline_in.sub(clip->timeline_in)
            : preview_->times.duration.sub(clip->duration);
    if (delta.value == 0) return std::nullopt;
    if (!preview_->link_group_id.empty() && preview_->linked_times.size() > 1) {
        std::vector<LinkedClipTrim> trims;
        trims.reserve(preview_->linked_times.size());
        for (const auto& member : preview_->linked_times)
            trims.push_back({member.first,
                             drag_hit_->edge == TimelineHitEdge::Head
                                 ? TrimEdge::Head
                                 : TrimEdge::Tail,
                             delta});
        return Operation{TrimLinkedClipsOperation{
            preview_->link_group_id, std::move(trims), {}}};
    }
    return Operation{TrimClipOperation{drag_hit_->clip_id,
                                       drag_hit_->edge == TimelineHitEdge::Head
                                           ? TrimEdge::Head
                                           : TrimEdge::Tail,
                                       delta, std::nullopt}};
}

bool TimelineInteraction::PointerUp(EditError& error, std::string& message) {
    bool applied = false;
    const std::optional<Operation> operation = PendingOperation();
    const bool valid = (preview_ && preview_->valid) ||
                       (move_preview_ && move_preview_->valid);
    if (operation && valid)
        applied = edit_log_.Apply(document_, *operation, error, message);
    else {
        error = EditError::None;
        message.clear();
    }
    drag_hit_.reset();
    drag_start_time_.reset();
    preview_.reset();
    move_preview_.reset();
    snap_guide_time_.reset();
    return applied;
}

void TimelineInteraction::CancelDrag() {
    drag_hit_.reset();
    drag_start_time_.reset();
    preview_.reset();
    move_preview_.reset();
    snap_guide_time_.reset();
}

const Ulid& TimelineInteraction::SelectedClipId() const {
    return selected_clip_id_;
}

const std::vector<Ulid>& TimelineInteraction::SelectedClipIds() const {
    return selected_clip_ids_;
}

void TimelineInteraction::SelectClip(const Ulid& clipId) {
    selected_clip_id_ = document_.FindClip(clipId) ? clipId : Ulid{};
    selected_clip_ids_.clear();
    if (!selected_clip_id_.empty())
        selected_clip_ids_.push_back(selected_clip_id_);
    selected_gap_.reset();
}

void TimelineInteraction::SelectClips(const std::vector<Ulid>& clipIds) {
    selected_clip_ids_.clear();
    for (const Ulid& id : clipIds) {
        if (document_.FindClip(id) &&
            std::find(selected_clip_ids_.begin(), selected_clip_ids_.end(),
                      id) == selected_clip_ids_.end())
            selected_clip_ids_.push_back(id);
    }
    selected_clip_id_ =
        selected_clip_ids_.size() == 1 ? selected_clip_ids_.front() : Ulid{};
    selected_gap_.reset();
}

const std::optional<TimelineGapSelection>& TimelineInteraction::SelectedGap()
    const {
    return selected_gap_;
}

void TimelineInteraction::ClearGapSelection() { selected_gap_.reset(); }

const std::optional<RationalTime>& TimelineInteraction::RequestedPlayhead()
    const {
    return requested_playhead_;
}

void TimelineInteraction::ClearRequestedPlayhead() {
    requested_playhead_.reset();
}

const std::optional<TimelineTrimPreview>& TimelineInteraction::TrimPreview()
    const {
    return preview_;
}

const std::optional<TimelineMovePreview>& TimelineInteraction::MovePreview()
    const {
    return move_preview_;
}

bool TimelineInteraction::HasActiveDrag() const {
    return drag_hit_.has_value();
}

void TimelineInteraction::SetSnappingEnabled(bool enabled) {
    snapping_enabled_ = enabled;
    if (!enabled) snap_guide_time_.reset();
}

bool TimelineInteraction::SnappingEnabled() const { return snapping_enabled_; }

const std::optional<RationalTime>& TimelineInteraction::SnapGuideTime() const {
    return snap_guide_time_;
}

void TimelineInteraction::SetLinkedSelectionEnabled(bool enabled) {
    linked_selection_enabled_ = enabled;
}

std::vector<TimelineClipRect> VisibleTimelineClips(
    const Document& document, const TimelineViewport& viewport, double width,
    const std::vector<Ulid>& selectedClipIds,
    const std::optional<TimelineTrimPreview>& preview,
    const std::optional<TimelineMovePreview>& movePreview) {
    std::vector<TimelineClipRect> result;
    std::vector<TimelineClipRect> movingRects;
    const auto tracks = TimelineTracksInDisplayOrder(document);
    for (size_t trackIndex = 0; trackIndex < tracks.size(); ++trackIndex) {
        for (const DocumentClip& clip : tracks[trackIndex]->clips) {
            size_t displayTrackIndex = trackIndex;
            RationalTime start = clip.timeline_in;
            RationalTime duration = clip.duration;
            bool isPreview = false;
            bool valid = true;
            bool isMoving = false;
            if (preview && (preview->clip_id == clip.id ||
                            std::any_of(preview->linked_times.begin(),
                                        preview->linked_times.end(),
                                        [&](const auto& member) {
                                            return member.first == clip.id;
                                        }))) {
                const auto member = std::find_if(
                    preview->linked_times.begin(), preview->linked_times.end(),
                    [&](const auto& value) { return value.first == clip.id; });
                const ExactClipTimes& times =
                    member == preview->linked_times.end() ? preview->times
                                                          : member->second;
                start = times.timeline_in;
                duration = times.duration;
                isPreview = true;
                valid = preview->valid;
            } else if (movePreview) {
                const auto linked =
                    std::find_if(movePreview->linked_moves.begin(),
                                 movePreview->linked_moves.end(),
                                 [&](const LinkedClipMove& move) {
                                     return move.clip_id == clip.id;
                                 });
                const bool anchor = movePreview->clip_id == clip.id;
                if (!anchor && linked == movePreview->linked_moves.end()) {
                    // No preview for this clip.
                } else {
                    start = linked != movePreview->linked_moves.end()
                                ? linked->timeline_in
                                : movePreview->timeline_in;
                    isPreview = true;
                    valid = movePreview->valid;
                    isMoving = true;
                    const Ulid targetTrack =
                        linked != movePreview->linked_moves.end()
                            ? linked->track_id
                            : movePreview->target_track_id;
                    const auto target =
                        std::find_if(tracks.begin(), tracks.end(),
                                     [&](const DocumentTrack* track) {
                                         return track->id == targetTrack;
                                     });
                    if (target != tracks.end())
                        displayTrackIndex = static_cast<size_t>(
                            std::distance(tracks.begin(), target));
                }
            }
            const double left = viewport.TimeToX(start);
            const double right = viewport.TimeToX(start.add(duration));
            if (std::max(left, right) < viewport.header_width ||
                std::min(left, right) > width)
                continue;
            TimelineClipRect rect{
                clip.id,
                clip.source_id,
                left,
                kTimelineRulerHeight +
                    displayTrackIndex * viewport.track_height + 3.0,
                right - left,
                viewport.track_height - 6.0,
                std::find(selectedClipIds.begin(), selectedClipIds.end(),
                          clip.id) != selectedClipIds.end(),
                isPreview,
                valid,
                isMoving,
                tracks[displayTrackIndex]->kind == "audio",
                std::nullopt};
            if (rect.audio) {
                rect.sync_drift = ClipSyncDrift(document, clip.id);
                if (rect.sync_drift && isMoving && movePreview &&
                    movePreview->linked_moves.size() <= 1)
                    rect.sync_drift =
                        rect.sync_drift->add(start.sub(clip.timeline_in));
            }
            if (isMoving)
                movingRects.push_back(std::move(rect));
            else
                result.push_back(std::move(rect));
        }
    }
    // Ghosts are deliberately last, matching NLE stacking conventions and
    // keeping an overwrite destination readable above affected clips.
    for (TimelineClipRect& rect : movingRects)
        result.push_back(std::move(rect));
    return result;
}
