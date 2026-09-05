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
    double header_width = 96.0;

    double TimeToX(RationalTime time) const;
    RationalTime XToTime(double x, int32_t rate) const;

    void ScrollByPixels(double delta_x, int32_t rate);
    void ZoomAroundX(double x, double factor, int32_t rate);
    void FitDuration(RationalTime duration, double width);
    std::vector<double> TickXs(double width) const;
};

// UI-2026-08 -- a practical lower bound must still allow long-form projects
// to fit. Four pixels per second capped an ordinary editor pane at roughly
// three minutes, making the zoom bar and FitDuration misleadingly stop early.
// Numerical safety floor only: at this scale an ordinary editor pane spans
// decades, so navigation has no user-visible zoom-out limit.
constexpr double kTimelineMinPixelsPerSecond = 0.000001;
constexpr double kTimelineMaxPixelsPerSecond = 4000.0;

// UI-2026-08 -- Keep device-specific NSEvent details out of the viewport.
// AppKit only reports whether deltas are precise and which modifiers are
// active; this portable policy decides whether the same gesture pans or
// zooms, so mouse and trackpad regressions are covered by unit tests.
enum class TimelineScrollAction { Pan, Zoom };

struct TimelineScrollIntent {
    TimelineScrollAction action = TimelineScrollAction::Pan;
    double delta = 0.0;
};

TimelineScrollIntent ResolveTimelineScrollIntent(double deltaX, double deltaY,
                                                 bool preciseDeltas,
                                                 bool shiftPressed);

enum class TimelineZoomBarPart { None, LeftHandle, Body, RightHandle };

struct TimelineZoomBarGeometry {
    double rail_x = 0.0;
    double rail_width = 0.0;
    double thumb_x = 0.0;
    double thumb_width = 0.0;
    double handle_width = 4.0;

    TimelineZoomBarPart HitTest(double x) const;
};

TimelineZoomBarGeometry CalculateTimelineZoomBarGeometry(
    const TimelineViewport& viewport, RationalTime duration, double width);

struct TimelineZoomBarDrag {
    TimelineZoomBarPart part = TimelineZoomBarPart::None;
    double pointer_x = 0.0;
    TimelineViewport initial_viewport;
    TimelineZoomBarGeometry initial_geometry;
};

void UpdateTimelineZoomBarDrag(TimelineViewport& viewport,
                               const TimelineZoomBarDrag& drag, double pointerX,
                               RationalTime duration, double width,
                               int32_t rate);

constexpr double kTimelineToolbarHeight = 26.0;
constexpr double kTimelineScaleHeight = 28.0;
constexpr double kTimelineRenderBandHeight = 5.0;
// Compatibility name used by the interaction engine for "everything above
// the first track". It now includes the integrated toolbar and render band;
// the actual ruler remains kTimelineScaleHeight.
constexpr double kTimelineRulerHeight =
    kTimelineToolbarHeight + kTimelineScaleHeight + kTimelineRenderBandHeight;
constexpr double kTimelineZoomBarHeight = 14.0;
constexpr double kTimelineEdgeHitWidth = 6.0;
constexpr double kTimelineSnapDistance = 8.0;

enum class PlayheadResolution { Frame, Sample };

RationalTime QuantizePlayheadPosition(RationalTime position,
                                      PlayheadResolution resolution,
                                      MediaRate frameRate,
                                      int32_t sampleRate = 48000);

enum class TimelineHitEdge { None, Head, Tail };
enum class TimelineTrimMode { Normal, Ripple, Roll, Slip };

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

// Exact edit points shared by keyboard navigation and temporary playhead
// snapping. Results are sorted and deduplicated across every track.
std::vector<RationalTime> TimelineEditPoints(const Document& document);
std::optional<RationalTime> AdjacentTimelineEdit(const Document& document,
                                                 const RationalTime& position,
                                                 bool forward);
std::optional<RationalTime> SnapTimelineTimeToEdit(
    const Document& document, const TimelineViewport& viewport,
    const RationalTime& candidate);

