#pragma once

#include "Document.h"
#include "EditLog.h"

#include <optional>
#include <vector>

// The only boundary between exact timeline time and display coordinates.
// Coordinates are in logical points; backing-scale conversion belongs to the
// renderer and never affects edit rounding.
struct TimelineViewport {
    RationalTime view_start{0, 1};
    double pixels_per_second = 100.0;
    double track_height = 44.0;
    double header_width = 72.0;

    double TimeToX(RationalTime time) const;
    RationalTime XToTime(double x, int32_t rate) const;

    void ScrollByPixels(double delta_x, int32_t rate);
    void ZoomAroundX(double x, double factor, int32_t rate);
    void FitDuration(RationalTime duration, double width);
    std::vector<double> TickXs(double width) const;
};

constexpr double kTimelineRulerHeight = 24.0;
constexpr double kTimelineEdgeHitWidth = 6.0;
constexpr double kTimelineSnapDistance = 8.0;

enum class PlayheadResolution { Frame, Sample };

RationalTime QuantizePlayheadPosition(RationalTime position,
                                      PlayheadResolution resolution,
                                      MediaRate frameRate,
                                      int32_t sampleRate = 48000);

enum class TimelineHitEdge { None, Head, Tail };

struct TimelineHit {
    Ulid clip_id;
    Ulid track_id;
    TimelineHitEdge edge = TimelineHitEdge::None;
};

struct TimelineGapSelection {
    Ulid track_id;
    RationalTime start;
    RationalTime duration;
};

struct TimelineClipRect {
    Ulid clip_id;
    Ulid source_id;
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    bool selected = false;
    bool preview = false;
    bool valid = true;
    bool moving = false;
    bool audio = false;
    std::optional<RationalTime> sync_drift;
};

std::vector<const DocumentTrack*> TimelineTracksInDisplayOrder(
    const Document& document);

std::optional<TimelineHit> HitTestTimeline(const Document& document,
                                           const TimelineViewport& viewport,
                                           double x, double y, double width);
std::optional<TimelineGapSelection> HitTestTimelineGap(
    const Document& document, const TimelineViewport& viewport, double x,
    double y, double width, int32_t timeRate);
std::vector<Ulid> LassoHitTestTimeline(const Document& document,
                                       const TimelineViewport& viewport,
                                       double startX, double startY,
                                       double endX, double endY, double width);
std::vector<Ulid> ExpandLinkedClipSelection(const Document& document,
                                            const std::vector<Ulid>& clipIds);

struct TimelineTrimPreview {
    Ulid clip_id;
    ExactClipTimes times;
    bool valid = false;
    Ulid link_group_id;
    std::vector<std::pair<Ulid, ExactClipTimes>> linked_times;
};

struct TimelineMovePreview {
    Ulid clip_id;
    Ulid target_track_id;
    RationalTime timeline_in;
    bool valid = false;
    Ulid link_group_id;
    std::vector<LinkedClipMove> linked_moves;
};

std::optional<RationalTime> ClipSyncDrift(const Document& document,
                                          const Ulid& clipId);

class TimelineInteraction {
public:
    TimelineInteraction(Document& document, EditLog& editLog,
                        TimelineViewport& viewport);

    void PointerDown(double x, double y, double width, int32_t timeRate);
    void PointerDrag(double x, double y, double width);
    bool PointerUp(EditError& error, std::string& message);
    void CancelDrag();

    const Ulid& SelectedClipId() const;
    const std::vector<Ulid>& SelectedClipIds() const;
    void SelectClip(const Ulid& clipId);
    void SelectClips(const std::vector<Ulid>& clipIds);
    const std::optional<TimelineGapSelection>& SelectedGap() const;
    void ClearGapSelection();
    const std::optional<RationalTime>& RequestedPlayhead() const;
    void ClearRequestedPlayhead();
    const std::optional<TimelineTrimPreview>& TrimPreview() const;
    const std::optional<TimelineMovePreview>& MovePreview() const;
    bool HasActiveDrag() const;
    void SetSnappingEnabled(bool enabled);
    bool SnappingEnabled() const;
    const std::optional<RationalTime>& SnapGuideTime() const;
    void SetLinkedSelectionEnabled(bool enabled);
    std::optional<Operation> PendingOperation() const;

private:
    void UpdatePreview(double x, double y, double width);
    RationalTime ConstrainTrimDelta(const DocumentClip& clip, TrimEdge edge,
                                    RationalTime delta) const;
    std::optional<RationalTime> FindSnapTime(
        RationalTime candidate, const DocumentTrack& track,
        const Ulid& excludedClipId, bool includeSyncTarget = false) const;

    Document& document_;
    EditLog& edit_log_;
    TimelineViewport& viewport_;
    Ulid selected_clip_id_;
    std::vector<Ulid> selected_clip_ids_;
    std::optional<TimelineGapSelection> selected_gap_;
    std::optional<RationalTime> requested_playhead_;
    std::optional<TimelineTrimPreview> preview_;
    std::optional<TimelineMovePreview> move_preview_;
    std::optional<TimelineHit> drag_hit_;
    std::optional<RationalTime> drag_start_time_;
    int32_t drag_rate_ = 1;
    double drag_x_ = 0.0;
    bool snapping_enabled_ = true;
    bool linked_selection_enabled_ = true;
    std::optional<RationalTime> snap_guide_time_;
};

std::vector<TimelineClipRect> VisibleTimelineClips(
    const Document& document, const TimelineViewport& viewport, double width,
    const std::vector<Ulid>& selectedClipIds,
    const std::optional<TimelineTrimPreview>& preview,
    const std::optional<TimelineMovePreview>& movePreview = std::nullopt);
