#include "TimelineView.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
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

void ClampViewportStartToZero(TimelineViewport& viewport) {
    if (viewport.view_start < RationalTime{0, 1})
        viewport.view_start = {0, viewport.view_start.rate};
}

double DominantScrollDelta(double deltaX, double deltaY) {
    if (!std::isfinite(deltaX) || !std::isfinite(deltaY)) return 0.0;
    return std::abs(deltaX) > 0.01 ? deltaX : deltaY;
}

long double Seconds(const RationalTime& time) {
    if (time.rate <= 0)
        throw std::invalid_argument("time rate must be positive");
    return static_cast<long double>(time.value) / time.rate;
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

std::vector<const DocumentTrack*> TracksOfKindInEditOrder(
    const Document& document, const std::string& kind) {
    std::vector<const DocumentTrack*> result;
    for (const DocumentTrack& track : document.sequence.tracks)
        if (track.kind == kind) result.push_back(&track);
    std::stable_sort(result.begin(), result.end(),
                     [](const DocumentTrack* left, const DocumentTrack* right) {
                         return left->index < right->index;
                     });
    return result;
}

std::optional<int64_t> TrackOrdinal(const Document& document,
                                    const DocumentTrack& track) {
    const auto tracks = TracksOfKindInEditOrder(document, track.kind);
    const auto found = std::find(tracks.begin(), tracks.end(), &track);
    if (found == tracks.end()) return std::nullopt;
    return static_cast<int64_t>(std::distance(tracks.begin(), found));
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
    ClampViewportStartToZero(*this);
}

void TimelineViewport::ZoomAroundX(double x, double factor, int32_t rate) {
    if (!std::isfinite(factor) || factor <= 0.0)
        throw std::invalid_argument("zoom factor must be positive");
    const RationalTime anchor = XToTime(x, rate);
    pixels_per_second = std::clamp(pixels_per_second * factor, 4.0, 4000.0);
    const RationalTime displaced = XToTime(x, rate);
    view_start = view_start.add(anchor.sub(displaced));
    ClampViewportStartToZero(*this);
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

TimelineScrollIntent ResolveTimelineScrollIntent(double deltaX, double deltaY,
                                                 bool preciseDeltas,
                                                 bool shiftPressed) {
    const double delta = DominantScrollDelta(deltaX, deltaY);
    if (preciseDeltas || shiftPressed) {
        // Precise deltas already use logical points. A discrete wheel reports
        // line-sized units, so give Shift+wheel a useful navigation step.
        return {TimelineScrollAction::Pan,
                preciseDeltas ? delta : delta * 32.0};
    }
    return {TimelineScrollAction::Zoom, delta};
}

TimelineZoomBarPart TimelineZoomBarGeometry::HitTest(double x) const {
    if (!std::isfinite(x) || rail_width <= 0.0 || thumb_width <= 0.0 ||
        x < thumb_x || x > thumb_x + thumb_width)
        return TimelineZoomBarPart::None;
    const double hitWidth =
        std::min(thumb_width * 0.5, std::max(8.0, handle_width * 2.0));
    if (x <= thumb_x + hitWidth) return TimelineZoomBarPart::LeftHandle;
    if (x >= thumb_x + thumb_width - hitWidth)
        return TimelineZoomBarPart::RightHandle;
    return TimelineZoomBarPart::Body;
}

TimelineZoomBarGeometry CalculateTimelineZoomBarGeometry(
    const TimelineViewport& viewport, RationalTime duration, double width) {
    CheckViewport(viewport);
    TimelineZoomBarGeometry geometry;
    geometry.rail_x = viewport.header_width;
    geometry.rail_width = std::max(0.0, width - viewport.header_width);
    geometry.thumb_x = geometry.rail_x;
    geometry.thumb_width = geometry.rail_width;
    if (geometry.rail_width <= 0.0) return geometry;

    const long double durationSeconds = std::max(0.0L, Seconds(duration));
    const long double visibleSeconds =
        geometry.rail_width / viewport.pixels_per_second;
    const long double rulerSeconds = std::max(durationSeconds, visibleSeconds);
    if (rulerSeconds <= 0.0L) return geometry;

    const double rawWidth = geometry.rail_width *
                            static_cast<double>(visibleSeconds / rulerSeconds);
    geometry.thumb_width = std::clamp(
        rawWidth, std::min(24.0, geometry.rail_width), geometry.rail_width);
    const long double maximumStart =
        std::max(0.0L, rulerSeconds - visibleSeconds);
    if (maximumStart <= 0.0L) return geometry;
    const long double start =
        std::clamp(Seconds(viewport.view_start), 0.0L, maximumStart);
    geometry.thumb_x += (geometry.rail_width - geometry.thumb_width) *
                        static_cast<double>(start / maximumStart);
    return geometry;
}

void UpdateTimelineZoomBarDrag(TimelineViewport& viewport,
                               const TimelineZoomBarDrag& drag, double pointerX,
                               RationalTime duration, double width,
                               int32_t rate) {
    if (!std::isfinite(pointerX) || rate <= 0 ||
        drag.part == TimelineZoomBarPart::None)
        return;
    viewport = drag.initial_viewport;
    const double delta = pointerX - drag.pointer_x;
    const TimelineZoomBarGeometry& geometry = drag.initial_geometry;
    if (geometry.rail_width <= 0.0) return;

    if (drag.part == TimelineZoomBarPart::Body) {
        const double travel = geometry.rail_width - geometry.thumb_width;
        const long double visibleSeconds =
            geometry.rail_width / viewport.pixels_per_second;
        const long double maximumStart =
            std::max(0.0L, Seconds(duration) - visibleSeconds);
        if (travel <= 0.0 || maximumStart <= 0.0L) return;
        const long double initialStart = Seconds(viewport.view_start);
        const long double requested =
            std::clamp(initialStart + static_cast<long double>(delta / travel) *
                                          maximumStart,
                       0.0L, maximumStart);
        viewport.view_start = {RoundedInt64(requested * rate), rate};
        return;
    }

    const double minimumWidth = std::min(24.0, geometry.rail_width);
    double requestedWidth = geometry.thumb_width;
    double anchorX = viewport.header_width;
    if (drag.part == TimelineZoomBarPart::LeftHandle) {
        requestedWidth -= delta;
        anchorX = width;
    } else {
        requestedWidth += delta;
    }
    requestedWidth =
        std::clamp(requestedWidth, minimumWidth, geometry.rail_width);
    if (requestedWidth >= geometry.rail_width - 0.5) {
        viewport.FitDuration(duration, width);
        return;
    }
    if (geometry.thumb_width <= 0.0) return;
    viewport.ZoomAroundX(anchorX, geometry.thumb_width / requestedWidth, rate);
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
    tracks.reserve(document.sequence.tracks.size());
    for (const DocumentTrack& track : document.sequence.tracks)
        tracks.push_back(&track);
    std::stable_sort(tracks.begin(), tracks.end(),
                     [](const DocumentTrack* a, const DocumentTrack* b) {
                         if (a->kind != b->kind) return a->kind == "video";
                         // Like Resolve: the highest video layer is displayed
                         // at the top, while audio counts downward from A1.
                         return a->kind == "video" ? a->index > b->index
                                                   : a->index < b->index;
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
    for (const DocumentTrack& track : document.sequence.tracks) {
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

Operation TimelineCutOperationForClip(const Document& document,
                                      const DocumentClip& clip,
                                      const RationalTime& position,
                                      bool linkedSelection) {
    if (linkedSelection && !clip.link_group_id.empty()) {
        std::vector<Ulid> members =
            ExpandLinkedClipSelection(document, {clip.id});
        members.erase(
            std::remove_if(
                members.begin(), members.end(),
                [&](const Ulid& id) {
                    const DocumentClip* member = document.FindClip(id);
                    return !member || position <= member->timeline_in ||
                           position >=
                               member->timeline_in.add(member->duration);
                }),
            members.end());
        if (members.size() > 1)
            return SplitLinkedClipsOperation{
                clip.link_group_id, members, position, {}, {}, {}, {}};
    }
    return SplitClipOperation{clip.id, position, {}};
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

std::vector<RationalTime> TimelineEditPoints(const Document& document) {
    std::vector<RationalTime> points;
    points.push_back({0, document.sequence.frame_rate.num});
    for (const DocumentTrack& track : document.sequence.tracks) {
        for (const DocumentClip& clip : track.clips) {
            points.push_back(clip.timeline_in);
            points.push_back(clip.timeline_in.add(clip.duration));
        }
    }
    std::sort(points.begin(), points.end());
    points.erase(std::unique(points.begin(), points.end()), points.end());
    return points;
}

std::optional<RationalTime> AdjacentTimelineEdit(const Document& document,
                                                 const RationalTime& position,
                                                 bool forward) {
    const std::vector<RationalTime> points = TimelineEditPoints(document);
    if (forward) {
        const auto next =
            std::upper_bound(points.begin(), points.end(), position);
        return next == points.end() ? std::nullopt
                                    : std::optional<RationalTime>(*next);
    }
    const auto previous =
        std::lower_bound(points.begin(), points.end(), position);
    if (previous == points.begin()) return std::nullopt;
    return *std::prev(previous);
}

std::optional<RationalTime> SnapTimelineTimeToEdit(
    const Document& document, const TimelineViewport& viewport,
    const RationalTime& candidate) {
    std::optional<RationalTime> best;
    double bestDistance = kTimelineSnapDistance + 1.0;
    for (const RationalTime& point : TimelineEditPoints(document)) {
        const double distance =
            std::abs(viewport.TimeToX(point) - viewport.TimeToX(candidate));
        if (distance <= kTimelineSnapDistance && distance < bestDistance) {
            best = point;
            bestDistance = distance;
        }
    }
    return best;
}

TimelineInteraction::TimelineInteraction(Document& document, EditLog& editLog,
                                         TimelineViewport& viewport)
    : document_(document), edit_log_(editLog), viewport_(viewport) {}

void TimelineInteraction::PointerDown(double x, double y, double width,
                                      int32_t timeRate) {
    requested_playhead_.reset();
    preview_.reset();
    trim_operation_.reset();
    move_preview_.reset();
    snap_guide_time_.reset();
    drag_hit_.reset();
    drag_start_time_.reset();
    auto hit = HitTestTimeline(document_, viewport_, x, y, width);
    if (!hit) {
        selected_gap_ =
            HitTestTimelineGap(document_, viewport_, x, y, width, timeRate);
        if (x >= viewport_.header_width && y >= 0.0) {
            requested_playhead_ = viewport_.XToTime(x, timeRate);
        }
        selected_clip_id_.clear();
        primary_clip_id_.clear();
        selected_clip_ids_.clear();
        return;
    }
    if (trim_mode_ == TimelineTrimMode::Slip) hit->edge = TimelineHitEdge::None;
    selected_gap_.reset();
    selected_clip_id_ = hit->clip_id;
    primary_clip_id_ = hit->clip_id;
    selected_clip_ids_ = {hit->clip_id};
    const DocumentTrack* selectedTrack = document_.FindTrack(hit->track_id);
    if (selectedTrack && selectedTrack->locked) return;
    if (linked_selection_enabled_) {
        std::vector<Ulid> linked =
            ExpandLinkedClipSelection(document_, selected_clip_ids_);
        const bool lockedPartner =
            hit->edge == TimelineHitEdge::None &&
            std::any_of(linked.begin(), linked.end(), [&](const Ulid& id) {
                const DocumentTrack* track = document_.FindTrackForClip(id);
                return id != hit->clip_id && track && track->locked;
            });
        // A locked linked partner is a hard boundary, not a reason to make an
        // otherwise editable anchor immovable. For this body-move gesture,
        // temporarily detach the anchor from linked editing; the persistent
        // link group itself is unchanged.
        if (!lockedPartner) selected_clip_ids_ = std::move(linked);
    }
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
        if (trim_mode_ == TimelineTrimMode::Slip) {
            std::vector<Ulid> clipIds{clip->id};
            if (linked_selection_enabled_ && !clip->link_group_id.empty()) {
                for (const Ulid& id : selected_clip_ids_) {
                    const DocumentClip* member = document_.FindClip(id);
                    if (id != clip->id && member &&
                        member->link_group_id == clip->link_group_id)
                        clipIds.push_back(id);
                }
            }
            RationalTime constrained = delta;
            for (const Ulid& id : clipIds) {
                const DocumentClip* member = document_.FindClip(id);
                const DocumentSource* source =
                    member ? document_.FindSource(member->source_id) : nullptr;
                if (!member || !source) continue;
                const RationalTime minimum =
                    RationalTime{0, member->source_in.rate}.sub(
                        member->source_in);
                const RationalTime maximum = source->duration.sub(
                    member->source_in.add(member->duration));
                if (constrained < minimum) constrained = minimum;
                if (constrained > maximum) constrained = maximum;
            }
            TimelineTrimPreview preview;
            preview.clip_id = clip->id;
            preview.link_group_id =
                clipIds.size() > 1 ? clip->link_group_id : Ulid{};
            Document candidate = document_;
            Operation operation = SlipEditOperation{clipIds, constrained, {}};
            Operation inverse = RemoveClipOperation{};
            EditError error = EditError::None;
            std::string message;
            preview.valid =
                ApplyOperation(candidate, operation, inverse, error, message);
            for (const Ulid& id : clipIds) {
                const DocumentClip* changed = candidate.FindClip(id);
                if (!changed) continue;
                const ExactClipTimes times{changed->source_in,
                                           changed->duration,
                                           changed->timeline_in};
                if (id == clip->id) preview.times = times;
                preview.linked_times.push_back({id, times});
            }
            if (preview.valid && constrained.value != 0)
                trim_operation_ =
                    SlipEditOperation{std::move(clipIds), constrained, {}};
            else
                trim_operation_.reset();
            preview_ = std::move(preview);
            move_preview_.reset();
            return;
        }
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
            if ((snapping_enabled_ || playhead_snap_target_) && targetTrack) {
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
                    const DocumentTrack* anchorTrack =
                        document_.FindTrackForClip(clip->id);
                    const DocumentTrack* anchorTarget =
                        document_.FindTrack(preview.target_track_id);
                    std::optional<int64_t> trackOffset;
                    if (anchorTrack && anchorTarget &&
                        anchorTrack->kind == anchorTarget->kind) {
                        const auto sourceOrdinal =
                            TrackOrdinal(document_, *anchorTrack);
                        const auto targetOrdinal =
                            TrackOrdinal(document_, *anchorTarget);
                        if (sourceOrdinal && targetOrdinal)
                            trackOffset = *targetOrdinal - *sourceOrdinal;
                    }
                    for (const Ulid& id : selected_clip_ids_) {
                        const DocumentClip* member = document_.FindClip(id);
                        const DocumentTrack* memberTrack =
                            document_.FindTrackForClip(id);
                        if (!member || !memberTrack ||
                            member->link_group_id != clip->link_group_id)
                            continue;
                        Ulid destinationTrack;
                        if (trackOffset) {
                            const auto memberOrdinal =
                                TrackOrdinal(document_, *memberTrack);
                            const auto compatibleTracks =
                                TracksOfKindInEditOrder(document_,
                                                        memberTrack->kind);
                            if (memberOrdinal) {
                                const int64_t destinationOrdinal =
                                    *memberOrdinal + *trackOffset;
                                if (destinationOrdinal >= 0 &&
                                    static_cast<size_t>(destinationOrdinal) <
                                        compatibleTracks.size())
                                    destinationTrack =
                                        compatibleTracks
                                            [static_cast<size_t>(
                                                 destinationOrdinal)]
                                                ->id;
                            }
                        }
                        // An empty destination deliberately makes the atomic
                        // candidate invalid when one linked kind has no
                        // corresponding Vn/An track. Never leave the partner
                        // silently behind on its original track.
                        preview.linked_moves.push_back(
                            {id, destinationTrack,
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
    if ((snapping_enabled_ || playhead_snap_target_) && track) {
        snappedBoundary = FindSnapTime(candidateBoundary, *track, clip->id);
        if (snappedBoundary) delta = snappedBoundary->sub(originalBoundary);
    }
    if (trim_mode_ == TimelineTrimMode::Normal)
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
        if (trim_mode_ == TimelineTrimMode::Ripple) {
            std::vector<Ulid> linkedIds;
            if (linked_selection_enabled_ && !clip->link_group_id.empty()) {
                for (const Ulid& id : selected_clip_ids_)
                    if (id != clip->id) linkedIds.push_back(id);
            }
            std::vector<Ulid> syncTracks;
            for (const DocumentTrack& candidateTrack :
                 document_.sequence.tracks)
                if (candidateTrack.sync_lock)
                    syncTracks.push_back(candidateTrack.id);
            operation = RippleTrimOperation{clip->id,
                                            edge,
                                            delta,
                                            std::move(linkedIds),
                                            std::move(syncTracks),
                                            {}};
        } else if (trim_mode_ == TimelineTrimMode::Roll) {
            const RationalTime cut = originalBoundary;
            const DocumentClip* left = nullptr;
            const DocumentClip* right = nullptr;
            if (track) {
                for (const DocumentClip& candidateClip : track->clips) {
                    if (candidateClip.timeline_in.add(candidateClip.duration) ==
                        cut)
                        left = &candidateClip;
                    if (candidateClip.timeline_in == cut)
                        right = &candidateClip;
                }
            }
            std::vector<RollEditPair> pairs;
            if (left && right && left->id != right->id) {
                pairs.push_back({left->id, right->id});
                if (linked_selection_enabled_ && !left->link_group_id.empty() &&
                    !right->link_group_id.empty()) {
                    for (const DocumentTrack& candidateTrack :
                         document_.sequence.tracks) {
                        if (track && candidateTrack.id == track->id) continue;
                        const DocumentClip* linkedLeft = nullptr;
                        const DocumentClip* linkedRight = nullptr;
                        for (const DocumentClip& candidateClip :
                             candidateTrack.clips) {
                            if (candidateClip.link_group_id ==
                                    left->link_group_id &&
                                candidateClip.timeline_in.add(
                                    candidateClip.duration) == cut)
                                linkedLeft = &candidateClip;
                            if (candidateClip.link_group_id ==
                                    right->link_group_id &&
                                candidateClip.timeline_in == cut)
                                linkedRight = &candidateClip;
                        }
                        if (linkedLeft && linkedRight)
                            pairs.push_back({linkedLeft->id, linkedRight->id});
                    }
                }
            }
            operation = RollEditOperation{std::move(pairs), delta, {}};
        } else if (linked_selection_enabled_ && !clip->link_group_id.empty()) {
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
        const Operation pendingOperation = operation;
        Operation inverse = RemoveClipOperation{};
        EditError error = EditError::None;
        std::string message;
        preview.valid =
            ApplyOperation(candidate, operation, inverse, error, message);
        if (preview.valid) {
            const DocumentClip* changed = candidate.FindClip(clip->id);
            if (changed)
                preview.times = {changed->source_in, changed->duration,
                                 changed->timeline_in};
            for (const DocumentTrack& candidateTrack :
                 candidate.sequence.tracks) {
                for (const DocumentClip& changedClip : candidateTrack.clips) {
                    if (changedClip.id == clip->id) continue;
                    const DocumentClip* original =
                        document_.FindClip(changedClip.id);
                    if (original &&
                        (original->source_in != changedClip.source_in ||
                         original->duration != changedClip.duration ||
                         original->timeline_in != changedClip.timeline_in))
                        preview.linked_times.push_back(
                            {changedClip.id,
                             {changedClip.source_in, changedClip.duration,
                              changedClip.timeline_in}});
                }
            }
            if (delta.value != 0)
                trim_operation_ = pendingOperation;
            else
                trim_operation_.reset();
        } else {
            trim_operation_.reset();
        }
        if (!preview.link_group_id.empty()) {
            for (const Ulid& id : selected_clip_ids_) {
                if (std::any_of(
                        preview.linked_times.begin(),
                        preview.linked_times.end(),
                        [&](const auto& linked) { return linked.first == id; }))
                    continue;
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
        trim_operation_.reset();
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
    if (playhead_snap_target_) consider(*playhead_snap_target_);
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
    if (trim_operation_) return trim_operation_;
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
    trim_operation_.reset();
    move_preview_.reset();
    snap_guide_time_.reset();
    return applied;
}

void TimelineInteraction::CancelDrag() {
    drag_hit_.reset();
    drag_start_time_.reset();
    preview_.reset();
    trim_operation_.reset();
    move_preview_.reset();
    snap_guide_time_.reset();
}

const Ulid& TimelineInteraction::SelectedClipId() const {
    return selected_clip_id_;
}

const Ulid& TimelineInteraction::PrimarySelectedClipId() const {
    return primary_clip_id_;
}

const std::vector<Ulid>& TimelineInteraction::SelectedClipIds() const {
    return selected_clip_ids_;
}

void TimelineInteraction::SelectClip(const Ulid& clipId) {
    selected_clip_id_ = document_.FindClip(clipId) ? clipId : Ulid{};
    primary_clip_id_ = selected_clip_id_;
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
    // The anchor is the caller's first surviving id. Callers that expand a
    // click through ExpandLinkedClipSelection pass the clicked clip first
    // and that function preserves input order, so this is the clip the user
    // actually pointed at rather than whichever linked clip sorts first.
    primary_clip_id_ =
        selected_clip_ids_.empty() ? Ulid{} : selected_clip_ids_.front();
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

void TimelineInteraction::SetPlayheadSnapTarget(
    std::optional<RationalTime> target) {
    playhead_snap_target_ = std::move(target);
}

const std::optional<RationalTime>& TimelineInteraction::SnapGuideTime() const {
    return snap_guide_time_;
}

void TimelineInteraction::SetLinkedSelectionEnabled(bool enabled) {
    linked_selection_enabled_ = enabled;
}

void TimelineInteraction::SetTrimMode(TimelineTrimMode mode) {
    trim_mode_ = mode;
}

TimelineTrimMode TimelineInteraction::TrimMode() const { return trim_mode_; }

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
                    displayTrackIndex * viewport.track_height + 1.5,
                right - left,
                viewport.track_height - 3.0,
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
    // Moving candidates are deliberately last, matching NLE stacking
    // conventions and keeping the opaque overwrite result above affected
    // clips.
    for (TimelineClipRect& rect : movingRects)
        result.push_back(std::move(rect));
    return result;
}

std::optional<DeleteGapOperation> TimelineRangeDeleteOperation(
    const Document& document, const RationalTime& rangeIn,
    const RationalTime& rangeOut, bool ripple) {
    std::vector<Ulid> allTracks;
    allTracks.reserve(document.sequence.tracks.size());
    for (const DocumentTrack& track : document.sequence.tracks)
        allTracks.push_back(track.id);
    return TimelineRangeDeleteOperation(document, rangeIn, rangeOut, ripple,
                                        allTracks);
}

std::optional<DeleteGapOperation> TimelineRangeDeleteOperation(
    const Document& document, const RationalTime& rangeIn,
    const RationalTime& rangeOut, bool ripple,
    const std::vector<Ulid>& targetedTrackIds) {
    if (rangeOut <= rangeIn) return std::nullopt;
    const std::set<Ulid> targeted(targetedTrackIds.begin(),
                                  targetedTrackIds.end());
    const bool hasEditableTarget = std::any_of(
        document.sequence.tracks.begin(), document.sequence.tracks.end(),
        [&](const DocumentTrack& track) {
            return !track.locked && targeted.count(track.id) != 0;
        });
    if (!hasEditableTarget) return std::nullopt;
    const RationalTime rangeDuration = rangeOut.sub(rangeIn);
    std::map<Ulid, Ulid> leftGroups;
    std::map<Ulid, Ulid> rightGroups;
    const auto sideGroup = [](std::map<Ulid, Ulid>& groups,
                              const Ulid& original) -> Ulid {
        if (original.empty()) return {};
        auto [item, inserted] = groups.emplace(original, Ulid{});
        if (inserted) item->second = GenerateUlid();
        return item->second;
    };
    std::vector<ExactTrackState> results;
    for (const DocumentTrack& track : document.sequence.tracks) {
        const bool directlyTargeted = targeted.count(track.id) != 0;
        if (track.locked || (!directlyTargeted && !(ripple && track.sync_lock)))
            continue;
        std::vector<DocumentClip> clips;
        bool changed = false;
        for (const DocumentClip& original : track.clips) {
            const RationalTime clipStart = original.timeline_in;
            const RationalTime clipEnd = clipStart.add(original.duration);
            if (clipEnd <= rangeIn) {
                clips.push_back(original);
                continue;
            }
            if (clipStart >= rangeOut) {
                DocumentClip shifted = original;
                if (ripple) {
                    shifted.timeline_in = clipStart.sub(rangeDuration);
                    changed = true;
                }
                clips.push_back(std::move(shifted));
                continue;
            }
            changed = true;
            const bool hasLeft = clipStart < rangeIn;
            const bool hasRight = clipEnd > rangeOut;
            if (hasLeft) {
                DocumentClip left = original;
                left.duration = rangeIn.sub(clipStart);
                if (hasRight)
                    left.link_group_id =
                        sideGroup(leftGroups, original.link_group_id);
                clips.push_back(std::move(left));
            }
            if (hasRight) {
                DocumentClip right = original;
                if (hasLeft) right.id = GenerateUlid();
                right.source_in =
                    original.source_in.add(rangeOut.sub(clipStart));
                right.duration = clipEnd.sub(rangeOut);
                right.timeline_in = ripple ? rangeIn : rangeOut;
                if (hasLeft)
                    right.link_group_id =
                        sideGroup(rightGroups, original.link_group_id);
                clips.push_back(std::move(right));
            }
        }
        if (!changed) continue;
        std::sort(clips.begin(), clips.end(),
                  [](const DocumentClip& a, const DocumentClip& b) {
                      return a.timeline_in < b.timeline_in;
                  });
        results.push_back({track.id, std::move(clips)});
    }
    if (results.empty()) return std::nullopt;
    DeleteGapOperation operation{
        results.front().track_id, rangeIn, rangeDuration, results, {}};
    for (size_t index = 1; index < results.size(); ++index)
        operation.linked_track_ids.push_back(results[index].track_id);
    return operation;
}

std::optional<DeleteGapOperation> BuildTimelineRippleDeleteSelection(
    const Document& document, const std::vector<Ulid>& clipIds) {
    std::optional<RationalTime> rangeIn;
    std::optional<RationalTime> rangeOut;
    std::vector<Ulid> trackIds;
    for (const Ulid& id : clipIds) {
        const DocumentClip* clip = document.FindClip(id);
        const DocumentTrack* track = document.FindTrackForClip(id);
        if (!clip || !track) continue;
        const RationalTime end = clip->timeline_in.add(clip->duration);
        if (!rangeIn || clip->timeline_in < *rangeIn)
            rangeIn = clip->timeline_in;
        if (!rangeOut || end > *rangeOut) rangeOut = end;
        if (std::find(trackIds.begin(), trackIds.end(), track->id) ==
            trackIds.end())
            trackIds.push_back(track->id);
    }
    if (!rangeIn || !rangeOut) return std::nullopt;
    return TimelineRangeDeleteOperation(document, *rangeIn, *rangeOut, true,
                                        trackIds);
}

std::optional<DeleteGapOperation> TimelineSourceEditOperation(
    const Document& document, const Ulid& sourceId,
    const RationalTime& sourceIn, const RationalTime& sourceOut,
    const RationalTime& timelineIn, const Ulid& targetTrackId, bool insert,
    const std::vector<Ulid>& audioTargetTrackIds) {
    const DocumentSource* source = document.FindSource(sourceId);
    const DocumentTrack* target = document.FindTrack(targetTrackId);
    if (!source || !target || target->locked || target->kind != "video" ||
        timelineIn.rate <= 0 || timelineIn.value < 0 || sourceIn.rate <= 0 ||
        sourceOut <= sourceIn || sourceIn.value < 0 ||
        sourceOut > source->duration)
        return std::nullopt;
    const std::set<Ulid> audioTargets(audioTargetTrackIds.begin(),
                                      audioTargetTrackIds.end());
    if (audioTargets.size() != audioTargetTrackIds.size() ||
        std::any_of(
            audioTargets.begin(), audioTargets.end(), [&](const Ulid& id) {
                const DocumentTrack* track = document.FindTrack(id);
                return !track || track->locked || track->kind != "audio";
            }))
        return std::nullopt;
    const RationalTime duration = sourceOut.sub(sourceIn);
    const RationalTime timelineOut = timelineIn.add(duration);
    const Ulid videoClipId = GenerateUlid();
    const Ulid linkGroupId = audioTargets.empty() ? Ulid{} : GenerateUlid();
    std::vector<ExactTrackState> results;

    for (const DocumentTrack& track : document.sequence.tracks) {
        const bool isTarget = track.id == targetTrackId;
        const bool isAudioTarget = audioTargets.count(track.id) != 0;
        const bool directlyTargeted = isTarget || isAudioTarget;
        if (track.locked || (!directlyTargeted && !(insert && track.sync_lock)))
            continue;
        std::vector<DocumentClip> clips;
        for (const DocumentClip& original : track.clips) {
            const RationalTime start = original.timeline_in;
            const RationalTime end = start.add(original.duration);
            if (insert) {
                if (end <= timelineIn) {
                    clips.push_back(original);
                } else if (start >= timelineIn) {
                    DocumentClip shifted = original;
                    shifted.timeline_in = start.add(duration);
                    clips.push_back(std::move(shifted));
                } else {
                    DocumentClip left = original;
                    left.duration = timelineIn.sub(start);
                    clips.push_back(std::move(left));
                    DocumentClip right = original;
                    right.id = GenerateUlid();
                    right.source_in =
                        original.source_in.add(timelineIn.sub(start));
                    right.duration = end.sub(timelineIn);
                    right.timeline_in = timelineIn.add(duration);
                    right.link_group_id.clear();
                    right.sync_anchor_clip_id.clear();
                    right.sync_reference_delta = {0, 1};
                    clips.push_back(std::move(right));
                }
                continue;
            }

            if (end <= timelineIn || start >= timelineOut) {
                clips.push_back(original);
                continue;
            }
            if (start < timelineIn) {
                DocumentClip left = original;
                left.duration = timelineIn.sub(start);
                clips.push_back(std::move(left));
            }
            if (end > timelineOut) {
                DocumentClip right = original;
                if (start < timelineIn) right.id = GenerateUlid();
                right.source_in =
                    original.source_in.add(timelineOut.sub(start));
                right.duration = end.sub(timelineOut);
                right.timeline_in = timelineOut;
                if (start < timelineIn) {
                    right.link_group_id.clear();
                    right.sync_anchor_clip_id.clear();
                    right.sync_reference_delta = {0, 1};
                }
                clips.push_back(std::move(right));
            }
        }
        if (directlyTargeted) {
            DocumentClip inserted{isTarget ? videoClipId : GenerateUlid(),
                                  sourceId, sourceIn, duration, timelineIn};
            if (isTarget) inserted.include_audio = false;
            if (!audioTargets.empty()) {
                inserted.link_group_id = linkGroupId;
                inserted.sync_anchor_clip_id = videoClipId;
                inserted.sync_reference_delta = {0, 1};
            }
            clips.push_back(std::move(inserted));
        }
        std::stable_sort(
            clips.begin(), clips.end(),
            [](const DocumentClip& left, const DocumentClip& right) {
                return left.timeline_in < right.timeline_in;
            });
        results.push_back({track.id, std::move(clips)});
    }
    if (results.empty()) return std::nullopt;
    const auto targetResult = std::find_if(
        results.begin(), results.end(), [&](const ExactTrackState& state) {
            return state.track_id == targetTrackId;
        });
    if (targetResult == results.end()) return std::nullopt;
    DeleteGapOperation operation{
        targetTrackId, timelineIn, duration, results, {}};
    for (const ExactTrackState& state : results)
        if (state.track_id != targetTrackId)
            operation.linked_track_ids.push_back(state.track_id);
    return operation;
}