// Builds the canonical blade operation. With linked selection enabled, every
// member of the same A/V group containing the cut is split atomically.
Operation TimelineCutOperationForClip(const Document& document,
                                      const DocumentClip& clip,
                                      const RationalTime& position,
                                      bool linkedSelection);

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

    // Empty unless *exactly one* clip is selected. Callers that must act on
    // an unambiguous single target rely on that.
    const Ulid& SelectedClipId() const;
    // The clip the selection is anchored on -- the one actually clicked --
    // even when selecting it pulled in others. Non-empty whenever anything
    // is selected. Surfaces that describe or edit one clip at a time (the
    // Inspector) want this, not SelectedClipId(): linking a video clip to
    // its audio is the norm for imported footage, so requiring a selection
    // of exactly one would leave them permanently blank.
    const Ulid& PrimarySelectedClipId() const;
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
    void SetPlayheadSnapTarget(std::optional<RationalTime> target);
    const std::optional<RationalTime>& SnapGuideTime() const;
    void SetLinkedSelectionEnabled(bool enabled);
    void SetTrimMode(TimelineTrimMode mode);
    TimelineTrimMode TrimMode() const;
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
    Ulid primary_clip_id_;
    std::vector<Ulid> selected_clip_ids_;
    std::optional<TimelineGapSelection> selected_gap_;
    std::optional<RationalTime> requested_playhead_;
    std::optional<TimelineTrimPreview> preview_;
    std::optional<TimelineMovePreview> move_preview_;
    std::optional<Operation> trim_operation_;
    std::optional<TimelineHit> drag_hit_;
    std::optional<RationalTime> drag_start_time_;
    int32_t drag_rate_ = 1;
    double drag_x_ = 0.0;
    bool snapping_enabled_ = true;
    std::optional<RationalTime> playhead_snap_target_;
    bool linked_selection_enabled_ = true;
    TimelineTrimMode trim_mode_ = TimelineTrimMode::Normal;
    std::optional<RationalTime> snap_guide_time_;
};

std::vector<TimelineClipRect> VisibleTimelineClips(
    const Document& document, const TimelineViewport& viewport, double width,
    const std::vector<Ulid>& selectedClipIds,
    const std::optional<TimelineTrimPreview>& preview,
    const std::optional<TimelineMovePreview>& movePreview = std::nullopt);

// Builds one exact, undoable edit over every unlocked track. Clear preserves
// the range as a gap; ripple closes it. Clips crossing a boundary are split
// while retaining exact source time.
std::optional<DeleteGapOperation> TimelineRangeDeleteOperation(
    const Document& document, const RationalTime& rangeIn,
    const RationalTime& rangeOut, bool ripple);

// Targeted tracks are edited directly. During ripple edits, other unlocked
// tracks participate only when their sync lock is enabled. An empty target
// list intentionally means that no track is edited.
std::optional<DeleteGapOperation> TimelineRangeDeleteOperation(
    const Document& document, const RationalTime& rangeIn,
    const RationalTime& rangeOut, bool ripple,
    const std::vector<Ulid>& targetedTrackIds);

// Builds the exact range edit used by UI ripple-delete for a clip selection.
// Linked selections may span tracks; downstream sync-locked tracks follow the
// same range through TimelineRangeDeleteOperation.
std::optional<DeleteGapOperation> BuildTimelineRippleDeleteSelection(
    const Document& document, const std::vector<Ulid>& clipIds);

// Builds one atomic source-to-record edit. Insert opens time on the target and
// every unlocked sync-locked follower; overwrite replaces only the target
// interval. The returned exact-track operation is deterministic and undoable.
std::optional<DeleteGapOperation> TimelineSourceEditOperation(
    const Document& document, const Ulid& sourceId,
    const RationalTime& sourceIn, const RationalTime& sourceOut,
    const RationalTime& timelineIn, const Ulid& targetTrackId, bool insert,
    const std::vector<Ulid>& audioTargetTrackIds = {});
