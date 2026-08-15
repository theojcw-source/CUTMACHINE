#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

extern "C" {
#include <libavutil/frame.h>
}

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "AudioPlayback.h"
#include "Cli.h"
#include "ColorEffects.h"
#include "DecodeWorker.h"
#include "Document.h"
#include "EditLog.h"
#include "Export.h"
#include "FrameCache.h"
#include "Ingest.h"
#include "McpProjectBackend.h"
#include "McpServer.h"
#include "MediaTaskManager.h"
#include "PanelLayout.h"
#include "PerformanceMetrics.h"
#include "Project.h"
#include "ProjectRecovery.h"
#include "ProjectStorage.h"
#include "Proxy.h"
#include "Relink.h"
#include "Renderer.h"
#include "Thumbnail.h"
#include "Timecode.h"
#include "Timeline.h"
#include "TimelineView.h"
#include "TransportBar.h"
#include "UiTheme.h"
#include "Waveform.h"

// F2.1 -- ROADMAP.md design system: reusable AppKit chrome and the fixed
// right-hand panel host that F2.2 (Inspector) and F2.4 (Chat) will each
// install their content into (see PanelHostView.h). Objective-C++
// declarations, so #import rather than #include, matching AppKit/Foundation
// above.
#import "InspectorView.h"
#import "PanelHostView.h"
#import "UiComponents.h"
#import "UiThemeAppKit.h"
// F2.5 -- ROADMAP.md playback transport: installs into PanelSlot::Transport
// the same way the F2.1 imports above install into the other docks.
#import "TransportView.h"

namespace {

constexpr size_t kGlobalCacheBudget = 2000000000ULL;  // 2.0 GB, all sources.
constexpr double kAddTrackRowHeight = 22.0;
NSPasteboardType const kCutmachineMediaPasteboardType =
    @"com.cutmachine.library-media";
NSPasteboardType const kCutmachineBinPasteboardType = @"com.cutmachine.bin";

enum class TimelineTool { Select, Hand, Zoom, Cut, Slip };
enum class HistoryDomain { Timeline, Project };

NSString* ToolName(TimelineTool tool) {
    switch (tool) {
        case TimelineTool::Select:
            return @"Sélection (V)";
        case TimelineTool::Hand:
            return @"Main (H)";
        case TimelineTool::Zoom:
            return @"Zoom (Z)";
        case TimelineTool::Cut:
            return @"Lame (C/B)";
        case TimelineTool::Slip:
            return @"Slip (Y)";
    }
}

constexpr NSEventModifierFlags kShortcutModifierMask =
    NSEventModifierFlagCommand | NSEventModifierFlagOption |
    NSEventModifierFlagControl | NSEventModifierFlagShift;

bool ParseShortcutSpec(NSString* spec, NSString** key,
                       NSEventModifierFlags* modifiers, NSString** error) {
    NSString* trimmed = [spec
        stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
    if (trimmed.length == 0) {
        *key = @"";
        *modifiers = 0;
        return true;
    }
    NSEventModifierFlags flags = 0;
    NSString* parsedKey = nil;
    for (NSString* raw in [trimmed componentsSeparatedByString:@"+"]) {
        NSString* token =
            [[raw stringByTrimmingCharactersInSet:NSCharacterSet
                                                      .whitespaceCharacterSet]
                lowercaseString];
        if ([token isEqualToString:@"cmd"] ||
            [token isEqualToString:@"command"] || [token isEqualToString:@"⌘"])
            flags |= NSEventModifierFlagCommand;
        else if ([token isEqualToString:@"alt"] ||
                 [token isEqualToString:@"option"] ||
                 [token isEqualToString:@"⌥"])
            flags |= NSEventModifierFlagOption;
        else if ([token isEqualToString:@"ctrl"] ||
                 [token isEqualToString:@"control"] ||
                 [token isEqualToString:@"⌃"])
            flags |= NSEventModifierFlagControl;
        else if ([token isEqualToString:@"shift"] ||
                 [token isEqualToString:@"⇧"])
            flags |= NSEventModifierFlagShift;
        else if (!parsedKey)
            parsedKey = token;
        else {
            if (error)
                *error = @"Une combinaison ne peut contenir qu’une touche.";
            return false;
        }
    }
    if (!parsedKey) {
        if (error) *error = @"Ajoutez une touche après les modificateurs.";
        return false;
    }
    NSDictionary<NSString*, NSString*>* aliases = @{
        @"espace" : @"space",
        @"suppr" : @"delete",
        @"retour" : @"delete",
        @"virgule" : @",",
        @"comma" : @",",
        @"point" : @"."
    };
    parsedKey = aliases[parsedKey] ?: parsedKey;
    if (parsedKey.length != 1 &&
        ![@[ @"space", @"delete" ] containsObject:parsedKey]) {
        if (error)
            *error = @"Utilisez une lettre, un chiffre, Space ou Delete.";
        return false;
    }
    *key = parsedKey;
    *modifiers = flags;
    return true;
}

NSString* ShortcutSignature(NSString* spec, NSString** error) {
    NSString* key = nil;
    NSEventModifierFlags modifiers = 0;
    if (!ParseShortcutSpec(spec, &key, &modifiers, error)) return nil;
    if (key.length == 0) return @"";
    return [NSString
        stringWithFormat:@"%llu:%@", static_cast<unsigned long long>(modifiers),
                         key];
}

NSString* MenuKeyEquivalentForShortcut(NSString* key) {
    if ([key isEqualToString:@"space"]) return @" ";
    if ([key isEqualToString:@"delete"]) return @"\b";
    return key ?: @"";
}

struct ResolvedSlot {
    bool active = false;
    Ulid sourceId;
    int64_t frame = -1;
    float opacity = 1.0f;
    // The DocumentClip currently occupying this slot. Used only to look up
    // that clip's color.* effects stack for F1.3 grading; see
    // presentNearestFrameAtDeadline.
    Ulid clipId;
};

struct RenderedSlot {
    bool active = false;
    Ulid sourceId;
    int64_t frame = -1;
    float opacity = 1.0f;
    Ulid clipId;

    bool operator==(const RenderedSlot& other) const {
        return active == other.active && sourceId == other.sourceId &&
               frame == other.frame && opacity == other.opacity &&
               clipId == other.clipId;
    }
};

struct ProbedImportItem {
    std::filesystem::path absolute_path;
    std::optional<LibraryMedia> media;
    std::string error;
};

struct ProbedImportBatch {
    std::filesystem::path document_path;
    std::string target_bin;
    std::vector<ProbedImportItem> items;
};

struct PendingProxy {
    Ulid media_id;
    std::filesystem::path absolute_path;
    std::string stored_path;
};

struct PendingWaveform {
    Ulid media_id;
    std::filesystem::path absolute_path;
};

struct PendingThumbnail {
    Ulid media_id;
    std::filesystem::path absolute_path;
};

struct RelinkProbeResult {
    LibraryMedia media;
    std::string error;
};

struct PendingRelink {
    Ulid media_id;
    std::filesystem::path absolute_path;
    std::string stored_path;
    std::shared_ptr<RelinkProbeResult> result;
};

struct BatchRelinkResult {
    std::vector<RelinkReplacement> replacements;
    size_t unmatched = 0;
    size_t ambiguous = 0;
    size_t incompatible = 0;
    std::string last_error;
};

struct PendingBatchRelink {
    std::shared_ptr<BatchRelinkResult> result;
};

bool DecodeWorkerMatchesSource(const DecodeWorker& worker,
                               const DocumentSource& source) {
    if (static_cast<int64_t>(worker.FrameRateNumerator()) * source.rate.den !=
        static_cast<int64_t>(source.rate.num) * worker.FrameRateDenominator())
        return false;
    const int64_t declaredFrames =
        source.duration.to_frames(source.rate.num, source.rate.den);
    return declaredFrames <= worker.FrameCount();
}

struct TimelineClipboardClip {
    DocumentClip clip;
    Ulid track_id;
    std::string track_kind;
    size_t kind_ordinal = 0;
};

std::vector<const DocumentTrack*> TracksOfKind(const Document& document,
                                               const std::string& kind) {
    std::vector<const DocumentTrack*> tracks;
    for (const DocumentTrack& track : document.sequence.tracks)
        if (track.kind == kind) tracks.push_back(&track);
    std::stable_sort(tracks.begin(), tracks.end(),
                     [](const DocumentTrack* left, const DocumentTrack* right) {
                         return left->index < right->index;
                     });
    return tracks;
}

std::vector<TimelineClipboardClip> CopyTimelineClips(
    const Document& document, const std::vector<Ulid>& clipIds) {
    std::vector<TimelineClipboardClip> result;
    for (const Ulid& id : clipIds) {
        if (std::any_of(result.begin(), result.end(),
                        [&](const auto& item) { return item.clip.id == id; }))
            continue;
        const DocumentClip* clip = document.FindClip(id);
        const DocumentTrack* track = document.FindTrackForClip(id);
        if (!clip || !track) continue;
        const auto kindTracks = TracksOfKind(document, track->kind);
        const auto found =
            std::find(kindTracks.begin(), kindTracks.end(), track);
        result.push_back({*clip, track->id, track->kind,
                          found == kindTracks.end()
                              ? 0
                              : static_cast<size_t>(
                                    std::distance(kindTracks.begin(), found))});
    }
    std::stable_sort(result.begin(), result.end(),
                     [](const TimelineClipboardClip& left,
                        const TimelineClipboardClip& right) {
                         if (left.clip.timeline_in != right.clip.timeline_in)
                             return left.clip.timeline_in <
                                    right.clip.timeline_in;
                         if (left.track_kind != right.track_kind)
                             return left.track_kind < right.track_kind;
                         return left.kind_ordinal < right.kind_ordinal;
                     });
    return result;
}

std::optional<PasteClipsOperation> PasteTimelineClipboardAt(
    const Document& document,
    const std::vector<TimelineClipboardClip>& clipboard, RationalTime anchor) {
    if (clipboard.empty()) return std::nullopt;
    RationalTime first = clipboard.front().clip.timeline_in;
    for (const TimelineClipboardClip& item : clipboard)
        if (item.clip.timeline_in < first) first = item.clip.timeline_in;
    PasteClipsOperation operation;
    for (const TimelineClipboardClip& item : clipboard) {
        const DocumentTrack* target = document.FindTrack(item.track_id);
        if (!target || target->kind != item.track_kind) {
            const auto tracks = TracksOfKind(document, item.track_kind);
            if (item.kind_ordinal >= tracks.size()) return std::nullopt;
            target = tracks[item.kind_ordinal];
        }
        const DocumentClip& clip = item.clip;
        operation.clips.push_back({clip.id,
                                   target->id,
                                   clip.source_id,
                                   clip.source_in,
                                   clip.duration,
                                   anchor.add(clip.timeline_in.sub(first)),
                                   {},
                                   clip.link_group_id,
                                   {},
                                   clip.sync_anchor_clip_id,
                                   {},
                                   clip.sync_reference_delta});
    }
    return operation;
}

std::optional<PasteClipsOperation> PasteTimelineClipboardAtMoves(
    const std::vector<TimelineClipboardClip>& clipboard,
    const TimelineMovePreview& preview) {
    if (clipboard.empty() || !preview.valid) return std::nullopt;
    std::map<Ulid, std::pair<Ulid, RationalTime>> destinations;
    for (const LinkedClipMove& move : preview.linked_moves)
        destinations[move.clip_id] = {move.track_id, move.timeline_in};
    if (destinations.empty())
        destinations[preview.clip_id] = {preview.target_track_id,
                                         preview.timeline_in};
    PasteClipsOperation operation;
    for (const TimelineClipboardClip& item : clipboard) {
        const auto destination = destinations.find(item.clip.id);
        if (destination == destinations.end()) return std::nullopt;
        const DocumentClip& clip = item.clip;
        operation.clips.push_back({clip.id,
                                   destination->second.first,
                                   clip.source_id,
                                   clip.source_in,
                                   clip.duration,
                                   destination->second.second,
                                   {},
                                   clip.link_group_id,
                                   {},
                                   clip.sync_anchor_clip_id,
                                   {},
                                   clip.sync_reference_delta});
    }
    return operation;
}

struct AppState {
    std::unique_ptr<ProjectSessionLock> projectLock;
    Project project;
    ProjectEditLog projectEditLog;
    Ulid activeTimelineId;
    HistoryDomain lastHistoryDomain = HistoryDomain::Timeline;
    Document document;
    EditLog editLog;
    std::map<Ulid, EditLog> timelineEditLogs;
    std::map<Ulid, RationalTime> timelinePositions;
    std::map<Ulid, std::set<Ulid>> timelineTargetTracks;
    std::set<Ulid> targetedTrackIds;
    TimelineViewport viewport;
    std::unique_ptr<TimelineInteraction> interaction;
    std::unique_ptr<Timeline> timeline;
    std::unique_ptr<Renderer> renderer;
    std::unique_ptr<Renderer> sourceRenderer;
    std::unique_ptr<Renderer> programRenderer;
    std::unique_ptr<FrameCache> frameCache;
    std::unique_ptr<PerformanceMetrics> performanceMetrics;
    std::unique_ptr<AudioPlayback> audioPlayback;
    std::unique_ptr<MediaTaskManager> mediaTasks =
        std::make_unique<MediaTaskManager>(2);
    std::map<Ulid, std::shared_ptr<ProbedImportBatch>> pendingImports;
    std::map<Ulid, PendingProxy> pendingProxies;
    std::map<Ulid, PendingWaveform> pendingWaveforms;
    std::map<Ulid, PendingThumbnail> pendingThumbnails;
    std::map<Ulid, PendingRelink> pendingRelinks;
    std::map<Ulid, PendingBatchRelink> pendingBatchRelinks;
    std::map<Ulid, AudioWaveform> waveforms;
    bool proxiesEnabled = true;
    std::map<Ulid, std::unique_ptr<DecodeWorker>> workers;
    // Sources remain part of the edit even when their files are unavailable.
    // Keeping this separate from the document makes reconnecting media a
    // runtime concern rather than a destructive edit.
    std::set<Ulid> offlineSourceIds;
    // Runtime probe cache. UI metadata never mutates the edit document.
    std::map<Ulid, LibraryMedia> mediaMetadata;
    std::vector<Ulid> videoTrackIds;
    std::vector<ResolvedSlot> requested;
    std::vector<RenderedSlot> rendered;
    RenderedSlot sourceRendered;
    RationalTime duration{0, 1};
    RationalTime requestedPosition{0, 1};
    std::optional<RationalTime> timelineIn;
    std::optional<RationalTime> timelineOut;
    PlayheadResolution playheadResolution = PlayheadResolution::Frame;
    bool overlayDirty = true;
    TimelineTool tool = TimelineTool::Select;
    bool spaceHand = false;
    bool navigationDragging = false;
    bool scrubDragging = false;
    bool editDragging = false;
    bool duplicateDragging = false;
    bool lassoCandidate = false;
    bool lassoDragging = false;
    bool linkedSelection = true;
    bool linkedSelectionGesture = true;
    std::vector<TimelineClipboardClip> timelineClipboard;
    std::vector<TimelineClipboardClip> duplicateDragClipboard;
    double lassoStartX = 0.0;
    double lassoStartY = 0.0;
    double lassoCurrentX = 0.0;
    double lassoCurrentY = 0.0;
    bool spaceUsedForPan = false;
    double navigationLastX = 0.0;
    int playbackDirection = 0;
    RationalTime playbackAnchor{0, 1};
    std::chrono::steady_clock::time_point playbackStarted;
    std::optional<double> cutPreviewX;
    std::optional<double> cutPreviewY;
    bool sourceMonitor = false;
    Ulid sourceMonitorId;
    RationalTime sourceMonitorPosition{0, 1};
    std::optional<RationalTime> sourceIn;
    std::optional<RationalTime> sourceOut;
    bool sourceMonitorActive = false;
    double sourceMonitorZoom = 0.0;
    double programMonitorZoom = 0.0;
    bool sourceMonitorVisible = true;
    double sourceMonitorPanelWidth = 0.0;
    Ulid contextClipId;
    Ulid contextTrackId;
    RationalTime contextTime{0, 1};
    std::optional<TimelineGapSelection> contextGap;
    std::atomic_bool exportCancel{false};
    bool exportRunning = false;
};

std::array<float, 3> ClipColor(const Ulid& sourceId, bool audio) {
    uint64_t hash = UINT64_C(1469598103934665603);
    for (const unsigned char byte : sourceId) {
        hash ^= byte;
        hash *= UINT64_C(1099511628211);
    }
    const float variation = static_cast<float>(hash & 0xff) / 2550.0f;
    if (audio)
        return {0.08f + variation * 0.18f, 0.30f + variation * 0.72f,
                0.20f + variation * 0.32f};
    return {0.10f + variation * 0.24f, 0.27f + variation * 0.48f,
            0.42f + variation * 0.58f};
}

NSImage* SystemSymbol(NSString* name, NSString* description,
                      CGFloat pointSize = 13.0) {
    NSImage* image = [NSImage imageWithSystemSymbolName:name
                               accessibilityDescription:description];
    if (!image) return nil;
    NSImageSymbolConfiguration* configuration = [NSImageSymbolConfiguration
        configurationWithPointSize:pointSize
                            weight:NSFontWeightMedium];
    return [image imageWithSymbolConfiguration:configuration];
}

NSString* TimeString(const RationalTime& time) {
    return [NSString stringWithFormat:@"%lld/%d",
                                      static_cast<long long>(time.value),
                                      time.rate];
}

// Delegates to Timecode.h's plain-C++ FormatTimecode so this label and the
// new Transport panel (TransportView.mm) format HH:MM:SS:FF identically
// instead of maintaining two copies of the same rule.
NSString* TimelineTimecode(const RationalTime& time, MediaRate rate) {
    return [NSString stringWithUTF8String:FormatTimecode(time, rate).c_str()];
}

std::string SyncDriftLabel(const RationalTime& drift, MediaRate frameRate) {
    const int64_t frames = drift.to_frames(frameRate.num, frameRate.den);
    const RationalTime frameQuantized{
        frames * static_cast<int64_t>(frameRate.den), frameRate.num};
    int64_t value = frames;
    const char* unit = "f";
    if (frameQuantized != drift) {
        value = drift.to_frames(48000);
        unit = "smp";
    }
    return std::string(value > 0 ? "+" : "") + std::to_string(value) + unit;
}

DeleteGapOperation GapDeleteOperationForSelection(
    const Document& document, const TimelineGapSelection& gap,
    bool linkedSelection) {
    DeleteGapOperation operation{gap.track_id, gap.start, gap.duration, {}};
    if (!linkedSelection) return operation;
    const DocumentTrack* track = document.FindTrack(gap.track_id);
    if (!track) return operation;
    const RationalTime gapEnd = gap.start.add(gap.duration);
    const DocumentClip* following = nullptr;
    for (const DocumentClip& clip : track->clips) {
        if (clip.timeline_in < gapEnd) continue;
        if (!following || clip.timeline_in < following->timeline_in)
            following = &clip;
    }
    if (!following || following->link_group_id.empty()) return operation;
    for (const Ulid& clipId :
         ExpandLinkedClipSelection(document, {following->id})) {
        const DocumentTrack* linkedTrack = document.FindTrackForClip(clipId);
        if (!linkedTrack || linkedTrack->id == gap.track_id ||
            std::find(operation.linked_track_ids.begin(),
                      operation.linked_track_ids.end(),
                      linkedTrack->id) != operation.linked_track_ids.end())
            continue;
        operation.linked_track_ids.push_back(linkedTrack->id);
    }
    return operation;
}

}  // namespace

@protocol TimelineEventTarget <NSObject>
- (void)timelineMouseDown:(NSEvent*)event;
- (void)timelineMouseDragged:(NSEvent*)event;
- (void)timelineMouseUp:(NSEvent*)event;
- (void)timelineMouseMoved:(NSEvent*)event;
- (void)timelineScroll:(NSEvent*)event;
- (BOOL)timelineKeyDown:(NSEvent*)event;
- (void)timelineKeyUp:(NSEvent*)event;
- (BOOL)timelineDropMedia:(NSString*)mediaId atViewPoint:(NSPoint)point;
- (NSMenu*)timelineMenuForEvent:(NSEvent*)event;
@end

@class TimelineMetalView;
@protocol TimelineMetalViewResizeTarget <NSObject>
- (void)timelineMetalViewDidResize:(TimelineMetalView*)view;
@end

@interface CutmachineSplitView : NSSplitView
@end

@implementation CutmachineSplitView
- (CGFloat)dividerThickness {
    return 7.0;
}
- (void)drawDividerInRect:(NSRect)rect {
    [[NSColor colorWithWhite:0.055 alpha:1.0] setFill];
    NSRectFill(rect);
    [[NSColor colorWithWhite:0.24 alpha:1.0] setFill];
    if (self.isVertical) {
        const CGFloat x = NSMidX(rect) - 0.5;
        NSRectFill(NSMakeRect(x, NSMidY(rect) - 18.0, 1.0, 36.0));
    } else {
        const CGFloat y = NSMidY(rect) - 0.5;
        NSRectFill(NSMakeRect(NSMidX(rect) - 24.0, y, 48.0, 1.0));
    }
}
@end

@interface TimelineMetalView : NSView
@property(nonatomic, weak) id<TimelineEventTarget> eventTarget;
@property(nonatomic, weak) id<TimelineMetalViewResizeTarget> resizeTarget;
@end

@implementation TimelineMetalView
- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame]))
        [self registerForDraggedTypes:@[ kCutmachineMediaPasteboardType ]];
    return self;
}
- (BOOL)acceptsFirstResponder {
    return YES;
}
- (void)setFrameSize:(NSSize)newSize {
    const BOOL changed = !NSEqualSizes(self.frame.size, newSize);
    [super setFrameSize:newSize];
    if (changed) [self.resizeTarget timelineMetalViewDidResize:self];
}
- (NSView*)hitTest:(NSPoint)point {
    // Monitor controls remain interactive while passive labels stay visual
    // overlays and editing gestures remain owned by the Metal surface.
    NSView* child = [super hitTest:point];
    if ([child isKindOfClass:NSPopUpButton.class] ||
        [child isKindOfClass:NSButton.class])
        return child;
    return NSPointInRect(point, self.bounds) ? self : nil;
}
- (void)mouseDown:(NSEvent*)event {
    [self.window makeFirstResponder:self];
    [self.eventTarget timelineMouseDown:event];
}
- (void)mouseDragged:(NSEvent*)event {
    [self.eventTarget timelineMouseDragged:event];
}
- (void)mouseUp:(NSEvent*)event {
    [self.eventTarget timelineMouseUp:event];
}
- (void)mouseMoved:(NSEvent*)event {
    [self.eventTarget timelineMouseMoved:event];
}
- (void)scrollWheel:(NSEvent*)event {
    [self.eventTarget timelineScroll:event];
}
- (void)keyDown:(NSEvent*)event {
    if (![self.eventTarget timelineKeyDown:event]) [super keyDown:event];
}
- (void)keyUp:(NSEvent*)event {
    [self.eventTarget timelineKeyUp:event];
}
- (NSMenu*)menuForEvent:(NSEvent*)event {
    return [self.eventTarget timelineMenuForEvent:event];
}
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender {
    return [[sender draggingPasteboard]
               availableTypeFromArray:@[ kCutmachineMediaPasteboardType ]]
               ? NSDragOperationCopy
               : NSDragOperationNone;
}
- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
    NSString* mediaId = [[sender draggingPasteboard]
        stringForType:kCutmachineMediaPasteboardType];
    if (!mediaId) return NO;
    const NSPoint point = [self convertPoint:sender.draggingLocation
                                    fromView:nil];
    return [self.eventTarget timelineDropMedia:mediaId atViewPoint:point];
}
@end

@interface ContextOutlineView : NSOutlineView
@end
@implementation ContextOutlineView
- (NSMenu*)menuForEvent:(NSEvent*)event {
    const NSInteger row = [self
        rowAtPoint:[self convertPoint:event.locationInWindow fromView:nil]];
    if (row >= 0)
        [self selectRowIndexes:[NSIndexSet indexSetWithIndex:row]
            byExtendingSelection:NO];
    else {
        for (NSInteger candidate = 0; candidate < self.numberOfRows;
             ++candidate) {
            if ([[self itemAtRow:candidate] isEqual:@"__root__"]) {
                [self selectRowIndexes:[NSIndexSet indexSetWithIndex:candidate]
                    byExtendingSelection:NO];
                break;
            }
        }
    }
    return [super menuForEvent:event];
}
@end

@interface ContextTableView : NSTableView
@end
@implementation ContextTableView
- (NSMenu*)menuForEvent:(NSEvent*)event {
    const NSInteger row = [self
        rowAtPoint:[self convertPoint:event.locationInWindow fromView:nil]];
    if (row >= 0 && ![self.selectedRowIndexes containsIndex:row])
        [self selectRowIndexes:[NSIndexSet indexSetWithIndex:row]
            byExtendingSelection:NO];
    return [super menuForEvent:event];
}
@end

@interface ContextCollectionView : NSCollectionView
@end
@implementation ContextCollectionView
- (NSMenu*)menuForEvent:(NSEvent*)event {
    NSIndexPath* indexPath =
        [self indexPathForItemAtPoint:[self convertPoint:event.locationInWindow
                                                fromView:nil]];
    if (indexPath && ![self.selectionIndexPaths containsObject:indexPath])
        self.selectionIndexPaths = [NSSet setWithObject:indexPath];
    return [super menuForEvent:event];
}
@end

@interface MediaIconItem : NSCollectionViewItem
@end

@implementation MediaIconItem
- (void)loadView {
    self.view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 132, 112)];
    NSImageView* image =
        [[NSImageView alloc] initWithFrame:NSMakeRect(14, 27, 104, 76)];
    image.imageScaling = NSImageScaleProportionallyUpOrDown;
    image.wantsLayer = YES;
    image.layer.backgroundColor =
        [NSColor colorWithWhite:0.13 alpha:1.0].CGColor;
    image.layer.cornerRadius = 5.0;
    NSTextField* label =
        [[NSTextField alloc] initWithFrame:NSMakeRect(4, 4, 124, 19)];
    label.editable = NO;
    label.selectable = NO;
    label.bezeled = NO;
    label.drawsBackground = NO;
    label.alignment = NSTextAlignmentCenter;
    label.font = [NSFont systemFontOfSize:11.0];
    label.lineBreakMode = NSLineBreakByTruncatingMiddle;
    self.imageView = image;
    self.textField = label;
    [self.view addSubview:image];
    [self.view addSubview:label];
}
- (void)setSelected:(BOOL)selected {
    [super setSelected:selected];
    self.view.layer.backgroundColor =
        selected ? [NSColor selectedContentBackgroundColor].CGColor
                 : NSColor.clearColor.CGColor;
}
@end

@interface AppDelegate : NSObject <NSApplicationDelegate,
                                   NSWindowDelegate,
                                   NSSplitViewDelegate,
                                   TimelineEventTarget,
                                   TimelineMetalViewResizeTarget,
                                   NSOutlineViewDataSource,
                                   NSOutlineViewDelegate,
                                   NSTableViewDataSource,
                                   NSTableViewDelegate,
                                   NSCollectionViewDataSource,
                                   NSCollectionViewDelegate,
                                   NSTextFieldDelegate,
                                   CMInspectorViewDelegate>
@property(nonatomic, strong) NSWindow* window;
@property(nonatomic, strong) NSWindow* startupWindow;
@property(nonatomic, strong) NSPopUpButton* recentProjectsPopup;
@property(nonatomic, assign) NSInteger startupChoice;
@property(nonatomic, copy) NSString* startupSelectedPath;
@property(nonatomic, strong) TimelineMetalView* metalView;
@property(nonatomic, strong) TimelineMetalView* sourceMonitorView;
@property(nonatomic, strong) TimelineMetalView* programMonitorView;
@property(nonatomic, strong) NSView* sourceMonitorPanel;
@property(nonatomic, strong) NSView* programMonitorPanel;
@property(nonatomic, strong) NSView* mediaPanel;
@property(nonatomic, strong) NSSplitView* workspaceSplitView;
@property(nonatomic, strong) NSSplitView* editorSplitView;
@property(nonatomic, strong) NSSplitView* monitorSplitView;
// F2.1 design system: fixed right-hand dock hosting the Inspector (F2.2) and
// Chat (F2.4) tabs. See PanelHostView.h -- the set of tabs and their order
// are fixed at construction time, never user-rearrangeable.
@property(nonatomic, strong) CMPanelHostView* rightDockPanel;
// F2.5 design system: fixed bottom dock hosting the Transport panel (play/
// pause, scrub bar, timecode, frame step). One slot, same fixed-tab host as
// rightDockPanel -- see PanelHostView.h.
@property(nonatomic, strong) CMPanelHostView* bottomDockPanel;
@property(nonatomic, strong) CMTransportView* transportView;
// F2.2 -- Inspector content installed into rightDockPanel's Inspector slot
// (see -applicationDidFinishLaunching below).
@property(nonatomic, strong) CMInspectorView* inspectorView;
@property(nonatomic, strong) NSPopUpButton* binPopup;
@property(nonatomic, strong) NSPopUpButton* mediaPopup;
@property(nonatomic, strong) NSTextField* binSummaryLabel;
@property(nonatomic, strong) NSTextField* mediaTaskLabel;
@property(nonatomic, strong) NSProgressIndicator* mediaTaskProgress;
@property(nonatomic, strong) NSButton* mediaTaskCancelButton;
@property(nonatomic, copy) NSString* displayedMediaTaskId;
@property(nonatomic, strong) NSButton* assignMediaButton;
@property(nonatomic, strong) NSOutlineView* binOutline;
@property(nonatomic, strong) NSTableView* mediaTable;
@property(nonatomic, strong) NSSearchField* mediaSearchField;
@property(nonatomic, strong) NSMutableArray<NSString*>* visibleMediaIds;
@property(nonatomic, copy) NSString* selectedBinId;
@property(nonatomic, strong) NSCollectionView* mediaCollection;
@property(nonatomic, strong)
    NSMutableDictionary<NSString*, NSImage*>* mediaThumbnails;
@property(nonatomic, strong) NSScrollView* mediaListScroll;
@property(nonatomic, strong) NSScrollView* mediaIconScroll;
@property(nonatomic, strong) NSSegmentedControl* mediaViewToggle;
@property(nonatomic, strong) NSButton* sourceMonitorButton;
@property(nonatomic, strong) NSTextField* infoLabel;
@property(nonatomic, strong) NSTextField* offlineMediaLabel;
@property(nonatomic, strong) NSTextField* sourceOfflineMediaLabel;
@property(nonatomic, strong) NSTextField* sourceMonitorTitleLabel;
@property(nonatomic, strong) NSTextField* sourceMonitorZoneLabel;
@property(nonatomic, strong) NSTextField* programMonitorTitleLabel;
@property(nonatomic, strong) NSPopUpButton* sourceMonitorZoomPopup;
@property(nonatomic, strong) NSPopUpButton* programMonitorZoomPopup;
@property(nonatomic, strong) NSButton* sourceMonitorToggleButton;
@property(nonatomic, strong) NSTextField* timelineTimecodeLabel;
@property(nonatomic, strong) NSMutableArray<NSTextField*>* trackHeaderLabels;
@property(nonatomic, strong) NSMutableArray<NSImageView*>* timelineToolIcons;
@property(nonatomic, strong) NSButton* linkedSelectionButton;
@property(nonatomic, strong)
    NSMutableDictionary<NSString*, NSString*>* shortcutBindings;
@property(nonatomic, strong)
    NSMutableDictionary<NSString*, NSMenuItem*>* shortcutMenuItems;
@property(nonatomic, strong)
    NSMutableDictionary<NSString*, NSTextField*>* keyboardEditingFields;
@property(nonatomic, strong) NSPopUpButton* keyboardCommandPopup;
@property(nonatomic, strong) NSButton* keyboardCommandModifier;
@property(nonatomic, strong) NSButton* keyboardOptionModifier;
@property(nonatomic, strong) NSButton* keyboardControlModifier;
@property(nonatomic, strong) NSButton* keyboardShiftModifier;
@property(nonatomic, strong) NSTimer* displayTimer;
@property(nonatomic, copy) NSString* documentPath;
@property(nonatomic, copy) NSString* lastProjectLoadError;
@property(nonatomic, assign) AppState* state;
- (BOOL)importMediaURLs:(NSArray<NSURL*>*)urls intoBin:(NSString*)binId;
- (BOOL)chooseStartupProject;
- (BOOL)createStartupProject;
- (BOOL)openStartupProject;
- (NSString*)resolvedProjectPath:(NSString*)selection;
- (void)collectPortableProject:(id)sender;
- (NSArray<NSString*>*)selectedMediaIds;
- (void)beginEditingBin:(NSString*)binId;
- (void)refreshTimelineChrome;
- (BOOL)hasValidTimelineRange;
- (BOOL)deleteTimelineRangeRipple:(BOOL)ripple;
- (void)menuCopyTimelineClips:(id)sender;
- (void)menuPasteTimelineClips:(id)sender;
- (void)menuAddCrossDissolve:(id)sender;
- (void)menuRemoveCrossDissolve:(id)sender;
- (BOOL)applyTimelinePaste:(PasteClipsOperation)operation
                     label:(NSString*)label;
- (BOOL)persistStagedDocument:(const Document&)document
                      editLog:(const EditLog&)editLog
                      message:(std::string&)message;
- (BOOL)commitProjectCandidate:(const Project&)project
                       editLog:(const EditLog&)editLog
                       message:(std::string&)message;
- (BOOL)commitProjectCandidate:(const Project&)project
                       editLog:(const EditLog&)editLog
                    projectLog:(const ProjectEditLog&)projectLog
                       message:(std::string&)message;
- (BOOL)activateTimelineIdentifier:(NSString*)identifier;
- (void)createTimelinePressed:(id)sender;
- (void)requestSourcePosition:(RationalTime)position;
- (BOOL)performSourceEditInsert:(BOOL)insert;
- (void)updateSourceZoneLabel;
- (void)refreshMediaTaskStatus;
- (void)processCompletedMediaImports;
- (void)commitMediaImportBatch:(ProbedImportBatch*)batch;
- (void)processCompletedMediaProxies;
- (void)processCompletedMediaWaveforms;
- (void)processCompletedMediaThumbnails;
- (void)processCompletedMediaRelinks;
- (void)processCompletedBatchRelinks;
- (void)reloadDecodeWorkers;
- (void)refreshAfterProjectMutation;
- (void)enqueueProxyForMediaIdentifier:(NSString*)identifier;
- (void)loadOrEnqueueWaveformForMediaIdentifier:(NSString*)identifier;
- (void)enqueueWaveformForMediaIdentifier:(NSString*)identifier;
- (void)loadOrEnqueueThumbnailForMediaIdentifier:(NSString*)identifier;
- (void)enqueueThumbnailForMediaIdentifier:(NSString*)identifier;
@end

@implementation AppDelegate

- (NSArray<NSDictionary<NSString*, NSString*>*>*)shortcutDefinitions {
    return @[
        @{
            @"id" : @"tool.select",
            @"title" : @"Outil · Sélection",
            @"default" : @"V"
        },
        @{@"id" : @"tool.hand", @"title" : @"Outil · Main", @"default" : @"H"},
        @{@"id" : @"tool.zoom", @"title" : @"Outil · Zoom", @"default" : @"Z"},
        @{@"id" : @"tool.cut", @"title" : @"Outil · Lame", @"default" : @"C"},
        @{
            @"id" : @"tool.cut.alternate",
            @"title" : @"Outil · Lame (secondaire)",
            @"default" : @"B"
        },
        @{@"id" : @"tool.slip", @"title" : @"Outil · Slip", @"default" : @"Y"},
        @{
            @"id" : @"play.toggle",
            @"title" : @"Lecture · Lecture/Pause",
            @"default" : @"Space"
        },
        @{
            @"id" : @"play.reverse",
            @"title" : @"Lecture · Arrière",
            @"default" : @"J"
        },
        @{
            @"id" : @"play.stop",
            @"title" : @"Lecture · Arrêt",
            @"default" : @"K"
        },
        @{
            @"id" : @"play.forward",
            @"title" : @"Lecture · Avant",
            @"default" : @"L"
        },
        @{
            @"id" : @"mark.in",
            @"title" : @"Clip · Point d’entrée",
            @"default" : @"I"
        },
        @{
            @"id" : @"mark.out",
            @"title" : @"Clip · Point de sortie",
            @"default" : @"O"
        },
        @{
            @"id" : @"mark.clear",
            @"title" : @"Clip · Effacer In/Out",
            @"default" : @"Alt+X"
        },
        @{
            @"id" : @"edit.delete",
            @"title" : @"Édition · Supprimer",
            @"default" : @"Delete"
        },
        @{
            @"id" : @"edit.ripple",
            @"title" : @"Édition · Suppression ripple",
            @"default" : @"Shift+Delete"
        },
        @{
            @"id" : @"edit.copy",
            @"title" : @"Édition · Copier les clips",
            @"default" : @"Cmd+C"
        },
        @{
            @"id" : @"edit.paste",
            @"title" : @"Édition · Coller les clips",
            @"default" : @"Cmd+V"
        },
        @{
            @"id" : @"timeline.fit",
            @"title" : @"Timeline · Tout cadrer",
            @"default" : @"F"
        },
        @{
            @"id" : @"view.fullscreen",
            @"title" : @"Affichage · Plein écran",
            @"default" : @"P"
        },
        @{
            @"id" : @"timeline.snapping",
            @"title" : @"Timeline · Magnétisme",
            @"default" : @"N"
        },
        @{
            @"id" : @"timeline.linked",
            @"title" : @"Timeline · Sélection liée",
            @"default" : @"Cmd+Shift+L"
        },
        @{
            @"id" : @"transition.add",
            @"title" : @"Timeline · Ajouter un fondu enchaîné",
            @"default" : @"Cmd+T"
        },
        @{
            @"id" : @"source.insert",
            @"title" : @"Source → Record · Insérer",
            @"default" : @","
        },
        @{
            @"id" : @"source.overwrite",
            @"title" : @"Source → Record · Écraser",
            @"default" : @"."
        },
    ];
}

- (void)loadShortcutBindings {
    self.shortcutBindings = [NSMutableDictionary dictionary];
    NSDictionary* stored = [NSUserDefaults.standardUserDefaults
        dictionaryForKey:@"KeyboardShortcuts.v1"];
    for (NSDictionary* definition in [self shortcutDefinitions]) {
        NSString* identifier = definition[@"id"];
        NSString* value = [stored[identifier] isKindOfClass:NSString.class]
                              ? stored[identifier]
                              : definition[@"default"];
        self.shortcutBindings[identifier] = value;
    }
    self.shortcutMenuItems = [NSMutableDictionary dictionary];
}

- (BOOL)event:(NSEvent*)event matchesShortcut:(NSString*)identifier {
    NSString* expectedKey = nil;
    NSEventModifierFlags expectedModifiers = 0;
    if (!ParseShortcutSpec(self.shortcutBindings[identifier], &expectedKey,
                           &expectedModifiers, nullptr) ||
        expectedKey.length == 0)
        return NO;
    NSString* eventKey = event.charactersIgnoringModifiers.lowercaseString;
    if (event.keyCode == 51 || event.keyCode == 117) eventKey = @"delete";
    if ([eventKey isEqualToString:@" "]) eventKey = @"space";
    const NSEventModifierFlags actual =
        event.modifierFlags & kShortcutModifierMask;
    return actual == expectedModifiers &&
           [eventKey isEqualToString:expectedKey];
}

- (void)applyShortcutBindingsToMenus {
    [self.shortcutMenuItems
        enumerateKeysAndObjectsUsingBlock:^(NSString* identifier,
                                            NSMenuItem* item, BOOL* stop) {
          (void)stop;
          NSString* key = nil;
          NSEventModifierFlags modifiers = 0;
          ParseShortcutSpec(self.shortcutBindings[identifier], &key, &modifiers,
                            nullptr);
          item.keyEquivalent = MenuKeyEquivalentForShortcut(key);
          item.keyEquivalentModifierMask = modifiers;
        }];
}

- (NSMenuItem*)shortcutMenuItem:(NSString*)title
                         action:(SEL)action
                        command:(NSString*)identifier {
    NSMenuItem* item = [self menuItem:title action:action key:@""];
    self.shortcutMenuItems[identifier] = item;
    return item;
}

- (void)keyboardKeyPressed:(NSButton*)sender {
    NSString* identifier =
        self.keyboardCommandPopup.selectedItem.representedObject;
    NSTextField* field = self.keyboardEditingFields[identifier];
    if (!field || sender.identifier.length == 0) return;
    NSMutableArray<NSString*>* parts = [NSMutableArray array];
    if (self.keyboardCommandModifier.state == NSControlStateValueOn)
        [parts addObject:@"Cmd"];
    if (self.keyboardOptionModifier.state == NSControlStateValueOn)
        [parts addObject:@"Alt"];
    if (self.keyboardControlModifier.state == NSControlStateValueOn)
        [parts addObject:@"Ctrl"];
    if (self.keyboardShiftModifier.state == NSControlStateValueOn)
        [parts addObject:@"Shift"];
    [parts addObject:sender.identifier];
    field.stringValue = [parts componentsJoinedByString:@"+"];
    self.infoLabel.stringValue = [NSString
        stringWithFormat:@"%@ affecté à %@", field.stringValue,
                         self.keyboardCommandPopup.selectedItem.title];
}

- (NSButton*)keyboardModifierButton:(NSString*)title frame:(NSRect)frame {
    NSButton* button = [NSButton checkboxWithTitle:title target:nil action:nil];
    button.frame = frame;
    button.font = [NSFont systemFontOfSize:11.0 weight:NSFontWeightMedium];
    return button;
}

- (NSView*)keyboardViewForDefinitions:(NSArray*)definitions
                               fields:(NSMutableDictionary*)fields {
    NSView* keyboard =
        [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 650, 255)];
    NSTextField* instruction = [NSTextField
        labelWithString:@"Choisissez une commande, activez les modificateurs "
                        @"puis cliquez sur une touche."];
    instruction.frame = NSMakeRect(0, 229, 650, 20);
    instruction.textColor = NSColor.secondaryLabelColor;
    [keyboard addSubview:instruction];

    self.keyboardEditingFields = fields;
    self.keyboardCommandPopup =
        [[NSPopUpButton alloc] initWithFrame:NSMakeRect(0, 197, 315, 27)];
    for (NSDictionary* definition in definitions) {
        [self.keyboardCommandPopup addItemWithTitle:definition[@"title"]];
        self.keyboardCommandPopup.lastItem.representedObject =
            definition[@"id"];
    }
    [keyboard addSubview:self.keyboardCommandPopup];
    self.keyboardCommandModifier =
        [self keyboardModifierButton:@"⌘ Cmd"
                               frame:NSMakeRect(330, 199, 72, 24)];
    self.keyboardOptionModifier =
        [self keyboardModifierButton:@"⌥ Alt"
                               frame:NSMakeRect(402, 199, 68, 24)];
    self.keyboardControlModifier =
        [self keyboardModifierButton:@"⌃ Ctrl"
                               frame:NSMakeRect(470, 199, 72, 24)];
    self.keyboardShiftModifier =
        [self keyboardModifierButton:@"⇧ Shift"
                               frame:NSMakeRect(542, 199, 85, 24)];
    for (NSButton* modifier in @[
             self.keyboardCommandModifier, self.keyboardOptionModifier,
             self.keyboardControlModifier, self.keyboardShiftModifier
         ])
        [keyboard addSubview:modifier];

    NSArray<NSArray<NSString*>*>* rows = @[
        @[
            @"1", @"2", @"3", @"4", @"5", @"6", @"7", @"8", @"9", @"0",
            @"Delete"
        ],
        @[ @"A", @"Z", @"E", @"R", @"T", @"Y", @"U", @"I", @"O", @"P" ],
        @[ @"Q", @"S", @"D", @"F", @"G", @"H", @"J", @"K", @"L", @"M" ],
        @[ @"W", @"X", @"C", @"V", @"B", @"N", @",", @".", @"Space" ]
    ];
    const CGFloat keyHeight = 35.0;
    const CGFloat gap = 5.0;
    for (NSUInteger row = 0; row < rows.count; ++row) {
        CGFloat x = row == 0 ? 0.0 : row * 12.0;
        const CGFloat y = 151.0 - row * (keyHeight + gap);
        for (NSString* key in rows[row]) {
            CGFloat width = [key isEqualToString:@"Space"]    ? 170.0
                            : [key isEqualToString:@"Delete"] ? 70.0
                                                              : 48.0;
            NSString* title = [key isEqualToString:@"Space"]    ? @"Espace"
                              : [key isEqualToString:@"Delete"] ? @"⌫"
                                                                : key;
            NSButton* button =
                [NSButton buttonWithTitle:title
                                   target:self
                                   action:@selector(keyboardKeyPressed:)];
            button.frame = NSMakeRect(x, y, width, keyHeight);
            button.identifier = key;
            button.bezelStyle = NSBezelStyleTexturedRounded;
            button.font = [NSFont systemFontOfSize:12.0
                                            weight:NSFontWeightMedium];
            [keyboard addSubview:button];
            x += width + gap;
        }
    }
    return keyboard;
}

- (void)editKeyboardShortcuts:(id)sender {
    (void)sender;
    NSArray* definitions = [self shortcutDefinitions];
    NSMutableDictionary<NSString*, NSTextField*>* fields =
        [NSMutableDictionary dictionary];
    const CGFloat rowHeight = 32.0;
    const CGFloat documentHeight = rowHeight * definitions.count;
    NSView* document =
        [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 650, documentHeight)];
    for (NSUInteger index = 0; index < definitions.count; ++index) {
        NSDictionary* definition = definitions[index];
        const CGFloat y = documentHeight - (index + 1) * rowHeight + 4.0;
        NSTextField* label = [NSTextField labelWithString:definition[@"title"]];
        label.frame = NSMakeRect(8, y + 3, 370, 22);
        NSTextField* field =
            [[NSTextField alloc] initWithFrame:NSMakeRect(390, y, 245, 25)];
        field.stringValue = self.shortcutBindings[definition[@"id"]] ?: @"";
        field.placeholderString = @"Non assigné";
        field.toolTip = @"Exemples : V, Space, Alt+X, Cmd+Shift+L";
        fields[definition[@"id"]] = field;
        [document addSubview:label];
        [document addSubview:field];
    }
    NSScrollView* scroll =
        [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 650, 300)];
    scroll.hasVerticalScroller = YES;
    scroll.borderType = NSBezelBorder;
    scroll.documentView = document;

    while (true) {
        NSAlert* alert = [[NSAlert alloc] init];
        alert.messageText = @"Raccourcis clavier";
        alert.informativeText =
            @"Cliquez sur le clavier ou saisissez directement une combinaison. "
             "Une valeur vide désactive la commande.";
        NSView* accessory =
            [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 650, 565)];
        NSView* keyboard = [self keyboardViewForDefinitions:definitions
                                                     fields:fields];
        keyboard.frame = NSMakeRect(0, 310, 650, 255);
        scroll.frame = NSMakeRect(0, 0, 650, 300);
        [accessory addSubview:keyboard];
        [accessory addSubview:scroll];
        alert.accessoryView = accessory;
        [alert addButtonWithTitle:@"Enregistrer"];
        [alert addButtonWithTitle:@"Annuler"];
        [alert addButtonWithTitle:@"Valeurs par défaut"];
        const NSModalResponse response = [alert runModal];
        if (response == NSAlertSecondButtonReturn) return;
        if (response == NSAlertThirdButtonReturn) {
            for (NSDictionary* definition in definitions)
                fields[definition[@"id"]].stringValue = definition[@"default"];
            continue;
        }
        NSMutableDictionary<NSString*, NSString*>* candidate =
            [NSMutableDictionary dictionary];
        NSMutableDictionary<NSString*, NSString*>* owners =
            [NSMutableDictionary dictionary];
        owners[ShortcutSignature(@"Cmd+Q", nullptr)] = @"Quitter";
        owners[ShortcutSignature(@"Cmd+Comma", nullptr)] =
            @"Raccourcis clavier";
        owners[ShortcutSignature(@"Cmd+Z", nullptr)] = @"Annuler";
        owners[ShortcutSignature(@"Cmd+Shift+Z", nullptr)] = @"Rétablir";
        owners[ShortcutSignature(@"Cmd+Shift+T", nullptr)] =
            @"Ajouter une piste vidéo";
        owners[ShortcutSignature(@"Cmd+Alt+Shift+T", nullptr)] =
            @"Ajouter une piste audio";
        NSString* validationError = nil;
        for (NSDictionary* definition in definitions) {
            NSString* identifier = definition[@"id"];
            NSString* spec = [fields[identifier].stringValue
                stringByTrimmingCharactersInSet:NSCharacterSet
                                                    .whitespaceCharacterSet];
            NSString* parseError = nil;
            NSString* signature = ShortcutSignature(spec, &parseError);
            if (!signature) {
                validationError =
                    [NSString stringWithFormat:@"%@ : %@", definition[@"title"],
                                               parseError];
                break;
            }
            if (signature.length > 0 && owners[signature]) {
                validationError = [NSString
                    stringWithFormat:@"Conflit entre « %@ » et « %@ ».",
                                     owners[signature], definition[@"title"]];
                break;
            }
            if (signature.length > 0) owners[signature] = definition[@"title"];
            candidate[identifier] = spec;
        }
        if (validationError) {
            NSAlert* errorAlert = [[NSAlert alloc] init];
            errorAlert.alertStyle = NSAlertStyleWarning;
            errorAlert.messageText = @"Raccourci invalide";
            errorAlert.informativeText = validationError;
            [errorAlert runModal];
            continue;
        }
        self.shortcutBindings = candidate;
        [NSUserDefaults.standardUserDefaults setObject:candidate
                                                forKey:@"KeyboardShortcuts.v1"];
        [self applyShortcutBindingsToMenus];
        self.infoLabel.stringValue = @"Raccourcis clavier mis à jour";
        return;
    }
}

- (NSMenuItem*)menuItem:(NSString*)title action:(SEL)action key:(NSString*)key {
    NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:title
                                                  action:action
                                           keyEquivalent:key ?: @""];
    item.target = self;
    return item;
}

- (void)installApplicationMenus {
    [self loadShortcutBindings];
    NSMenu* bar = [[NSMenu alloc] initWithTitle:@"Main"];
    NSMenuItem* appRoot = [[NSMenuItem alloc] initWithTitle:@"CUTMACHINE"
                                                     action:nil
                                              keyEquivalent:@""];
    NSMenu* app = [[NSMenu alloc] initWithTitle:@"CUTMACHINE"];
    [app addItemWithTitle:@"À propos de CUTMACHINE"
                   action:@selector(orderFrontStandardAboutPanel:)
            keyEquivalent:@""];
    [app addItem:NSMenuItem.separatorItem];
    [app addItem:[self menuItem:@"Raccourcis clavier…"
                         action:@selector(editKeyboardShortcuts:)
                            key:@","]];
    [app addItem:NSMenuItem.separatorItem];
    [app addItemWithTitle:@"Quitter CUTMACHINE"
                   action:@selector(terminate:)
            keyEquivalent:@"q"];
    appRoot.submenu = app;
    [bar addItem:appRoot];

    NSMenuItem* fileRoot = [[NSMenuItem alloc] initWithTitle:@"Fichier"
                                                      action:nil
                                               keyEquivalent:@""];
    NSMenu* file = [[NSMenu alloc] initWithTitle:@"Fichier"];
    [file addItem:[self menuItem:@"Importer des rushes…"
                          action:@selector(importMediaPressed:)
                             key:@"i"]];
    [file addItem:[self menuItem:@"Reconnecter les médias offline…"
                          action:@selector(batchRelinkOfflineMedia:)
                             key:@""]];
    [file addItem:[self menuItem:@"Collecter le projet…"
                          action:@selector(collectPortableProject:)
                             key:@""]];
    [file addItem:NSMenuItem.separatorItem];
    [file addItem:[self menuItem:@"Exporter la vidéo finale…"
                          action:@selector(exportFinalVideo:)
                             key:@"e"]];
    fileRoot.submenu = file;
    [bar addItem:fileRoot];

    NSMenuItem* editRoot = [[NSMenuItem alloc] initWithTitle:@"Édition"
                                                      action:nil
                                               keyEquivalent:@""];
    NSMenu* edit = [[NSMenu alloc] initWithTitle:@"Édition"];
    [edit addItem:[self menuItem:@"Annuler"
                          action:@selector(menuUndo:)
                             key:@"z"]];
    NSMenuItem* redo = [self menuItem:@"Rétablir"
                               action:@selector(menuRedo:)
                                  key:@"z"];
    redo.keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagShift;
    [edit addItem:redo];
    [edit addItem:NSMenuItem.separatorItem];
    [edit addItem:[self shortcutMenuItem:@"Copier les clips"
                                  action:@selector(menuCopyTimelineClips:)
                                 command:@"edit.copy"]];
    [edit addItem:[self shortcutMenuItem:@"Coller les clips"
                                  action:@selector(menuPasteTimelineClips:)
                                 command:@"edit.paste"]];
    [edit addItem:NSMenuItem.separatorItem];
    [edit addItem:[self shortcutMenuItem:@"Supprimer la sélection"
                                  action:@selector(menuDeleteSelection:)
                                 command:@"edit.delete"]];
    [edit addItem:[self shortcutMenuItem:@"Suppression ripple"
                                  action:@selector(menuRippleDeleteSelection:)
                                 command:@"edit.ripple"]];
    editRoot.submenu = edit;
    [bar addItem:editRoot];

    NSMenuItem* clipRoot = [[NSMenuItem alloc] initWithTitle:@"Clip"
                                                      action:nil
                                               keyEquivalent:@""];
    NSMenu* clip = [[NSMenu alloc] initWithTitle:@"Clip"];
    [clip addItem:[self menuItem:@"Ouvrir dans le moniteur source"
                          action:@selector(openSelectedMediaInSourceMonitor:)
                             key:@""]];
    [clip addItem:[self menuItem:@"Couper au playhead"
                          action:@selector(menuCutSelectedAtPlayhead:)
                             key:@""]];
    [clip addItem:NSMenuItem.separatorItem];
    [clip addItem:[self shortcutMenuItem:@"Définir le point d’entrée"
                                  action:@selector(menuMarkTimelineIn:)
                                 command:@"mark.in"]];
    [clip addItem:[self shortcutMenuItem:@"Définir le point de sortie"
                                  action:@selector(menuMarkTimelineOut:)
                                 command:@"mark.out"]];
    [clip addItem:[self shortcutMenuItem:@"Effacer les points In/Out"
                                  action:@selector(menuClearTimelineRange:)
                                 command:@"mark.clear"]];
    [clip addItem:NSMenuItem.separatorItem];
    [clip addItem:[self shortcutMenuItem:@"Insérer depuis le moniteur source"
                                  action:@selector(menuInsertSource:)
                                 command:@"source.insert"]];
    [clip addItem:[self shortcutMenuItem:@"Écraser depuis le moniteur source"
                                  action:@selector(menuOverwriteSource:)
                                 command:@"source.overwrite"]];
    clipRoot.submenu = clip;
    [bar addItem:clipRoot];

    NSMenuItem* colorRoot = [[NSMenuItem alloc] initWithTitle:@"Couleur"
                                                       action:nil
                                                keyEquivalent:@""];
    NSMenu* color = [[NSMenu alloc] initWithTitle:@"Couleur"];
    [color addItem:[self menuItem:@"Preset Sony S-Log3 → Rec.2020 HLG"
                           action:@selector(applySonyColorPreset:)
                              key:@""]];
    [color addItem:[self menuItem:@"Configurer la gestion colorimétrique…"
                           action:@selector(configureColorManagement:)
                              key:@""]];
    [color addItem:NSMenuItem.separatorItem];
    [color addItem:[self menuItem:@"Désactiver la gestion colorimétrique"
                           action:@selector(disableColorManagement:)
                              key:@""]];
    colorRoot.submenu = color;
    [bar addItem:colorRoot];

    NSMenuItem* timelineRoot = [[NSMenuItem alloc] initWithTitle:@"Timeline"
                                                          action:nil
                                                   keyEquivalent:@""];
    NSMenu* timeline = [[NSMenu alloc] initWithTitle:@"Timeline"];
    NSMenuItem* newTimeline = [self menuItem:@"Nouvelle timeline…"
                                      action:@selector(createTimelinePressed:)
                                         key:@"n"];
    newTimeline.keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagOption;
    [timeline addItem:newTimeline];
    [timeline addItem:NSMenuItem.separatorItem];
    [timeline addItem:[self shortcutMenuItem:@"Outil Sélection"
                                      action:@selector(menuSelectTool:)
                                     command:@"tool.select"]];
    [timeline addItem:[self shortcutMenuItem:@"Outil Main"
                                      action:@selector(menuHandTool:)
                                     command:@"tool.hand"]];
    [timeline addItem:[self shortcutMenuItem:@"Outil Zoom"
                                      action:@selector(menuZoomTool:)
                                     command:@"tool.zoom"]];
    [timeline addItem:[self shortcutMenuItem:@"Outil Lame"
                                      action:@selector(menuCutTool:)
                                     command:@"tool.cut"]];
    [timeline addItem:[self shortcutMenuItem:@"Outil Slip"
                                      action:@selector(menuSlipTool:)
                                     command:@"tool.slip"]];
    [timeline addItem:NSMenuItem.separatorItem];
    [timeline addItem:[self shortcutMenuItem:@"Magnétisme"
                                      action:@selector(menuToggleSnapping:)
                                     command:@"timeline.snapping"]];
    [timeline
        addItem:[self shortcutMenuItem:@"Sélection liée"
                                action:@selector(menuToggleLinkedSelection:)
                               command:@"timeline.linked"]];
    [timeline addItem:[self shortcutMenuItem:@"Ajouter un fondu enchaîné"
                                      action:@selector(menuAddCrossDissolve:)
                                     command:@"transition.add"]];
    [timeline addItem:[self menuItem:@"Supprimer le fondu au raccord"
                              action:@selector(menuRemoveCrossDissolve:)
                                 key:@""]];
    [timeline addItem:[self menuItem:@"Utiliser les proxies"
                              action:@selector(menuToggleProxies:)
                                 key:@""]];
    [timeline addItem:[self shortcutMenuItem:@"Cadrer toute la timeline"
                                      action:@selector(menuFitTimeline:)
                                     command:@"timeline.fit"]];
    [timeline addItem:[self menuItem:@"Réglages de séquence…"
                              action:@selector(configureSequence:)
                                 key:@""]];
    [timeline addItem:NSMenuItem.separatorItem];
    [timeline addItem:[self menuItem:@"Ajouter une piste vidéo"
                              action:@selector(menuAddVideoTrack:)
                                 key:@""]];
    [timeline addItem:[self menuItem:@"Ajouter une piste audio"
                              action:@selector(menuAddAudioTrack:)
                                 key:@""]];
    [timeline addItem:[self menuItem:@"Supprimer les pistes vides"
                              action:@selector(removeEmptyTracksPressed:)
                                 key:@""]];
    timelineRoot.submenu = timeline;
    [bar addItem:timelineRoot];

    NSMenuItem* playbackRoot = [[NSMenuItem alloc] initWithTitle:@"Lecture"
                                                          action:nil
                                                   keyEquivalent:@""];
    NSMenu* playback = [[NSMenu alloc] initWithTitle:@"Lecture"];
    [playback addItem:[self shortcutMenuItem:@"Lecture / Pause"
                                      action:@selector(menuPlayPause:)
                                     command:@"play.toggle"]];
    [playback addItem:[self shortcutMenuItem:@"Lecture arrière"
                                      action:@selector(menuPlayReverse:)
                                     command:@"play.reverse"]];
    [playback addItem:[self shortcutMenuItem:@"Arrêt"
                                      action:@selector(menuStop:)
                                     command:@"play.stop"]];
    [playback addItem:[self shortcutMenuItem:@"Lecture avant"
                                      action:@selector(menuPlayForward:)
                                     command:@"play.forward"]];
    playbackRoot.submenu = playback;
    [bar addItem:playbackRoot];

    NSMenuItem* viewRoot = [[NSMenuItem alloc] initWithTitle:@"Affichage"
                                                      action:nil
                                               keyEquivalent:@""];
    NSMenu* view = [[NSMenu alloc] initWithTitle:@"Affichage"];
    [view addItem:[self shortcutMenuItem:@"Activer/désactiver le plein écran"
                                  action:@selector(toggleFullscreen:)
                                 command:@"view.fullscreen"]];
    viewRoot.submenu = view;
    [bar addItem:viewRoot];
    NSApp.mainMenu = bar;
    [self applyShortcutBindingsToMenus];
}

- (void)toggleFullscreen:(id)sender {
    (void)sender;
    [self.window toggleFullScreen:nil];
}

- (BOOL)commitColorSettings:(const ColorManagementSettings&)settings {
    const ColorManagementSettings previous =
        self.state->document.color_management;
    self.state->document.color_management = settings;
    std::string message;
    if (![self persistEdits:message]) {
        self.state->document.color_management = previous;
        std::fprintf(stderr, "Unable to persist color settings: %s\n",
                     message.c_str());
        NSBeep();
        return NO;
    }
    self.state->overlayDirty = true;
    return YES;
}

- (void)configureSequence:(id)sender {
    (void)sender;
    if (!self.state) return;
    const DocumentSequence current = self.state->document.sequence;
    NSAlert* alert = [NSAlert new];
    alert.messageText = @"Réglages de séquence";
    alert.informativeText =
        @"Le format de séquence pilote le moniteur et les presets d’export.";
    [alert addButtonWithTitle:@"Appliquer"];
    [alert addButtonWithTitle:@"Annuler"];
    NSView* accessory =
        [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 460, 108)];

    NSTextField* nameLabel = [NSTextField labelWithString:@"Nom :"];
    nameLabel.frame = NSMakeRect(0, 79, 120, 22);
    nameLabel.alignment = NSTextAlignmentRight;
    [accessory addSubview:nameLabel];
    NSTextField* name =
        [[NSTextField alloc] initWithFrame:NSMakeRect(132, 76, 316, 24)];
    name.stringValue = [NSString stringWithUTF8String:current.name.c_str()];
    [accessory addSubview:name];

    auto addPopup = [&](NSString* labelText, NSArray<NSString*>* choices,
                        CGFloat y) {
        NSTextField* label = [NSTextField labelWithString:labelText];
        label.frame = NSMakeRect(0, y + 3, 120, 22);
        label.alignment = NSTextAlignmentRight;
        [accessory addSubview:label];
        NSPopUpButton* menu =
            [[NSPopUpButton alloc] initWithFrame:NSMakeRect(132, y, 316, 26)
                                       pullsDown:NO];
        [menu addItemsWithTitles:choices];
        [accessory addSubview:menu];
        return menu;
    };
    NSString* currentFormat = [NSString
        stringWithFormat:@"Actuel — %d × %d", current.width, current.height];
    NSPopUpButton* format =
        addPopup(@"Dimensions :",
                 @[
                     currentFormat, @"HD — 1920 × 1080", @"UHD — 3840 × 2160",
                     @"Vertical HD — 1080 × 1920", @"Carré — 1080 × 1080"
                 ],
                 44);
    NSString* currentRate = [NSString stringWithFormat:@"Actuelle — %d/%d i/s",
                                                       current.frame_rate.num,
                                                       current.frame_rate.den];
    NSPopUpButton* cadence =
        addPopup(@"Cadence :",
                 @[
                     currentRate, @"23,976 i/s", @"24 i/s", @"25 i/s",
                     @"29,97 i/s", @"50 i/s", @"59,94 i/s"
                 ],
                 12);
    alert.accessoryView = accessory;
    if ([alert runModal] != NSAlertFirstButtonReturn) return;

    UpdateSequenceOperation updated;
    updated.sequence_id = current.id;
    updated.name = name.stringValue.UTF8String ?: "Sequence 1";
    updated.width = current.width;
    updated.height = current.height;
    updated.frame_rate = current.frame_rate;
    switch (format.indexOfSelectedItem) {
        case 1:
            updated.width = 1920;
            updated.height = 1080;
            break;
        case 2:
            updated.width = 3840;
            updated.height = 2160;
            break;
        case 3:
            updated.width = 1080;
            updated.height = 1920;
            break;
        case 4:
            updated.width = 1080;
            updated.height = 1080;
            break;
        default:
            break;
    }
    switch (cadence.indexOfSelectedItem) {
        case 1:
            updated.frame_rate = {24000, 1001};
            break;
        case 2:
            updated.frame_rate = {24, 1};
            break;
        case 3:
            updated.frame_rate = {25, 1};
            break;
        case 4:
            updated.frame_rate = {30000, 1001};
            break;
        case 5:
            updated.frame_rate = {50, 1};
            break;
        case 6:
            updated.frame_rate = {60000, 1001};
            break;
        default:
            break;
    }
    EditError editError = EditError::None;
    std::string message;
    if (!self.state->editLog.Apply(self.state->document,
                                   Operation{std::move(updated)}, editError,
                                   message)) {
        [self showExportError:message];
        return;
    }
    [self refreshTimelineAfterEditFromPosition:self.state->requestedPosition];
    [self rebuildMediaList];
    if (![self persistEdits:message]) {
        [self showExportError:message];
        return;
    }
    self.state->overlayDirty = true;
}

- (void)showExportError:(const std::string&)message {
    NSAlert* alert = [NSAlert new];
    alert.alertStyle = NSAlertStyleCritical;
    alert.messageText = @"Export impossible";
    alert.informativeText =
        [NSString stringWithUTF8String:message.c_str()] ?: @"Erreur inconnue";
    [alert beginSheetModalForWindow:self.window completionHandler:nil];
}

- (void)exportFinalVideo:(id)sender {
    (void)sender;
    if (!self.state || self.state->exportRunning) return;

    const int32_t sequenceWidth = self.state->document.sequence.width;
    const int32_t sequenceHeight = self.state->document.sequence.height;
    const MediaRate sequenceRate = self.state->document.sequence.frame_rate;

    NSAlert* settingsAlert = [NSAlert new];
    settingsAlert.messageText = @"Exporter la vidéo finale";
    settingsAlert.informativeText =
        @"Choisissez un preset. Les autres menus servent uniquement "
         "d’override ponctuel.";
    [settingsAlert addButtonWithTitle:@"Choisir la destination…"];
    [settingsAlert addButtonWithTitle:@"Annuler"];
    NSView* accessory =
        [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 520, 172)];
    auto popup = [&](NSString* labelText, NSArray<NSString*>* choices,
                     CGFloat y) {
        NSTextField* label = [NSTextField labelWithString:labelText];
        label.frame = NSMakeRect(0, y + 3, 132, 22);
        label.alignment = NSTextAlignmentRight;
        [accessory addSubview:label];
        NSPopUpButton* menu =
            [[NSPopUpButton alloc] initWithFrame:NSMakeRect(144, y, 364, 26)
                                       pullsDown:NO];
        [menu addItemsWithTitles:choices];
        [accessory addSubview:menu];
        return menu;
    };

    NSMutableArray<NSString*>* presetNames = [NSMutableArray array];
    for (const ExportPresetDescriptor& preset : Exporter::Presets())
        [presetNames addObject:[NSString stringWithUTF8String:preset.name]];
    NSPopUpButton* preset = popup(@"Preset :", presetNames, 140);
    NSPopUpButton* container =
        popup(@"Conteneur :", @[ @"MP4", @"QuickTime MOV" ], 108);
    NSPopUpButton* resolution =
        popup(@"Résolution :",
              @[
                  @"Selon le preset", @"Format de la séquence", @"1920 × 1080",
                  @"3840 × 2160"
              ],
              76);
    NSPopUpButton* cadence =
        popup(@"Cadence :",
              @[
                  @"Selon le preset", @"Cadence de la séquence", @"23,976 i/s",
                  @"24 i/s", @"25 i/s", @"29,97 i/s", @"50 i/s", @"59,94 i/s"
              ],
              44);
    NSPopUpButton* encoder =
        popup(@"Encodage :",
              @[
                  @"Selon le preset", @"VideoToolbox — rapide",
                  @"x265 — qualité constante"
              ],
              12);
    settingsAlert.accessoryView = accessory;
    if ([settingsAlert runModal] != NSAlertFirstButtonReturn) return;

    const auto& presets = Exporter::Presets();
    const size_t presetIndex = std::min<size_t>(
        static_cast<size_t>(preset.indexOfSelectedItem), presets.size() - 1);
    ExportSettings selectedSettings =
        Exporter::SettingsForPreset(presets[presetIndex].id, "", sequenceWidth,
                                    sequenceHeight, sequenceRate);
    switch (resolution.indexOfSelectedItem) {
        case 1:
            selectedSettings.width = sequenceWidth + sequenceWidth % 2;
            selectedSettings.height = sequenceHeight + sequenceHeight % 2;
            break;
        case 2:
            selectedSettings.width = 1920;
            selectedSettings.height = 1080;
            break;
        case 3:
            selectedSettings.width = 3840;
            selectedSettings.height = 2160;
            break;
        default:
            break;
    }
    switch (cadence.indexOfSelectedItem) {
        case 1:
            selectedSettings.frame_rate = sequenceRate;
            break;
        case 2:
            selectedSettings.frame_rate = {24000, 1001};
            break;
        case 3:
            selectedSettings.frame_rate = {24, 1};
            break;
        case 4:
            selectedSettings.frame_rate = {25, 1};
            break;
        case 5:
            selectedSettings.frame_rate = {30000, 1001};
            break;
        case 6:
            selectedSettings.frame_rate = {50, 1};
            break;
        case 7:
            selectedSettings.frame_rate = {60000, 1001};
            break;
        default:
            break;
    }
    if (encoder.indexOfSelectedItem == 1)
        selectedSettings.encoder = ExportEncoder::HevcVideoToolbox;
    else if (encoder.indexOfSelectedItem == 2)
        selectedSettings.encoder = ExportEncoder::HevcSoftware;

    const BOOL mov = container.indexOfSelectedItem == 1;
    NSSavePanel* save = [NSSavePanel savePanel];
    save.title = @"Destination de l’export";
    save.nameFieldStringValue = mov ? @"export.mov" : @"export.mp4";
    save.allowedContentTypes =
        @[ [UTType typeWithFilenameExtension:(mov ? @"mov" : @"mp4")] ];
    [save
        beginSheetModalForWindow:self.window
               completionHandler:^(NSModalResponse response) {
                 if (response != NSModalResponseOK || !save.URL) return;

                 ExportSettings settings = selectedSettings;
                 settings.output_path = save.URL.fileSystemRepresentation ?: "";
                 settings.overwrite =
                     true;  // NSSavePanel already confirmed this.

                 ExportPlan plan;
                 std::string error;
                 if (!Exporter::BuildPlan(self.state->document,
                                          self.documentPath.UTF8String ?: "",
                                          settings, plan, error)) {
                     [self showExportError:error];
                     return;
                 }

                 NSAlert* progressAlert = [NSAlert new];
                 progressAlert.messageText = @"Export HEVC en cours";
                 progressAlert.informativeText = @"Préparation du rendu…";
                 [progressAlert addButtonWithTitle:@"Annuler"];
                 NSProgressIndicator* indicator = [[NSProgressIndicator alloc]
                     initWithFrame:NSMakeRect(0, 0, 420, 20)];
                 indicator.indeterminate = NO;
                 indicator.minValue = 0.0;
                 indicator.maxValue = 100.0;
                 progressAlert.accessoryView = indicator;
                 self.state->exportCancel.store(false);
                 self.state->exportRunning = true;
                 [progressAlert
                     beginSheetModalForWindow:self.window
                            completionHandler:^(NSModalResponse) {
                              if (self.state && self.state->exportRunning)
                                  self.state->exportCancel.store(true);
                            }];

                 dispatch_async(
                     dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
                       std::string renderError;
                       const bool ok = Exporter::Run(
                           plan,
                           [&](const ExportProgress& progress) {
                               dispatch_async(dispatch_get_main_queue(), ^{
                                 indicator.doubleValue =
                                     progress.fraction * 100.0;
                                 progressAlert.informativeText = [NSString
                                     stringWithFormat:
                                         @"%lld / %lld images — %.2fx",
                                         static_cast<long long>(
                                             progress.rendered_frames),
                                         static_cast<long long>(
                                             progress.total_frames),
                                         progress.speed];
                               });
                           },
                           &self.state->exportCancel, renderError);
                       dispatch_async(dispatch_get_main_queue(), ^{
                         self.state->exportRunning = false;
                         if (self.window.attachedSheet == progressAlert.window)
                             [self.window endSheet:progressAlert.window];
                         if (!ok) {
                             if (renderError != "export cancelled")
                                 [self showExportError:renderError];
                             return;
                         }
                         NSAlert* complete = [NSAlert new];
                         complete.messageText = @"Export terminé";
                         complete.informativeText = save.URL.path;
                         [complete beginSheetModalForWindow:self.window
                                          completionHandler:nil];
                       });
                     });
               }];
}

- (void)applySonyColorPreset:(id)sender {
    (void)sender;
    ColorManagementSettings settings;
    settings.enabled = true;
    settings.input_gamut = "sony_sgamut3_cine";
    settings.input_transfer = "sony_slog3";
    settings.input_ycbcr_matrix = "bt709";
    settings.input_range = "full";
    settings.working_gamut = "acescct";
    settings.output_gamut = "rec2020";
    settings.output_transfer = "hlg";
    [self commitColorSettings:settings];
}

- (void)disableColorManagement:(id)sender {
    (void)sender;
    ColorManagementSettings settings = self.state->document.color_management;
    settings.enabled = false;
    [self commitColorSettings:settings];
}

- (void)configureColorManagement:(id)sender {
    (void)sender;
    const ColorManagementSettings& current =
        self.state->document.color_management;
    NSAlert* alert = [NSAlert new];
    alert.messageText = @"Gestion colorimétrique du projet";
    alert.informativeText =
        @"Les transformations sont calculées en linéaire dans l’espace de "
         "composition AP1, avec ACEScct pour le grading. Pour les rushes "
         "Sony habituels, utilisez S-Gamut3.Cine / S-Log3 en plage Full, "
         "puis Rec.2020 / HLG.";
    [alert addButtonWithTitle:@"Appliquer"];
    [alert addButtonWithTitle:@"Annuler"];

    NSView* accessory =
        [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 500, 264)];
    NSButton* enabled = [NSButton checkboxWithTitle:@"Activer pour ce projet"
                                             target:nil
                                             action:nil];
    enabled.frame = NSMakeRect(0, 236, 500, 24);
    enabled.state =
        current.enabled ? NSControlStateValueOn : NSControlStateValueOff;
    [accessory addSubview:enabled];

    CGFloat y = 202.0;
    auto addPopup = [&](NSString* labelText,
                        std::initializer_list<std::pair<NSString*, NSString*>>
                            choices,
                        const std::string& selected) {
        NSTextField* label = [NSTextField labelWithString:labelText];
        label.frame = NSMakeRect(0, y + 3.0, 175, 20);
        label.alignment = NSTextAlignmentRight;
        [accessory addSubview:label];
        NSPopUpButton* popup =
            [[NSPopUpButton alloc] initWithFrame:NSMakeRect(188, y, 300, 26)
                                       pullsDown:NO];
        for (const auto& choice : choices) {
            [popup addItemWithTitle:choice.first];
            popup.lastItem.representedObject = choice.second;
            if ([choice.second
                    isEqualToString:[NSString
                                        stringWithUTF8String:selected.c_str()]])
                [popup selectItem:popup.lastItem];
        }
        [accessory addSubview:popup];
        y -= 32.0;
        return popup;
    };
    NSPopUpButton* inputGamut =
        addPopup(@"Gamut d’entrée :",
                 {{@"Sony S-Gamut3.Cine", @"sony_sgamut3_cine"},
                  {@"Sony S-Gamut3", @"sony_sgamut3"},
                  {@"Rec.709", @"rec709"},
                  {@"Rec.2020", @"rec2020"}},
                 current.input_gamut);
    NSPopUpButton* inputTransfer = addPopup(@"Courbe d’entrée :",
                                            {{@"Sony S-Log3", @"sony_slog3"},
                                             {@"Rec.709", @"rec709"},
                                             {@"Linéaire", @"linear"}},
                                            current.input_transfer);
    NSPopUpButton* ycbcr = addPopup(@"Matrice YCbCr :",
                                    {{@"Auto (métadonnées)", @"auto"},
                                     {@"BT.709", @"bt709"},
                                     {@"BT.2020 non constante", @"bt2020_ncl"}},
                                    current.input_ycbcr_matrix);
    NSPopUpButton* inputRange = addPopup(@"Plage du signal :",
                                         {{@"Auto (métadonnées)", @"auto"},
                                          {@"Full / Extended", @"full"},
                                          {@"Legal / Limited", @"limited"}},
                                         current.input_range);
    NSPopUpButton* working = addPopup(@"Espace de travail :",
                                      {{@"ACEScct (AP1)", @"acescct"},
                                       {@"Rec.2020 linéaire", @"rec2020"},
                                       {@"Rec.709 linéaire", @"rec709"}},
                                      current.working_gamut);
    NSPopUpButton* outputGamut =
        addPopup(@"Gamut de sortie :",
                 {{@"Rec.2020", @"rec2020"}, {@"Rec.709", @"rec709"}},
                 current.output_gamut);
    NSPopUpButton* outputTransfer = addPopup(
        @"Courbe de sortie :", {{@"HLG", @"hlg"}, {@"Rec.709", @"rec709"}},
        current.output_transfer);
    alert.accessoryView = accessory;
    if ([alert runModal] != NSAlertFirstButtonReturn) return;

    auto value = [](NSPopUpButton* popup) {
        NSString* string = popup.selectedItem.representedObject;
        return std::string(string.UTF8String ?: "");
    };
    ColorManagementSettings settings;
    settings.enabled = enabled.state == NSControlStateValueOn;
    settings.input_gamut = value(inputGamut);
    settings.input_transfer = value(inputTransfer);
    settings.input_ycbcr_matrix = value(ycbcr);
    settings.input_range = value(inputRange);
    settings.working_gamut = value(working);
    settings.output_gamut = value(outputGamut);
    settings.output_transfer = value(outputTransfer);
    if (settings.output_transfer == "hlg" &&
        settings.output_gamut != "rec2020") {
        NSAlert* invalid = [NSAlert new];
        invalid.messageText = @"Configuration incompatible";
        invalid.informativeText =
            @"La sortie HLG doit utiliser le gamut Rec.2020.";
        [invalid runModal];
        return;
    }
    [self commitColorSettings:settings];
}

- (NSArray<NSString*>*)recentProjectPaths {
    NSArray* stored = [NSUserDefaults.standardUserDefaults
        arrayForKey:@"RecentProjectPaths.v2"];
    NSMutableArray<NSString*>* available = [NSMutableArray array];
    for (id value in stored) {
        if (![value isKindOfClass:NSString.class]) continue;
        BOOL directory = NO;
        if ([NSFileManager.defaultManager fileExistsAtPath:value
                                               isDirectory:&directory] &&
            !directory)
            [available addObject:value];
        if (available.count == 8) break;
    }
    return available;
}

- (void)recordRecentProject:(NSString*)path {
    if (path.length == 0) return;
    NSString* standardized = path.stringByStandardizingPath;
    NSMutableArray<NSString*>* recent =
        [NSMutableArray arrayWithObject:standardized];
    for (NSString* candidate in [self recentProjectPaths]) {
        if (![candidate isEqualToString:standardized])
            [recent addObject:candidate];
        if (recent.count == 8) break;
    }
    [NSUserDefaults.standardUserDefaults setObject:recent
                                            forKey:@"RecentProjectPaths.v2"];
}

- (void)startupNewPressed:(id)sender {
    (void)sender;
    self.startupChoice = 1;
    [NSApp stopModal];
    [self.startupWindow orderOut:nil];
}

- (void)startupOpenPressed:(id)sender {
    (void)sender;
    self.startupChoice = 2;
    [NSApp stopModal];
    [self.startupWindow orderOut:nil];
}

- (void)startupRecentPressed:(id)sender {
    (void)sender;
    NSString* path = self.recentProjectsPopup.selectedItem.representedObject;
    if (path.length == 0) return;
    self.startupSelectedPath = path;
    self.startupChoice = 3;
    [NSApp stopModal];
    [self.startupWindow orderOut:nil];
}

- (void)startupQuitPressed:(id)sender {
    (void)sender;
    self.startupChoice = 0;
    [NSApp stopModal];
    [self.startupWindow orderOut:nil];
}

- (void)showStartupWindow {
    const NSRect frame = NSMakeRect(0, 0, 680, 430);
    self.startupWindow =
        [[NSWindow alloc] initWithContentRect:frame
                                    styleMask:NSWindowStyleMaskTitled
                                      backing:NSBackingStoreBuffered
                                        defer:NO];
    self.startupWindow.title = @"Bienvenue dans CUTMACHINE";
    self.startupWindow.releasedWhenClosed = NO;
    [self.startupWindow center];

    NSView* content = [[NSView alloc] initWithFrame:frame];
    content.wantsLayer = YES;
    content.layer.backgroundColor =
        [NSColor colorWithWhite:0.075 alpha:1.0].CGColor;
    self.startupWindow.contentView = content;

    NSImageView* icon =
        [[NSImageView alloc] initWithFrame:NSMakeRect(54, 278, 92, 92)];
    icon.image = NSApp.applicationIconImage
                     ?: SystemSymbol(@"scissors", @"CUTMACHINE", 64.0);
    icon.imageScaling = NSImageScaleProportionallyUpOrDown;
    [content addSubview:icon];

    NSTextField* title = [NSTextField labelWithString:@"CUTMACHINE"];
    title.frame = NSMakeRect(170, 325, 455, 42);
    title.font = [NSFont systemFontOfSize:30.0 weight:NSFontWeightBold];
    title.textColor = NSColor.labelColor;
    [content addSubview:title];
    NSTextField* subtitle = [NSTextField
        labelWithString:
            @"Commencez un nouveau montage ou reprenez un projet existant."];
    subtitle.frame = NSMakeRect(172, 294, 445, 24);
    subtitle.font = [NSFont systemFontOfSize:14.0];
    subtitle.textColor = NSColor.secondaryLabelColor;
    [content addSubview:subtitle];

    NSButton* create = [NSButton buttonWithTitle:@"Nouveau projet"
                                          target:self
                                          action:@selector(startupNewPressed:)];
    create.frame = NSMakeRect(54, 211, 274, 56);
    create.bezelStyle = NSBezelStyleTexturedRounded;
    create.font = [NSFont systemFontOfSize:15.0 weight:NSFontWeightSemibold];
    create.image = SystemSymbol(@"plus.square", @"Nouveau projet", 20.0);
    create.imagePosition = NSImageLeading;
    create.keyEquivalent = @"\r";
    [content addSubview:create];

    NSButton* open = [NSButton buttonWithTitle:@"Ouvrir un projet…"
                                        target:self
                                        action:@selector(startupOpenPressed:)];
    open.frame = NSMakeRect(352, 211, 274, 56);
    open.bezelStyle = NSBezelStyleTexturedRounded;
    open.font = [NSFont systemFontOfSize:15.0 weight:NSFontWeightSemibold];
    open.image = SystemSymbol(@"folder", @"Ouvrir un projet", 20.0);
    open.imagePosition = NSImageLeading;
    [content addSubview:open];

    NSTextField* recentLabel = [NSTextField labelWithString:@"Projets récents"];
    recentLabel.frame = NSMakeRect(54, 158, 160, 22);
    recentLabel.font = [NSFont systemFontOfSize:12.0
                                         weight:NSFontWeightSemibold];
    recentLabel.textColor = NSColor.secondaryLabelColor;
    [content addSubview:recentLabel];
    self.recentProjectsPopup =
        [[NSPopUpButton alloc] initWithFrame:NSMakeRect(54, 116, 460, 34)];
    NSArray<NSString*>* recentPaths = [self recentProjectPaths];
    if (recentPaths.count == 0) {
        [self.recentProjectsPopup addItemWithTitle:@"Aucun projet récent"];
        self.recentProjectsPopup.enabled = NO;
    } else {
        for (NSString* path in recentPaths) {
            NSString* title =
                path.lastPathComponent.stringByDeletingPathExtension;
            if ([title.pathExtension.lowercaseString
                    isEqualToString:@"cutmachine"])
                title = title.stringByDeletingPathExtension;
            [self.recentProjectsPopup addItemWithTitle:title];
            self.recentProjectsPopup.lastItem.representedObject = path;
            self.recentProjectsPopup.lastItem.toolTip = path;
        }
    }
    [content addSubview:self.recentProjectsPopup];
    NSButton* recent =
        [NSButton buttonWithTitle:@"Ouvrir"
                           target:self
                           action:@selector(startupRecentPressed:)];
    recent.frame = NSMakeRect(526, 116, 100, 34);
    recent.enabled = recentPaths.count > 0;
    [content addSubview:recent];

    NSButton* quit = [NSButton buttonWithTitle:@"Quitter"
                                        target:self
                                        action:@selector(startupQuitPressed:)];
    quit.frame = NSMakeRect(526, 38, 100, 30);
    quit.keyEquivalent = @"\e";
    [content addSubview:quit];

    [self.startupWindow makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    [NSApp runModalForWindow:self.startupWindow];
}

- (BOOL)createStartupProject {
    NSAlert* nameAlert = [NSAlert new];
    nameAlert.messageText = @"Créer un nouveau projet";
    nameAlert.informativeText = @"Donnez un nom au projet.";
    [nameAlert addButtonWithTitle:@"Continuer"];
    [nameAlert addButtonWithTitle:@"Annuler"];
    NSTextField* name =
        [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 380, 26)];
    name.stringValue = @"Nouveau projet";
    name.placeholderString = @"Nom du projet";
    nameAlert.accessoryView = name;
    if ([nameAlert runModal] != NSAlertFirstButtonReturn) return NO;
    NSString* projectName = [name.stringValue
        stringByTrimmingCharactersInSet:NSCharacterSet
                                            .whitespaceAndNewlineCharacterSet];
    if (projectName.length == 0) projectName = @"Nouveau projet";

    NSSavePanel* save = [NSSavePanel savePanel];
    save.title = @"Enregistrer le nouveau projet";
    save.prompt = @"Créer";
    save.nameFieldStringValue =
        [projectName stringByAppendingString:@".cutmachine-project"];
    UTType* collectionType =
        [UTType typeWithIdentifier:@"com.cutmachine.project-collection"];
    if (collectionType) save.allowedContentTypes = @[ collectionType ];
    save.canCreateDirectories = YES;
    if ([save runModal] != NSModalResponseOK || !save.URL) return NO;

    Project project(projectName.UTF8String ?: "Nouveau projet");
    project.timelines.front().name = "Timeline 1";
    std::string error;
    std::string path;
    if (!CreatePortableProject(save.URL.fileSystemRepresentation ?: "", project,
                               path, error)) {
        NSAlert* failure = [NSAlert new];
        failure.alertStyle = NSAlertStyleCritical;
        failure.messageText = @"Impossible de créer le projet";
        failure.informativeText = [NSString stringWithUTF8String:error.c_str()];
        [failure runModal];
        return NO;
    }
    self.documentPath = [NSString stringWithUTF8String:path.c_str()];
    return YES;
}

- (NSString*)resolvedProjectPath:(NSString*)selection {
    NSString* selected = selection.stringByStandardizingPath;
    BOOL directory = NO;
    if ([NSFileManager.defaultManager fileExistsAtPath:selected
                                           isDirectory:&directory] &&
        directory)
        selected = [selected
            stringByAppendingPathComponent:@"project.cutmachine.json"];
    return selected;
}

- (BOOL)openStartupProject {
    NSOpenPanel* open = [NSOpenPanel openPanel];
    open.title = @"Ouvrir un projet CUTMACHINE";
    open.prompt = @"Ouvrir";
    UTType* collectionType =
        [UTType typeWithIdentifier:@"com.cutmachine.project-collection"];
    if (collectionType) open.allowedContentTypes = @[ collectionType ];
    open.allowsMultipleSelection = NO;
    open.canChooseFiles = YES;
    open.canChooseDirectories = YES;
    if ([open runModal] != NSModalResponseOK || !open.URL) return NO;
    self.documentPath = [self resolvedProjectPath:open.URL.path];
    return YES;
}

- (void)collectPortableProject:(id)sender {
    (void)sender;
    if (!self.state || self.documentPath.length == 0) return;
    NSSavePanel* save = [NSSavePanel savePanel];
    save.title = @"Collecter le projet et ses médias";
    save.prompt = @"Collecter";
    save.canCreateDirectories = YES;
    NSString* projectName =
        [NSString stringWithUTF8String:self.state->project.name.c_str()];
    save.nameFieldStringValue =
        [projectName stringByAppendingString:@".cutmachine-project"];
    [save
        beginSheetModalForWindow:self.window
               completionHandler:^(NSModalResponse response) {
                 if (response != NSModalResponseOK || !save.URL) return;
                 NSString* source = [self.documentPath copy];
                 NSString* destination = [save.URL.path copy];
                 NSAlert* progress = [NSAlert new];
                 progress.messageText = @"Collecte du projet en cours";
                 progress.informativeText =
                     @"Copie et vérification des médias originaux…";
                 NSProgressIndicator* indicator = [[NSProgressIndicator alloc]
                     initWithFrame:NSMakeRect(0, 0, 420, 18)];
                 indicator.indeterminate = YES;
                 [indicator startAnimation:nil];
                 progress.accessoryView = indicator;
                 [progress beginSheetModalForWindow:self.window
                                  completionHandler:nil];

                 auto result = std::make_shared<PortableProjectResult>();
                 auto error = std::make_shared<std::string>();
                 dispatch_async(
                     dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
                       const bool success = CollectPortableProject(
                           source.fileSystemRepresentation ?: "",
                           destination.fileSystemRepresentation ?: "", *result,
                           *error);
                       dispatch_async(dispatch_get_main_queue(), ^{
                         [self.window endSheet:progress.window];
                         NSAlert* finished = [NSAlert new];
                         if (!success) {
                             finished.alertStyle = NSAlertStyleCritical;
                             finished.messageText = @"Collecte impossible";
                             finished.informativeText =
                                 [NSString stringWithUTF8String:error->c_str()];
                             [finished beginSheetModalForWindow:self.window
                                              completionHandler:nil];
                             return;
                         }
                         finished.messageText = @"Projet portable créé";
                         finished.informativeText = [NSString
                             stringWithFormat:
                                 @"%llu média%@ · %.2f Go copiés\n%@",
                                 static_cast<unsigned long long>(
                                     result->media_count),
                                 result->media_count == 1 ? @"" : @"s",
                                 result->media_bytes / 1000000000.0,
                                 destination];
                         [finished
                             addButtonWithTitle:@"Révéler dans le Finder"];
                         [finished addButtonWithTitle:@"Fermer"];
                         [finished
                             beginSheetModalForWindow:self.window
                                    completionHandler:^(
                                        NSModalResponse choice) {
                                      if (choice == NSAlertFirstButtonReturn)
                                          [NSWorkspace.sharedWorkspace
                                              activateFileViewerSelectingURLs:@[
                                                  save.URL
                                              ]];
                                    }];
                       });
                     });
               }];
}

- (BOOL)chooseStartupProject {
    while (self.documentPath.length == 0) {
        self.startupChoice = 0;
        self.startupSelectedPath = nil;
        [self showStartupWindow];
        if (self.startupChoice == 0) return NO;
        if (self.startupChoice == 1) {
            if ([self createStartupProject]) return YES;
        } else if (self.startupChoice == 2) {
            if ([self openStartupProject]) return YES;
        } else if (self.startupChoice == 3 &&
                   self.startupSelectedPath.length > 0) {
            self.documentPath = self.startupSelectedPath;
            return YES;
        }
    }
    return YES;
}

- (instancetype)initWithDocumentPath:(NSString*)documentPath {
    if ((self = [super init])) {
        _documentPath = [documentPath copy];
        _state = new AppState();
        _mediaThumbnails = [NSMutableDictionary dictionary];
    }
    return self;
}

- (BOOL)loadDocumentAndSources {
    const std::string documentPath(self.documentPath.UTF8String ?: "");
    std::string error;
    self.lastProjectLoadError = nil;
    self.state->projectLock = std::make_unique<ProjectSessionLock>();
    std::string lockOwner;
    if (!self.state->projectLock->Acquire(documentPath, lockOwner, error)) {
        self.lastProjectLoadError =
            [NSString stringWithUTF8String:error.c_str()];
        return NO;
    }
    CollectionIntegrityReport integrity;
    if (!VerifyPortableProject(documentPath, integrity, error)) {
        self.lastProjectLoadError = [NSString
            stringWithFormat:@"Paquet projet invalide : %s", error.c_str()];
        return NO;
    }
    if (!integrity.missing_media.empty() || !integrity.modified_media.empty()) {
        NSAlert* warning = [NSAlert new];
        warning.alertStyle = NSAlertStyleCritical;
        warning.messageText = @"Intégrité du projet compromise";
        warning.informativeText = [NSString
            stringWithFormat:@"%lu média%@ manquant%@ et %lu modifié%@. "
                             @"Le montage peut être ouvert, mais son rendu "
                              "n’est plus garanti identique.",
                             (unsigned long)integrity.missing_media.size(),
                             integrity.missing_media.size() == 1 ? @"" : @"s",
                             integrity.missing_media.size() == 1 ? @"" : @"s",
                             (unsigned long)integrity.modified_media.size(),
                             integrity.modified_media.size() == 1 ? @"" : @"s"];
        [warning addButtonWithTitle:@"Ouvrir quand même"];
        [warning addButtonWithTitle:@"Annuler"];
        if ([warning runModal] != NSAlertFirstButtonReturn) {
            self.lastProjectLoadError =
                @"Ouverture annulée après contrôle d’intégrité.";
            return NO;
        }
    }
    bool recoveredAutosave = false;
    const ProjectRecoveryInfo recovery = ProjectRecovery::Inspect(documentPath);
    if (recovery.state == ProjectRecoveryState::Available) {
        NSAlert* alert = [NSAlert new];
        alert.messageText = @"Récupération disponible";
        alert.informativeText = @"Une sauvegarde automatique plus récente que "
                                @"le projet a été trouvée.";
        [alert addButtonWithTitle:@"Récupérer"];
        [alert addButtonWithTitle:@"Ignorer"];
        if ([alert runModal] == NSAlertFirstButtonReturn) {
            Project recovered;
            if (!ProjectRecovery::LoadAutosave(documentPath, recovered,
                                               error)) {
                std::fprintf(stderr, "Unable to recover project: %s\n",
                             error.c_str());
                return NO;
            }
            std::map<std::string, EditLog> recoveredLogs;
            for (const DocumentSequence& timeline : recovered.timelines)
                recoveredLogs.emplace(timeline.id, EditLog{});
            if (!CommitStoredProjectAndLogs(documentPath, recovered,
                                            recoveredLogs, ProjectEditLog{},
                                            error)) {
                std::fprintf(stderr, "Unable to recover project: %s\n",
                             error.c_str());
                return NO;
            }
            self.state->project = std::move(recovered);
            recoveredAutosave = true;
        }
        if (!ProjectRecovery::DiscardAutosave(documentPath, error))
            std::fprintf(stderr, "Unable to discard autosave: %s\n",
                         error.c_str());
    } else if (recovery.state == ProjectRecoveryState::Invalid) {
        std::fprintf(stderr, "Invalid autosave ignored: %s\n",
                     recovery.error.c_str());
    }
    if (!recoveredAutosave &&
        !LoadStoredProject(documentPath, self.state->project, error)) {
        std::fprintf(stderr, "Unable to load project: %s\n", error.c_str());
        return NO;
    }
    self.state->activeTimelineId = self.state->project.active_timeline_id;
    self.state->document =
        self.state->project.MakeDocument(self.state->activeTimelineId);
    EditError editError = EditError::None;
    for (const DocumentSequence& timeline : self.state->project.timelines) {
        const std::string logPath =
            TimelineEditLogPathForProject(documentPath, timeline.id);
        std::error_code logExistsError;
        if (std::filesystem::exists(logPath, logExistsError)) {
            EditLog log;
            if (!EditLog::Load(logPath, log, editError, error)) {
                std::fprintf(stderr, "Unable to load timeline edit log: %s\n",
                             error.c_str());
                return NO;
            }
            self.state->timelineEditLogs[timeline.id] = std::move(log);
        }
        if (logExistsError) {
            std::fprintf(stderr, "Unable to inspect timeline edit log: %s\n",
                         logExistsError.message().c_str());
            return NO;
        }
    }
    const auto activeLog =
        self.state->timelineEditLogs.find(self.state->activeTimelineId);
    self.state->editLog = activeLog == self.state->timelineEditLogs.end()
                              ? EditLog{}
                              : activeLog->second;
    const std::string projectLogPath =
        ProjectEditLogPathForProject(documentPath);
    std::error_code projectLogExistsError;
    if (std::filesystem::exists(projectLogPath, projectLogExistsError) &&
        !ProjectEditLog::Load(projectLogPath, self.state->projectEditLog,
                              editError, error)) {
        std::fprintf(stderr, "Unable to load project edit log: %s\n",
                     error.c_str());
        return NO;
    }
    if (projectLogExistsError) {
        std::fprintf(stderr, "Unable to inspect project edit log: %s\n",
                     projectLogExistsError.message().c_str());
        return NO;
    }
    self.state->timelineEditLogs[self.state->activeTimelineId] =
        self.state->editLog;
    for (const DocumentTrack& track : self.state->document.sequence.tracks)
        self.state->targetedTrackIds.insert(track.id);
    self.state->timelineTargetTracks[self.state->activeTimelineId] =
        self.state->targetedTrackIds;
    self.state->viewport.view_start = {0, 1};
    self.state->viewport.pixels_per_second = 100.0;
    self.state->viewport.track_height = 34.0;
    self.state->viewport.header_width = 176.0;
    self.state->interaction = std::make_unique<TimelineInteraction>(
        self.state->document, self.state->editLog, self.state->viewport);
    self.state->timeline = std::make_unique<Timeline>(self.state->document);
    try {
        self.state->duration = self.state->timeline->Duration();
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "Invalid timeline duration: %s\n",
                     exception.what());
        return NO;
    }
    self.state->frameCache = std::make_unique<FrameCache>(kGlobalCacheBudget);
    self.state->performanceMetrics = std::make_unique<PerformanceMetrics>();
    const std::filesystem::path baseDirectory =
        std::filesystem::absolute(std::filesystem::path(documentPath))
            .parent_path();
    for (const DocumentSource& source : self.state->document.sources) {
        const LibraryMedia* media =
            self.state->document.FindLibraryMedia(source.id);
        std::filesystem::path mediaPath(source.path);
        if (mediaPath.is_relative()) mediaPath = baseDirectory / mediaPath;
        LibraryMedia detected;
        detected.id = source.id;
        detected.path = media ? media->path : source.path;
        detected.filename = mediaPath.filename().string();
        std::string probeError;
        if (ProbeMediaMetadata(mediaPath.lexically_normal().string(), detected,
                               probeError)) {
            self.state->mediaMetadata[source.id] = detected;
            std::fprintf(
                stderr,
                "Media %s: %dx%d, %s/%s, range=%s matrix=%s transfer=%s "
                "primaries=%s, rotation %d degrees, %s\n",
                detected.filename.c_str(), detected.width, detected.height,
                detected.codec.c_str(), detected.pixel_format.c_str(),
                detected.color_range.c_str(), detected.color_space.c_str(),
                detected.color_transfer.c_str(),
                detected.color_primaries.c_str(), detected.rotation_degrees,
                detected.orientation.c_str());
        } else {
            std::fprintf(stderr, "Metadata probe failed for %s: %s\n",
                         mediaPath.string().c_str(), probeError.c_str());
            if (media && media->metadata_complete)
                self.state->mediaMetadata[source.id] = *media;
        }
    }
    if (![self separateEmbeddedAudioByDefault:error]) {
        std::fprintf(stderr, "Unable to separate embedded audio: %s\n",
                     error.c_str());
        return NO;
    }
    self.state->audioPlayback = std::make_unique<AudioPlayback>();
    if (!self.state->audioPlayback->Open(self.state->document,
                                         baseDirectory.string(), error)) {
        std::fprintf(stderr, "Unable to initialize audio: %s\n", error.c_str());
        return NO;
    }
    for (const DocumentSource& source : self.state->document.sources) {
        const LibraryMedia* media =
            self.state->document.FindLibraryMedia(source.id);
        std::filesystem::path mediaPath(source.path);
        if (mediaPath.is_relative()) {
            mediaPath = baseDirectory / mediaPath;
        }
        const std::filesystem::path originalPath = mediaPath;
        if (self.state->proxiesEnabled && media && !media->proxy_path.empty()) {
            std::filesystem::path proxyPath(media->proxy_path);
            if (proxyPath.is_relative()) proxyPath = baseDirectory / proxyPath;
            std::error_code proxyError;
            if (std::filesystem::is_regular_file(proxyPath, proxyError) &&
                !proxyError)
                mediaPath = proxyPath;
        }
        const bool usingProxy = mediaPath != originalPath;
        auto worker =
            std::make_unique<DecodeWorker>(source.id, *self.state->frameCache,
                                           *self.state->performanceMetrics);
        if (!worker->Open(mediaPath.lexically_normal().string(), 5)) {
            if (mediaPath != originalPath) {
                worker = std::make_unique<DecodeWorker>(
                    source.id, *self.state->frameCache,
                    *self.state->performanceMetrics);
            }
            if (mediaPath == originalPath ||
                !worker->Open(originalPath.lexically_normal().string(), 5)) {
                std::fprintf(stderr, "Source offline %s at %s\n",
                             source.id.c_str(), originalPath.string().c_str());
                self.state->offlineSourceIds.insert(source.id);
                continue;
            }
        }
        if (usingProxy && !DecodeWorkerMatchesSource(*worker, source)) {
            std::fprintf(stderr,
                         "Proxy incompatible for source %s; using original\n",
                         source.id.c_str());
            worker = std::make_unique<DecodeWorker>(
                source.id, *self.state->frameCache,
                *self.state->performanceMetrics);
            if (!worker->Open(originalPath.lexically_normal().string(), 5)) {
                self.state->offlineSourceIds.insert(source.id);
                continue;
            }
        }
        if (!DecodeWorkerMatchesSource(*worker, source)) {
            std::fprintf(
                stderr, "Source %s declares rate %d/%d but media is %d/%d\n",
                source.id.c_str(), source.rate.num, source.rate.den,
                worker->FrameRateNumerator(), worker->FrameRateDenominator());
            return NO;
        }
        self.state->workers.emplace(source.id, std::move(worker));
    }

    [self rebuildVideoTrackIds];
    return YES;
}

- (BOOL)separateEmbeddedAudioByDefault:(std::string&)message {
    std::vector<Ulid> videoClipIds;
    for (const DocumentTrack& track : self.state->document.sequence.tracks) {
        if (track.kind != "video") continue;
        for (const DocumentClip& clip : track.clips) {
            if (!clip.include_audio) continue;
            const auto detected =
                self.state->mediaMetadata.find(clip.source_id);
            const LibraryMedia* media =
                detected == self.state->mediaMetadata.end()
                    ? self.state->document.FindLibraryMedia(clip.source_id)
                    : &detected->second;
            if (media && media->metadata_complete && media->has_audio)
                videoClipIds.push_back(clip.id);
        }
    }
    EditError error = EditError::None;
    for (const Ulid& videoClipId : videoClipIds) {
        const Ulid audioClipId = GenerateUlid();
        Ulid targetTrackId;
        for (const DocumentTrack* track :
             TimelineTracksInDisplayOrder(self.state->document)) {
            if (track->kind != "audio") continue;
            Document candidate = self.state->document;
            Operation probe =
                DetachAudioOperation{videoClipId, track->id, audioClipId, {}};
            Operation inverse = RemoveClipOperation{};
            EditError probeError = EditError::None;
            std::string probeMessage;
            if (ApplyOperation(candidate, probe, inverse, probeError,
                               probeMessage)) {
                targetTrackId = track->id;
                break;
            }
        }
        if (targetTrackId.empty()) {
            int32_t index = 0;
            for (const DocumentTrack& track :
                 self.state->document.sequence.tracks)
                index = std::max(index, track.index + 1);
            targetTrackId = GenerateUlid();
            if (!self.state->editLog.Apply(
                    self.state->document,
                    Operation{AddTrackOperation{targetTrackId, "audio", index}},
                    error, message))
                return NO;
        }
        if (!self.state->editLog.Apply(
                self.state->document,
                Operation{DetachAudioOperation{
                    videoClipId, targetTrackId, audioClipId, {}}},
                error, message))
            return NO;
    }
    size_t migratedLinks = 0;
    std::vector<Ulid> claimedAudio;
    struct LegacyPair {
        Ulid video;
        Ulid audio;
        Ulid group;
    };
    std::vector<LegacyPair> legacyPairs;
    for (const DocumentTrack& videoTrack :
         self.state->document.sequence.tracks) {
        if (videoTrack.kind != "video") continue;
        for (const DocumentClip& video : videoTrack.clips) {
            if (video.include_audio || !video.sync_anchor_clip_id.empty())
                continue;
            const DocumentClip* match = nullptr;
            for (const DocumentTrack& audioTrack :
                 self.state->document.sequence.tracks) {
                if (audioTrack.kind != "audio") continue;
                for (const DocumentClip& audio : audioTrack.clips) {
                    if (!audio.sync_anchor_clip_id.empty() ||
                        std::find(claimedAudio.begin(), claimedAudio.end(),
                                  audio.id) != claimedAudio.end() ||
                        audio.source_id != video.source_id ||
                        audio.source_in != video.source_in ||
                        audio.duration != video.duration ||
                        audio.timeline_in != video.timeline_in)
                        continue;
                    match = &audio;
                    break;
                }
                if (match) break;
            }
            if (!match) continue;
            const Ulid audioId = match->id;
            const Ulid groupId =
                !video.link_group_id.empty()
                    ? video.link_group_id
                    : (!match->link_group_id.empty() ? match->link_group_id
                                                     : audioId);
            legacyPairs.push_back({video.id, audioId, groupId});
            claimedAudio.push_back(audioId);
        }
    }
    for (const auto& pair : legacyPairs) {
        if (!self.state->editLog.Apply(
                self.state->document,
                Operation{SetClipLinkOperation{pair.video, pair.audio,
                                               pair.group, pair.group}},
                error, message))
            return NO;
        ++migratedLinks;
    }
    if (!videoClipIds.empty() || migratedLinks > 0) {
        if (![self persistEdits:message]) return NO;
        std::fprintf(
            stderr,
            "Separated %zu embedded audio clip(s), migrated %zu A/V link(s)\n",
            videoClipIds.size(), migratedLinks);
    }
    return YES;
}

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    (void)notification;
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    const BOOL pathWasProvided = self.documentPath.length > 0;
    while (true) {
        if (self.documentPath.length == 0 && ![self chooseStartupProject]) {
            [NSApp terminate:nil];
            return;
        }
        if ([self loadDocumentAndSources]) break;
        NSAlert* failure = [NSAlert new];
        failure.alertStyle = NSAlertStyleCritical;
        failure.messageText = @"Impossible d’ouvrir le projet";
        failure.informativeText =
            self.lastProjectLoadError
                ?: @"Le fichier n’est pas un projet CUTMACHINE valide ou il "
                   @"est inaccessible.";
        [failure runModal];
        if (pathWasProvided) {
            [NSApp terminate:nil];
            return;
        }
        self.documentPath = nil;
        delete self.state;
        self.state = new AppState();
        self.mediaThumbnails = [NSMutableDictionary dictionary];
    }
    [self recordRecentProject:self.documentPath];
    [self installApplicationMenus];

    const NSRect windowRect = NSMakeRect(0.0, 0.0, 1600.0, 960.0);
    self.window =
        [[NSWindow alloc] initWithContentRect:windowRect
                                    styleMask:(NSWindowStyleMaskTitled |
                                               NSWindowStyleMaskClosable |
                                               NSWindowStyleMaskResizable)
                                      backing:NSBackingStoreBuffered
                                        defer:NO];
    self.window.title =
        [NSString stringWithFormat:@"CUTMACHINE — %s — %s",
                                   self.state->project.name.c_str(),
                                   self.state->document.sequence.name.c_str()];
    self.window.delegate = self;
    self.window.acceptsMouseMovedEvents = YES;
    self.window.contentMinSize = NSMakeSize(900.0, 560.0);

    NSView* content = [[NSView alloc] initWithFrame:windowRect];
    self.window.contentView = content;
    constexpr double mediaPanelWidth = 320.0;
    // F2.1 design system: fixed-width right dock (Inspector/Chat tabs).
    // Fixed, not user-resized-and-remembered, on purpose -- see
    // PanelHostView.h.
    constexpr double rightDockWidth = 300.0;
    // F2.5 design system: fixed-height bottom dock (Transport panel: play/
    // pause, scrub bar, timecode, frame step). Sits directly above the
    // existing status bar, below the Metal-drawn timeline -- see
    // PanelHostView.h for why this height is fixed rather than
    // user-resizable.
    constexpr double bottomDockHeight = 84.0;
    const double workspaceHeight =
        windowRect.size.height - 42.0 - bottomDockHeight;
    self.workspaceSplitView = [[CutmachineSplitView alloc]
        initWithFrame:NSMakeRect(0.0, 42.0 + bottomDockHeight,
                                 windowRect.size.width, workspaceHeight)];
    self.workspaceSplitView.vertical = YES;
    self.workspaceSplitView.dividerStyle = NSSplitViewDividerStyleThin;
    self.workspaceSplitView.delegate = self;
    self.workspaceSplitView.autoresizingMask =
        NSViewWidthSizable | NSViewHeightSizable;
    self.workspaceSplitView.autosaveName = @"CUTMACHINE.WorkspaceSplit";
    [content addSubview:self.workspaceSplitView];
    self.mediaPanel = [[NSView alloc]
        initWithFrame:NSMakeRect(0.0, 0.0, mediaPanelWidth, workspaceHeight)];
    self.mediaPanel.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    self.mediaPanel.wantsLayer = YES;
    // Same surface token as the new right dock (UiTheme.h kSurfacePanel) --
    // both are side panels of the same visual kind.
    self.mediaPanel.layer.backgroundColor = CMSurfacePanelColor().CGColor;
    [self.workspaceSplitView addArrangedSubview:self.mediaPanel];

    NSTextField* libraryTitle =
        [NSTextField labelWithString:@"PROJET / CHUTIERS"];
    libraryTitle.frame =
        NSMakeRect(14.0, workspaceHeight - 34.0, mediaPanelWidth - 28.0, 18.0);
    libraryTitle.autoresizingMask = NSViewMinYMargin | NSViewWidthSizable;
    libraryTitle.font = [NSFont systemFontOfSize:11.0
                                          weight:NSFontWeightSemibold];
    libraryTitle.textColor = NSColor.secondaryLabelColor;
    [self.mediaPanel addSubview:libraryTitle];

    NSButton* addBinButton =
        [NSButton buttonWithTitle:@"+ Chutier"
                           target:self
                           action:@selector(createBinPressed:)];
    addBinButton.frame = NSMakeRect(12.0, workspaceHeight - 70.0, 142.0, 28.0);
    addBinButton.autoresizingMask = NSViewMinYMargin;
    addBinButton.bezelStyle = NSBezelStyleRounded;
    addBinButton.title = @"Chutier";
    addBinButton.image = SystemSymbol(@"folder.badge.plus", @"Nouveau chutier");
    addBinButton.imagePosition = NSImageLeading;
    [self.mediaPanel addSubview:addBinButton];

    NSButton* deleteBinButton =
        [NSButton buttonWithTitle:@"Supprimer"
                           target:self
                           action:@selector(deleteBinPressed:)];
    deleteBinButton.frame =
        NSMakeRect(166.0, workspaceHeight - 70.0, 142.0, 28.0);
    deleteBinButton.autoresizingMask = NSViewMinYMargin | NSViewMinXMargin;
    deleteBinButton.bezelStyle = NSBezelStyleRounded;
    deleteBinButton.image = SystemSymbol(@"trash", @"Supprimer le chutier");
    deleteBinButton.imagePosition = NSImageLeading;
    deleteBinButton.toolTip = @"Supprime le chutier sélectionné s’il est vide";
    [self.mediaPanel addSubview:deleteBinButton];

    self.binOutline = [[ContextOutlineView alloc]
        initWithFrame:NSMakeRect(0.0, 0.0, mediaPanelWidth - 24.0, 220.0)];
    NSTableColumn* binColumn =
        [[NSTableColumn alloc] initWithIdentifier:@"bin"];
    [self.binOutline addTableColumn:binColumn];
    self.binOutline.outlineTableColumn = binColumn;
    self.binOutline.headerView = nil;
    self.binOutline.dataSource = self;
    self.binOutline.delegate = self;
    [self.binOutline registerForDraggedTypes:@[
        kCutmachineMediaPasteboardType, kCutmachineBinPasteboardType,
        NSPasteboardTypeFileURL
    ]];
    [self.binOutline setDraggingSourceOperationMask:NSDragOperationMove
                                           forLocal:YES];
    NSScrollView* binScroll = [[NSScrollView alloc]
        initWithFrame:NSMakeRect(12.0, workspaceHeight - 300.0,
                                 mediaPanelWidth - 24.0, 220.0)];
    binScroll.autoresizingMask = NSViewMinYMargin | NSViewWidthSizable;
    binScroll.documentView = self.binOutline;
    binScroll.hasVerticalScroller = YES;
    binScroll.borderType = NSBezelBorder;
    [self.mediaPanel addSubview:binScroll];

    self.mediaSearchField = [[NSSearchField alloc]
        initWithFrame:NSMakeRect(12.0, workspaceHeight - 338.0,
                                 mediaPanelWidth - 104.0, 26.0)];
    self.mediaSearchField.placeholderString = @"Rechercher nom, codec, format…";
    self.mediaSearchField.target = self;
    self.mediaSearchField.action = @selector(mediaSearchChanged:);
    self.mediaSearchField.continuous = YES;
    self.mediaSearchField.autoresizingMask =
        NSViewMinYMargin | NSViewWidthSizable;
    [self.mediaPanel addSubview:self.mediaSearchField];

    self.mediaViewToggle = [[NSSegmentedControl alloc]
        initWithFrame:NSMakeRect(mediaPanelWidth - 84.0,
                                 workspaceHeight - 338.0, 72.0, 26.0)];
    self.mediaViewToggle.segmentCount = 2;
    [self.mediaViewToggle setImage:SystemSymbol(@"list.bullet", @"Vue liste")
                        forSegment:0];
    [self.mediaViewToggle
          setImage:SystemSymbol(@"square.grid.2x2", @"Vue grille")
        forSegment:1];
    self.mediaViewToggle.selectedSegment = 0;
    self.mediaViewToggle.target = self;
    self.mediaViewToggle.action = @selector(mediaViewChanged:);
    self.mediaViewToggle.autoresizingMask = NSViewMinYMargin;
    self.mediaViewToggle.autoresizingMask = NSViewMinYMargin | NSViewMinXMargin;
    [self.mediaPanel addSubview:self.mediaViewToggle];

    self.mediaTable = [[ContextTableView alloc]
        initWithFrame:NSMakeRect(0.0, 0.0, mediaPanelWidth - 24.0, 500.0)];
    for (NSArray<NSString*>* definition in @[
             @[ @"name", @"Nom", @"145" ], @[ @"format", @"Format", @"85" ],
             @[ @"duration", @"Durée", @"60" ]
         ]) {
        NSTableColumn* column =
            [[NSTableColumn alloc] initWithIdentifier:definition[0]];
        column.title = definition[1];
        column.width = definition[2].doubleValue;
        [self.mediaTable addTableColumn:column];
    }
    self.mediaTable.dataSource = self;
    self.mediaTable.delegate = self;
    self.mediaTable.allowsMultipleSelection = YES;
    self.mediaTable.usesAlternatingRowBackgroundColors = YES;
    self.mediaListScroll = [[NSScrollView alloc]
        initWithFrame:NSMakeRect(12.0, 112.0, mediaPanelWidth - 24.0,
                                 workspaceHeight - 462.0)];
    self.mediaListScroll.autoresizingMask =
        NSViewHeightSizable | NSViewWidthSizable;
    self.mediaListScroll.documentView = self.mediaTable;
    self.mediaListScroll.hasVerticalScroller = YES;
    self.mediaListScroll.hasHorizontalScroller = YES;
    self.mediaListScroll.borderType = NSBezelBorder;
    [self.mediaPanel addSubview:self.mediaListScroll];

    NSCollectionViewFlowLayout* iconLayout =
        [[NSCollectionViewFlowLayout alloc] init];
    iconLayout.itemSize = NSMakeSize(132.0, 112.0);
    iconLayout.minimumInteritemSpacing = 6.0;
    iconLayout.minimumLineSpacing = 8.0;
    iconLayout.sectionInset = NSEdgeInsetsMake(8, 8, 8, 8);
    self.mediaCollection = [[ContextCollectionView alloc]
        initWithFrame:NSMakeRect(0.0, 0.0, mediaPanelWidth - 24.0, 500.0)];
    self.mediaCollection.collectionViewLayout = iconLayout;
    self.mediaCollection.dataSource = self;
    self.mediaCollection.delegate = self;
    self.mediaCollection.selectable = YES;
    self.mediaCollection.allowsMultipleSelection = YES;
    [self.mediaCollection setDraggingSourceOperationMask:NSDragOperationCopy
                                                forLocal:YES];
    [self.mediaCollection registerClass:MediaIconItem.class
                  forItemWithIdentifier:@"media-icon"];
    self.mediaIconScroll =
        [[NSScrollView alloc] initWithFrame:self.mediaListScroll.frame];
    self.mediaIconScroll.autoresizingMask =
        NSViewHeightSizable | NSViewWidthSizable;
    self.mediaIconScroll.documentView = self.mediaCollection;
    self.mediaIconScroll.hasVerticalScroller = YES;
    self.mediaIconScroll.borderType = NSBezelBorder;
    self.mediaIconScroll.hidden = YES;
    [self.mediaPanel addSubview:self.mediaIconScroll];

    self.mediaTable.target = self;
    self.mediaTable.doubleAction = @selector(openSelectedMediaInSourceMonitor:);
    NSClickGestureRecognizer* iconDoubleClick =
        [[NSClickGestureRecognizer alloc]
            initWithTarget:self
                    action:@selector(openSelectedMediaInSourceMonitor:)];
    iconDoubleClick.numberOfClicksRequired = 2;
    [self.mediaCollection addGestureRecognizer:iconDoubleClick];
    [self.mediaTable setDraggingSourceOperationMask:NSDragOperationCopy
                                           forLocal:YES];

    self.assignMediaButton =
        [NSButton buttonWithTitle:@"Importer des rushes…"
                           target:self
                           action:@selector(importMediaPressed:)];
    self.assignMediaButton.frame = NSMakeRect(12.0, 76.0, 184.0, 28.0);
    self.assignMediaButton.autoresizingMask = NSViewMaxYMargin;
    self.assignMediaButton.bezelStyle = NSBezelStyleRounded;
    self.assignMediaButton.image =
        SystemSymbol(@"square.and.arrow.down", @"Importer des rushes");
    self.assignMediaButton.imagePosition = NSImageLeading;
    [self.mediaPanel addSubview:self.assignMediaButton];

    self.sourceMonitorButton =
        [NSButton buttonWithTitle:@"Source"
                           target:self
                           action:@selector(openSelectedMediaInSourceMonitor:)];
    self.sourceMonitorButton.frame = NSMakeRect(202.0, 76.0, 106.0, 28.0);
    self.sourceMonitorButton.autoresizingMask = NSViewMaxYMargin;
    self.sourceMonitorButton.autoresizingMask = NSViewMinXMargin;
    self.sourceMonitorButton.bezelStyle = NSBezelStyleRounded;
    self.sourceMonitorButton.image =
        SystemSymbol(@"rectangle.on.rectangle", @"Moniteur Source");
    self.sourceMonitorButton.imagePosition = NSImageLeading;
    self.sourceMonitorButton.toolTip =
        @"Ouvre le média sélectionné dans le moniteur source";
    [self.mediaPanel addSubview:self.sourceMonitorButton];

    NSMenu* binContext = [[NSMenu alloc] initWithTitle:@"Chutier"];
    [binContext addItem:[self menuItem:@"Nouveau chutier"
                                action:@selector(createBinPressed:)
                                   key:@""]];
    [binContext addItem:[self menuItem:@"Renommer"
                                action:@selector(renameBinPressed:)
                                   key:@""]];
    [binContext addItem:NSMenuItem.separatorItem];
    [binContext addItem:[self menuItem:@"Supprimer"
                                action:@selector(deleteBinPressed:)
                                   key:@""]];
    self.binOutline.menu = binContext;

    NSMenu* mediaContext = [[NSMenu alloc] initWithTitle:@"Média"];
    [mediaContext addItem:[self menuItem:@"Nouveau chutier"
                                  action:@selector(createBinPressed:)
                                     key:@""]];
    [mediaContext addItem:NSMenuItem.separatorItem];
    [mediaContext
        addItem:[self menuItem:@"Ouvrir dans le moniteur source"
                        action:@selector(openSelectedMediaInSourceMonitor:)
                           key:@""]];
    [mediaContext addItem:[self menuItem:@"Déplacer dans le chutier sélectionné"
                                  action:@selector(assignMediaToBinPressed:)
                                     key:@""]];
    [mediaContext addItem:NSMenuItem.separatorItem];
    [mediaContext addItem:[self menuItem:@"Révéler dans le Finder"
                                  action:@selector(revealSelectedMediaInFinder:)
                                     key:@""]];
    [mediaContext addItem:[self menuItem:@"Reconnecter le média…"
                                  action:@selector(relinkSelectedMedia:)
                                     key:@""]];
    [mediaContext addItem:[self menuItem:@"Reconnecter les médias offline…"
                                  action:@selector(batchRelinkOfflineMedia:)
                                     key:@""]];
    [mediaContext addItem:NSMenuItem.separatorItem];
    [mediaContext addItem:[self menuItem:@"Générer / recréer le proxy"
                                  action:@selector(generateSelectedMediaProxy:)
                                     key:@""]];
    [mediaContext addItem:[self menuItem:@"Supprimer le proxy"
                                  action:@selector(removeSelectedMediaProxy:)
                                     key:@""]];
    [mediaContext
        addItem:[self menuItem:@"Régénérer la vignette"
                        action:@selector(regenerateSelectedMediaThumbnail:)
                           key:@""]];
    self.mediaTable.menu = mediaContext;
    self.mediaCollection.menu = mediaContext;

    self.binSummaryLabel = [NSTextField labelWithString:@""];
    self.binSummaryLabel.frame =
        NSMakeRect(14.0, 42.0, mediaPanelWidth - 28.0, 34.0);
    self.binSummaryLabel.autoresizingMask =
        NSViewMaxYMargin | NSViewWidthSizable;
    self.binSummaryLabel.font = [NSFont systemFontOfSize:11.0];
    self.binSummaryLabel.textColor = NSColor.secondaryLabelColor;
    self.binSummaryLabel.maximumNumberOfLines = 2;
    [self.mediaPanel addSubview:self.binSummaryLabel];
    self.mediaTaskLabel = [NSTextField labelWithString:@"Tâches média : prêt"];
    self.mediaTaskLabel.frame =
        NSMakeRect(14.0, 13.0, mediaPanelWidth - 152.0, 18.0);
    self.mediaTaskLabel.autoresizingMask =
        NSViewMaxYMargin | NSViewWidthSizable;
    self.mediaTaskLabel.font = [NSFont systemFontOfSize:10.0];
    self.mediaTaskLabel.textColor = NSColor.tertiaryLabelColor;
    self.mediaTaskLabel.lineBreakMode = NSLineBreakByTruncatingMiddle;
    [self.mediaPanel addSubview:self.mediaTaskLabel];
    self.mediaTaskProgress = [[NSProgressIndicator alloc]
        initWithFrame:NSMakeRect(mediaPanelWidth - 132.0, 16.0, 88.0, 10.0)];
    self.mediaTaskProgress.autoresizingMask =
        NSViewMaxYMargin | NSViewMinXMargin;
    self.mediaTaskProgress.minValue = 0.0;
    self.mediaTaskProgress.maxValue = 100.0;
    self.mediaTaskProgress.doubleValue = 0.0;
    self.mediaTaskProgress.indeterminate = NO;
    [self.mediaPanel addSubview:self.mediaTaskProgress];
    self.mediaTaskCancelButton =
        [NSButton buttonWithTitle:@"×"
                           target:self
                           action:@selector(cancelDisplayedMediaTask:)];
    self.mediaTaskCancelButton.frame =
        NSMakeRect(mediaPanelWidth - 40.0, 7.0, 28.0, 26.0);
    self.mediaTaskCancelButton.autoresizingMask =
        NSViewMaxYMargin | NSViewMinXMargin;
    self.mediaTaskCancelButton.bezelStyle = NSBezelStyleRounded;
    self.mediaTaskCancelButton.toolTip = @"Annuler la tâche média active";
    self.mediaTaskCancelButton.enabled = NO;
    [self.mediaPanel addSubview:self.mediaTaskCancelButton];

    const double editorWidth =
        windowRect.size.width - mediaPanelWidth - rightDockWidth - 2.0;
    self.editorSplitView = [[CutmachineSplitView alloc]
        initWithFrame:NSMakeRect(0.0, 0.0, editorWidth, workspaceHeight)];
    self.editorSplitView.vertical = NO;
    self.editorSplitView.dividerStyle = NSSplitViewDividerStyleThin;
    self.editorSplitView.delegate = self;
    self.editorSplitView.autosaveName = @"CUTMACHINE.EditorSplit";
    self.editorSplitView.autoresizingMask =
        NSViewWidthSizable | NSViewHeightSizable;

    self.metalView = [[TimelineMetalView alloc]
        initWithFrame:NSMakeRect(0.0, 0.0, editorWidth,
                                 workspaceHeight * 0.56)];
    self.metalView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    self.metalView.eventTarget = self;
    self.metalView.resizeTarget = self;
    [self.editorSplitView addArrangedSubview:self.metalView];

    self.monitorSplitView = [[CutmachineSplitView alloc]
        initWithFrame:NSMakeRect(0.0, workspaceHeight * 0.56, editorWidth,
                                 workspaceHeight * 0.44)];
    self.monitorSplitView.vertical = YES;
    self.monitorSplitView.dividerStyle = NSSplitViewDividerStyleThin;
    self.monitorSplitView.delegate = self;
    self.monitorSplitView.autosaveName = @"CUTMACHINE.MonitorSplit";
    self.monitorSplitView.autoresizingMask =
        NSViewWidthSizable | NSViewHeightSizable;
    self.sourceMonitorPanel =
        [[NSView alloc] initWithFrame:NSMakeRect(0.0, 0.0, editorWidth * 0.5,
                                                 workspaceHeight * 0.44)];
    self.programMonitorPanel = [[NSView alloc]
        initWithFrame:NSMakeRect(editorWidth * 0.5, 0.0, editorWidth * 0.5,
                                 workspaceHeight * 0.44)];
    for (NSView* panel in
         @[ self.sourceMonitorPanel, self.programMonitorPanel ]) {
        panel.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        panel.wantsLayer = YES;
        panel.layer.backgroundColor = NSColor.blackColor.CGColor;
    }
    self.sourceMonitorView = [[TimelineMetalView alloc]
        initWithFrame:self.sourceMonitorPanel.bounds];
    self.sourceMonitorView.autoresizingMask =
        NSViewWidthSizable | NSViewHeightSizable;
    self.sourceMonitorView.eventTarget = self;
    self.sourceMonitorView.resizeTarget = self;
    [self.sourceMonitorPanel addSubview:self.sourceMonitorView];
    self.programMonitorView = [[TimelineMetalView alloc]
        initWithFrame:self.programMonitorPanel.bounds];
    self.programMonitorView.autoresizingMask =
        NSViewWidthSizable | NSViewHeightSizable;
    self.programMonitorView.eventTarget = self;
    self.programMonitorView.resizeTarget = self;
    [self.programMonitorPanel addSubview:self.programMonitorView];
    [self.monitorSplitView addArrangedSubview:self.sourceMonitorPanel];
    [self.monitorSplitView addArrangedSubview:self.programMonitorPanel];
    [self.monitorSplitView setHoldingPriority:NSLayoutPriorityDefaultLow
                            forSubviewAtIndex:0];
    [self.monitorSplitView setHoldingPriority:NSLayoutPriorityDefaultLow
                            forSubviewAtIndex:1];
    [self.monitorSplitView setPosition:editorWidth * 0.5 ofDividerAtIndex:0];
    [self.editorSplitView addArrangedSubview:self.monitorSplitView];
    [self.editorSplitView setHoldingPriority:NSLayoutPriorityDefaultLow
                           forSubviewAtIndex:0];
    [self.editorSplitView setHoldingPriority:NSLayoutPriorityDefaultLow
                           forSubviewAtIndex:1];
    [self.editorSplitView setPosition:workspaceHeight * 0.56
                     ofDividerAtIndex:0];
    [self.workspaceSplitView addArrangedSubview:self.editorSplitView];

    // F2.1 design system: fixed right dock, tabs = FixedPanelLayout()'s
    // PanelDock::Right slots (Inspector, Chat), in that fixed order. F2.2
    // and F2.4 will each call -setContentView:forSlot: once at startup to
    // replace their placeholder with real content; nothing else about this
    // dock's shape changes at runtime.
    self.rightDockPanel = [[CMPanelHostView alloc]
        initWithFrame:NSMakeRect(0.0, 0.0, rightDockWidth, workspaceHeight)
                 dock:ui::PanelDock::Right];
    self.rightDockPanel.autoresizingMask =
        NSViewWidthSizable | NSViewHeightSizable;
    [self.workspaceSplitView addArrangedSubview:self.rightDockPanel];

    // F2.2 -- Inspector: clip properties + the eight F1.3 color.* grading
    // knobs (ColorEffects.h), sliders built on CMControlRowView. Every
    // slider edit becomes a SetClipEffectsOperation, delivered here via
    // -inspectorView:didCommitClipEffects: (below) and applied through
    // self.state->editLog -- the same EditLog::Apply path CLI/MCP already
    // use for this operation (PHILOSOPHY.md principle 3, "aucune surface
    // n'est privilégiée"). See InspectorView.h/.mm for the view itself.
    self.inspectorView = [[CMInspectorView alloc]
        initWithFrame:NSMakeRect(0.0, 0.0, rightDockWidth, workspaceHeight)];
    self.inspectorView.delegate = self;
    self.inspectorView.autoresizingMask =
        NSViewWidthSizable | NSViewHeightSizable;
    [self.rightDockPanel setContentView:self.inspectorView
                                forSlot:ui::PanelSlot::Inspector];

    [self.workspaceSplitView setHoldingPriority:NSLayoutPriorityDefaultHigh
                              forSubviewAtIndex:0];
    [self.workspaceSplitView setHoldingPriority:NSLayoutPriorityDefaultHigh
                              forSubviewAtIndex:2];
    [self.workspaceSplitView setPosition:mediaPanelWidth ofDividerAtIndex:0];
    [self.workspaceSplitView setPosition:mediaPanelWidth + editorWidth
                        ofDividerAtIndex:1];

    // F2.5 design system: fixed bottom dock (Transport panel), full window
    // width, pinned just above the status bar -- see PanelHostView.h for why
    // this is a fixed slot rather than a user-rearrangeable one.
    // NSViewWidthSizable only (no height/min-Y flex): stays a fixed height
    // at a fixed distance from the window's bottom edge as the window
    // resizes, the same way mediaPanel/rightDockPanel stay pinned to their
    // edges via workspaceSplitView.
    self.bottomDockPanel = [[CMPanelHostView alloc]
        initWithFrame:NSMakeRect(0.0, 42.0, windowRect.size.width,
                                 bottomDockHeight)
                 dock:ui::PanelDock::Bottom];
    self.bottomDockPanel.autoresizingMask = NSViewWidthSizable;
    [content addSubview:self.bottomDockPanel];

    self.transportView = [[CMTransportView alloc]
        initWithFrame:NSMakeRect(0.0, 0.0, windowRect.size.width,
                                 bottomDockHeight -
                                     ui::theme::kTabStripHeight)];
    [self.bottomDockPanel setContentView:self.transportView
                                  forSlot:ui::PanelSlot::Transport];
    // Every position change still funnels through -requestTimelinePosition:
    // (quantized via TimelineView.h's QuantizePlayheadPosition, same as the
    // main timeline's own scrub/step/edit paths) -- these actions never
    // touch self.state->requestedPosition directly. See
    // -stepPlayheadFrames:/-transportScrubChanged:.
    self.transportView.playPauseButton.target = self;
    self.transportView.playPauseButton.action = @selector(menuPlayPause:);
    self.transportView.stepBackButton.target = self;
    self.transportView.stepBackButton.action = @selector(transportStepBack:);
    self.transportView.stepForwardButton.target = self;
    self.transportView.stepForwardButton.action =
        @selector(transportStepForward:);
    self.transportView.scrubSlider.target = self;
    self.transportView.scrubSlider.action = @selector(transportScrubChanged:);

    self.infoLabel = [NSTextField labelWithString:@"Aucun clip sélectionné"];
    self.infoLabel.frame =
        NSMakeRect(20.0, 12.0, windowRect.size.width - 225.0, 18.0);
    self.infoLabel.autoresizingMask = NSViewWidthSizable;
    self.infoLabel.font =
        [NSFont monospacedDigitSystemFontOfSize:12.0
                                         weight:NSFontWeightRegular];
    self.infoLabel.textColor = NSColor.secondaryLabelColor;
    self.infoLabel.lineBreakMode = NSLineBreakByTruncatingTail;
    [content addSubview:self.infoLabel];

    self.linkedSelectionButton =
        [NSButton buttonWithTitle:@"Sélection liée : ON"
                           target:self
                           action:@selector(linkedSelectionPressed:)];
    self.linkedSelectionButton.frame =
        NSMakeRect(windowRect.size.width - 180.0, 7.0, 160.0, 28.0);
    self.linkedSelectionButton.autoresizingMask = NSViewMinXMargin;
    self.linkedSelectionButton.bezelStyle = NSBezelStyleRounded;
    self.linkedSelectionButton.controlSize = NSControlSizeSmall;
    self.linkedSelectionButton.image =
        SystemSymbol(@"link", @"Sélection liée", 12.0);
    self.linkedSelectionButton.imagePosition = NSImageLeading;
    [self.linkedSelectionButton setButtonType:NSButtonTypePushOnPushOff];
    self.linkedSelectionButton.state = NSControlStateValueOn;
    self.linkedSelectionButton.toolTip =
        @"Sélectionner ensemble l’image et son audio associé";
    [content addSubview:self.linkedSelectionButton];
    [self refreshBinControlsSelecting:@"__all__"];

    self.state->renderer = std::make_unique<Renderer>();
    if (!self.state->renderer->Initialize(self.metalView)) {
        std::fprintf(stderr, "Renderer initialization failed\n");
        [NSApp terminate:nil];
        return;
    }
    self.state->sourceRenderer = std::make_unique<Renderer>();
    self.state->programRenderer = std::make_unique<Renderer>();
    if (!self.state->sourceRenderer->Initialize(self.sourceMonitorView) ||
        !self.state->programRenderer->Initialize(self.programMonitorView)) {
        std::fprintf(stderr, "Monitor renderer initialization failed\n");
        [NSApp terminate:nil];
        return;
    }
    self.offlineMediaLabel = [NSTextField labelWithString:@"MÉDIA OFFLINE"];
    self.offlineMediaLabel.alignment = NSTextAlignmentCenter;
    self.offlineMediaLabel.font =
        [NSFont systemFontOfSize:24.0 weight:NSFontWeightSemibold];
    self.offlineMediaLabel.textColor = NSColor.secondaryLabelColor;
    self.offlineMediaLabel.hidden = YES;
    [self.programMonitorView addSubview:self.offlineMediaLabel];
    self.offlineMediaLabel.frame =
        NSMakeRect(0.0, 0.0, self.programMonitorView.bounds.size.width,
                   self.programMonitorView.bounds.size.height);
    self.offlineMediaLabel.autoresizingMask =
        NSViewWidthSizable | NSViewHeightSizable;
    self.sourceOfflineMediaLabel =
        [NSTextField labelWithString:@"MÉDIA OFFLINE"];
    self.sourceOfflineMediaLabel.alignment = NSTextAlignmentCenter;
    self.sourceOfflineMediaLabel.font =
        [NSFont systemFontOfSize:24.0 weight:NSFontWeightSemibold];
    self.sourceOfflineMediaLabel.textColor = NSColor.secondaryLabelColor;
    self.sourceOfflineMediaLabel.hidden = YES;
    self.sourceOfflineMediaLabel.frame = self.sourceMonitorView.bounds;
    self.sourceOfflineMediaLabel.autoresizingMask =
        NSViewWidthSizable | NSViewHeightSizable;
    [self.sourceMonitorView addSubview:self.sourceOfflineMediaLabel];

    self.sourceMonitorTitleLabel =
        [NSTextField labelWithString:@"SOURCE — aucun rush chargé"];
    self.programMonitorTitleLabel = [NSTextField
        labelWithString:[NSString stringWithFormat:@"RECORD — %s",
                                                   self.state->document.sequence
                                                       .name.c_str()]];
    for (NSTextField* label in
         @[ self.sourceMonitorTitleLabel, self.programMonitorTitleLabel ]) {
        label.font = [NSFont systemFontOfSize:11.0 weight:NSFontWeightSemibold];
        label.textColor = [NSColor colorWithWhite:0.82 alpha:1.0];
        label.frame = NSMakeRect(10.0, 8.0, 360.0, 18.0);
        label.autoresizingMask = NSViewWidthSizable | NSViewMaxYMargin;
    }
    [self.sourceMonitorView addSubview:self.sourceMonitorTitleLabel];
    [self.programMonitorView addSubview:self.programMonitorTitleLabel];
    self.sourceMonitorZoneLabel =
        [NSTextField labelWithString:@"IN —    OUT —"];
    self.sourceMonitorZoneLabel.font =
        [NSFont monospacedDigitSystemFontOfSize:10.0 weight:NSFontWeightMedium];
    self.sourceMonitorZoneLabel.textColor = [NSColor colorWithRed:0.35
                                                            green:0.90
                                                             blue:0.62
                                                            alpha:1.0];
    self.sourceMonitorZoneLabel.frame = NSMakeRect(10.0, 29.0, 300.0, 16.0);
    self.sourceMonitorZoneLabel.autoresizingMask =
        NSViewWidthSizable | NSViewMaxYMargin;
    [self.sourceMonitorView addSubview:self.sourceMonitorZoneLabel];
    NSButton* sourceInsertButton =
        [NSButton buttonWithTitle:@"Insérer"
                           target:self
                           action:@selector(menuInsertSource:)];
    NSButton* sourceOverwriteButton =
        [NSButton buttonWithTitle:@"Écraser"
                           target:self
                           action:@selector(menuOverwriteSource:)];
    sourceInsertButton.frame = NSMakeRect(10.0, 49.0, 78.0, 24.0);
    sourceOverwriteButton.frame = NSMakeRect(92.0, 49.0, 82.0, 24.0);
    sourceInsertButton.image =
        SystemSymbol(@"arrow.down.to.line", @"Insérer depuis Source", 10.0);
    sourceOverwriteButton.image =
        SystemSymbol(@"square.on.square", @"Écraser depuis Source", 10.0);
    for (NSButton* button in @[ sourceInsertButton, sourceOverwriteButton ]) {
        button.bezelStyle = NSBezelStyleRounded;
        button.controlSize = NSControlSizeSmall;
        button.font = [NSFont systemFontOfSize:10.0];
        button.imagePosition = NSImageLeading;
        button.autoresizingMask = NSViewMaxYMargin;
        [self.sourceMonitorView addSubview:button];
    }
    self.sourceMonitorZoomPopup = [[NSPopUpButton alloc]
        initWithFrame:NSMakeRect(
                          self.sourceMonitorView.bounds.size.width - 92.0, 4.0,
                          84.0, 24.0)
            pullsDown:NO];
    self.programMonitorZoomPopup = [[NSPopUpButton alloc]
        initWithFrame:NSMakeRect(
                          self.programMonitorView.bounds.size.width - 92.0, 4.0,
                          84.0, 24.0)
            pullsDown:NO];
    for (NSPopUpButton* popup in
         @[ self.sourceMonitorZoomPopup, self.programMonitorZoomPopup ]) {
        [popup
            addItemsWithTitles:@[ @"Fit", @"25%", @"50%", @"100%", @"200%" ]];
        const NSInteger zoomTags[] = {0, 25, 50, 100, 200};
        for (NSInteger index = 0; index < popup.numberOfItems; ++index)
            [popup itemAtIndex:index].tag = zoomTags[index];
        popup.target = self;
        popup.action = @selector(monitorZoomChanged:);
        popup.controlSize = NSControlSizeSmall;
        popup.font =
            [NSFont monospacedDigitSystemFontOfSize:10.0
                                             weight:NSFontWeightMedium];
        popup.autoresizingMask = NSViewMinXMargin | NSViewMaxYMargin;
        popup.toolTip = @"Zoom d’affichage du moniteur";
    }
    [self.sourceMonitorView addSubview:self.sourceMonitorZoomPopup];
    [self.programMonitorView addSubview:self.programMonitorZoomPopup];
    self.sourceMonitorToggleButton =
        [NSButton buttonWithTitle:@"Source : ON"
                           target:self
                           action:@selector(toggleSourceMonitor:)];
    self.sourceMonitorToggleButton.frame = NSMakeRect(
        self.programMonitorView.bounds.size.width - 198.0, 4.0, 98.0, 24.0);
    self.sourceMonitorToggleButton.autoresizingMask =
        NSViewMinXMargin | NSViewMaxYMargin;
    self.sourceMonitorToggleButton.bezelStyle = NSBezelStyleRounded;
    self.sourceMonitorToggleButton.controlSize = NSControlSizeSmall;
    self.sourceMonitorToggleButton.font =
        [NSFont systemFontOfSize:10.0 weight:NSFontWeightMedium];
    self.sourceMonitorToggleButton.image = SystemSymbol(
        @"rectangle.split.2x1", @"Afficher le moniteur Source", 11.0);
    self.sourceMonitorToggleButton.imagePosition = NSImageLeading;
    self.sourceMonitorToggleButton.toolTip =
        @"Afficher ou masquer le moniteur Source";
    [self.programMonitorView addSubview:self.sourceMonitorToggleButton];
    self.trackHeaderLabels = [NSMutableArray array];
    self.timelineTimecodeLabel = [NSTextField labelWithString:@"00:00:00:00"];
    self.timelineTimecodeLabel.font =
        [NSFont monospacedDigitSystemFontOfSize:14.0
                                         weight:NSFontWeightSemibold];
    self.timelineTimecodeLabel.textColor = [NSColor colorWithRed:0.82
                                                           green:0.84
                                                            blue:0.87
                                                           alpha:1.0];
    self.timelineTimecodeLabel.alignment = NSTextAlignmentLeft;
    [self.metalView addSubview:self.timelineTimecodeLabel];
    self.timelineToolIcons = [NSMutableArray array];
    NSArray<NSString*>* toolSymbols = @[
        @"cursorarrow", @"hand.raised", @"magnifyingglass", @"scissors",
        @"arrow.left.and.right.square"
    ];
    NSArray<NSString*>* toolDescriptions =
        @[ @"Sélection", @"Main", @"Zoom", @"Lame", @"Slip (Y)" ];
    for (NSInteger index = 0; index < toolSymbols.count; ++index) {
        NSImageView* icon = [[NSImageView alloc]
            initWithFrame:NSMakeRect(110.0 + index * 17.0,
                                     self.metalView.bounds.size.height - 23.0,
                                     13.0, 13.0)];
        icon.image =
            SystemSymbol(toolSymbols[index], toolDescriptions[index], 11.0);
        icon.imageScaling = NSImageScaleProportionallyUpOrDown;
        icon.contentTintColor = index == 0 ? CMAccentBlueColor()
                                           : NSColor.secondaryLabelColor;
        icon.autoresizingMask = NSViewMinYMargin;
        icon.toolTip = toolDescriptions[index];
        [self.metalView addSubview:icon];
        [self.timelineToolIcons addObject:icon];
    }
    [self refreshTimelineChrome];
    self.state->viewport.FitDuration(self.state->duration,
                                     self.metalView.bounds.size.width);

    [self.window center];
    [self.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    for (auto& worker : self.state->workers) {
        worker.second->Start();
    }
    for (const DocumentSource& source : self.state->document.sources) {
        const auto detected = self.state->mediaMetadata.find(source.id);
        const LibraryMedia* media =
            detected == self.state->mediaMetadata.end()
                ? self.state->document.FindLibraryMedia(source.id)
                : &detected->second;
        if (media && media->has_audio &&
            self.state->offlineSourceIds.count(source.id) == 0)
            [self loadOrEnqueueWaveformForMediaIdentifier:
                      [NSString stringWithUTF8String:source.id.c_str()]];
        if (media && self.state->offlineSourceIds.count(source.id) == 0)
            [self loadOrEnqueueThumbnailForMediaIdentifier:
                      [NSString stringWithUTF8String:source.id.c_str()]];
    }
    [self requestResolvedPosition:{0, 1}];
    self.displayTimer = [NSTimer timerWithTimeInterval:(1.0 / 60.0)
                                                target:self
                                              selector:@selector(displayTick:)
                                              userInfo:nil
                                               repeats:YES];
    // Splitter dragging runs the AppKit event-tracking loop. Common modes keep
    // Metal presenting correctly letterboxed frames throughout the gesture.
    [NSRunLoop.mainRunLoop addTimer:self.displayTimer
                            forMode:NSRunLoopCommonModes];
}

- (void)application:(NSApplication*)application
          openFiles:(NSArray<NSString*>*)filenames {
    (void)application;
    if (filenames.count == 0) return;
    NSString* selected = [self resolvedProjectPath:filenames.firstObject];
    if (!self.window && self.documentPath.length == 0) {
        self.documentPath = selected;
        [NSApp replyToOpenOrPrint:NSApplicationDelegateReplySuccess];
        return;
    }
    [NSApp replyToOpenOrPrint:NSApplicationDelegateReplyCancel];
    NSAlert* alert = [NSAlert new];
    alert.messageText = @"Un projet est déjà ouvert";
    alert.informativeText =
        @"Fermez la fenêtre actuelle avant d’ouvrir un autre projet.";
    if (self.window)
        [alert beginSheetModalForWindow:self.window completionHandler:nil];
}

- (void)requestResolvedPosition:(RationalTime)position {
    self.state->requestedPosition = position;
    self.timelineTimecodeLabel.stringValue =
        TimelineTimecode(position, [self playheadFrameRate]);
    // F2.5: single choke point for every playhead move (scrub, playback
    // tick, arrow keys, edits all funnel through here already) -- the
    // Transport panel reads the same requestedPosition/duration this label
    // does, never a second notion of "now".
    [self.transportView setPosition:position
                            duration:self.state->duration
                                rate:[self playheadFrameRate]];
    self.state->requested.clear();
    for (const Ulid& trackId : self.state->videoTrackIds) {
        for (const ResolvedLayer& layer :
             self.state->timeline->ResolveTrackLayers(trackId, position)) {
            self.state->requested.push_back(
                {true, layer.frame.source_id, layer.frame.source_frame,
                 layer.opacity, layer.frame.clip_id});
            const auto worker = self.state->workers.find(layer.frame.source_id);
            if (worker != self.state->workers.end())
                worker->second->RequestFrame(layer.frame.source_frame);
        }
    }
    const auto topmost = std::find_if(
        self.state->requested.rbegin(), self.state->requested.rend(),
        [](const ResolvedSlot& slot) { return slot.active; });
    self.offlineMediaLabel.hidden =
        topmost == self.state->requested.rend() ||
        self.state->offlineSourceIds.count(topmost->sourceId) == 0;
}

- (void)requestSourcePosition:(RationalTime)position {
    const DocumentSource* source =
        self.state->document.FindSource(self.state->sourceMonitorId);
    if (!source) return;
    if (position < RationalTime{0, source->duration.rate})
        position = {0, source->duration.rate};
    if (position > source->duration) position = source->duration;
    const int64_t totalFrames =
        source->duration.to_frames(source->rate.num, source->rate.den);
    const int64_t frame = std::clamp<int64_t>(
        position.to_frames(source->rate.num, source->rate.den), 0,
        std::max<int64_t>(0, totalFrames - 1));
    position = {frame * static_cast<int64_t>(source->rate.den),
                source->rate.num};
    if (position > source->duration) position = source->duration;
    self.state->sourceMonitorPosition = position;
    const auto worker = self.state->workers.find(source->id);
    if (worker != self.state->workers.end())
        worker->second->RequestFrame(frame);
    self.state->overlayDirty = true;
}

- (void)updateSourceZoneLabel {
    const DocumentSource* source =
        self.state->document.FindSource(self.state->sourceMonitorId);
    const MediaRate rate = source ? source->rate : [self playheadFrameRate];
    NSString* inText = self.state->sourceIn
                           ? TimelineTimecode(*self.state->sourceIn, rate)
                           : @"—";
    NSString* outText = self.state->sourceOut
                            ? TimelineTimecode(*self.state->sourceOut, rate)
                            : @"—";
    self.sourceMonitorZoneLabel.stringValue =
        [NSString stringWithFormat:@"IN %@    OUT %@", inText, outText];
}

- (void)scrubSourceMonitorEvent:(NSEvent*)event {
    const DocumentSource* source =
        self.state->document.FindSource(self.state->sourceMonitorId);
    if (!source || self.sourceMonitorView.bounds.size.width <= 0.0) return;
    const NSPoint point =
        [self.sourceMonitorView convertPoint:event.locationInWindow
                                    fromView:nil];
    const long double fraction = std::clamp<long double>(
        point.x / self.sourceMonitorView.bounds.size.width, 0.0L, 1.0L);
    const int64_t frames =
        source->duration.to_frames(source->rate.num, source->rate.den);
    [self requestSourcePosition:{static_cast<int64_t>(
                                     std::llround(fraction * frames)) *
                                     static_cast<int64_t>(source->rate.den),
                                 source->rate.num}];
}

- (void)refreshTimelineChrome {
    if (!self.metalView || !self.state) return;
    for (NSTextField* label in self.trackHeaderLabels)
        [label removeFromSuperview];
    [self.trackHeaderLabels removeAllObjects];

    const double timelineHeight = [self timelineHeight];
    if (self.offlineMediaLabel)
        self.offlineMediaLabel.frame =
            NSMakeRect(0.0, 0.0, self.programMonitorView.bounds.size.width,
                       self.programMonitorView.bounds.size.height);
    self.timelineTimecodeLabel.frame = NSMakeRect(
        10.0, timelineHeight - kTimelineRulerHeight + 4.0,
        self.state->viewport.header_width - 20.0, kTimelineRulerHeight - 7.0);
    self.timelineTimecodeLabel.stringValue = TimelineTimecode(
        self.state->requestedPosition, [self playheadFrameRate]);

    const auto tracks = TimelineTracksInDisplayOrder(self.state->document);
    uint32_t videoNumber = static_cast<uint32_t>(std::count_if(
        tracks.begin(), tracks.end(),
        [](const DocumentTrack* track) { return track->kind == "video"; }));
    uint32_t audioNumber = 0;
    for (size_t index = 0; index < tracks.size(); ++index) {
        const bool video = tracks[index]->kind == "video";
        const uint32_t number = video ? videoNumber-- : ++audioNumber;
        NSString* title =
            [NSString stringWithFormat:@"%c%u", video ? 'V' : 'A', number];
        NSTextField* label = [NSTextField labelWithString:title];
        label.font = [NSFont monospacedDigitSystemFontOfSize:11.0
                                                      weight:NSFontWeightBold];
        label.alignment = NSTextAlignmentCenter;
        label.textColor =
            video ? [NSColor colorWithRed:0.47 green:0.76 blue:0.95 alpha:1.0]
                  : [NSColor colorWithRed:0.47 green:0.82 blue:0.61 alpha:1.0];
        label.drawsBackground = YES;
        label.backgroundColor = [NSColor colorWithWhite:0.075 alpha:0.96];
        label.wantsLayer = YES;
        label.layer.cornerRadius = 2.0;
        label.layer.borderWidth = 1.0;
        label.layer.borderColor =
            (video ? [NSColor colorWithRed:0.18 green:0.44 blue:0.61 alpha:1.0]
                   : [NSColor colorWithRed:0.18 green:0.48 blue:0.31 alpha:1.0])
                .CGColor;
        const double y = timelineHeight - kTimelineRulerHeight -
                         (index + 1) * self.state->viewport.track_height + 7.0;
        if (y + 20.0 < 0.0) continue;
        label.frame = NSMakeRect(9.0, y, 34.0, 20.0);
        [self.metalView addSubview:label];
        [self.trackHeaderLabels addObject:label];
    }
}

- (void)refreshBinControlsSelecting:(NSString*)selectedBinId {
    if (!self.binOutline || !self.mediaTable) return;
    NSString* requested = selectedBinId ?: self.selectedBinId ?: @"__all__";
    self.selectedBinId = requested;
    [self.binOutline reloadData];
    [self.binOutline expandItem:nil expandChildren:YES];
    const NSInteger row = [self.binOutline rowForItem:requested];
    if (row >= 0)
        [self.binOutline selectRowIndexes:[NSIndexSet indexSetWithIndex:row]
                     byExtendingSelection:NO];
    [self rebuildMediaList];
}

- (void)binSelectionChanged:(id)sender {
    (void)sender;
    const NSInteger row = self.binOutline.selectedRow;
    if (row >= 0) self.selectedBinId = [self.binOutline itemAtRow:row];
    [self rebuildMediaList];
}

- (NSArray<NSString*>*)childBinIds:(NSString*)parentId {
    NSMutableArray<NSString*>* result = [NSMutableArray array];
    const std::string parent(parentId.UTF8String ?: "");
    std::vector<const DocumentBin*> bins;
    for (const DocumentBin& bin : self.state->document.bins)
        if (bin.parent_id == parent) bins.push_back(&bin);
    std::stable_sort(bins.begin(), bins.end(),
                     [](const DocumentBin* left, const DocumentBin* right) {
                         return left->name < right->name;
                     });
    for (const DocumentBin* bin : bins)
        [result addObject:[NSString stringWithUTF8String:bin->id.c_str()]];
    return result;
}

- (NSInteger)outlineView:(NSOutlineView*)outlineView
    numberOfChildrenOfItem:(id)item {
    (void)outlineView;
    if (!item) return 2 + [self childBinIds:@""].count;
    NSString* identifier = item;
    if ([identifier hasPrefix:@"__"]) return 0;
    return [self childBinIds:identifier].count;
}

- (id)outlineView:(NSOutlineView*)outlineView
            child:(NSInteger)index
           ofItem:(id)item {
    (void)outlineView;
    if (!item) {
        if (index == 0) return @"__all__";
        if (index == 1) return @"__root__";
        return [self childBinIds:@""][index - 2];
    }
    return [self childBinIds:item][index];
}

- (BOOL)outlineView:(NSOutlineView*)outlineView isItemExpandable:(id)item {
    (void)outlineView;
    NSString* identifier = item;
    return ![identifier hasPrefix:@"__"] &&
           [self childBinIds:identifier].count > 0;
}

- (id<NSPasteboardWriting>)outlineView:(NSOutlineView*)outlineView
               pasteboardWriterForItem:(id)item {
    (void)outlineView;
    NSString* identifier = item;
    if ([identifier hasPrefix:@"__"] ||
        !self.state->document.FindBin(identifier.UTF8String ?: ""))
        return nil;
    NSPasteboardItem* pasteboardItem = [[NSPasteboardItem alloc] init];
    [pasteboardItem setString:identifier forType:kCutmachineBinPasteboardType];
    return pasteboardItem;
}

- (NSString*)dropTargetBinForItem:(id)item {
    NSString* identifier = [item isKindOfClass:NSString.class] ? item : nil;
    if (!identifier || [identifier isEqualToString:@"__all__"] ||
        [identifier isEqualToString:@"__root__"])
        return @"";
    return self.state->document.FindBin(identifier.UTF8String ?: "")
               ? identifier
               : nil;
}

- (BOOL)bin:(NSString*)binId canMoveInto:(NSString*)parentId {
    if ([binId isEqualToString:parentId]) return NO;
    const DocumentBin* cursor =
        self.state->document.FindBin(parentId.UTF8String ?: "");
    while (cursor) {
        if (cursor->id == (binId.UTF8String ?: "")) return NO;
        cursor = self.state->document.FindBin(cursor->parent_id);
    }
    return YES;
}

- (NSDragOperation)outlineView:(NSOutlineView*)outlineView
                  validateDrop:(id<NSDraggingInfo>)info
                  proposedItem:(id)item
            proposedChildIndex:(NSInteger)index {
    NSString* target = [self dropTargetBinForItem:item];
    if (!target) return NSDragOperationNone;
    [outlineView setDropItem:item dropChildIndex:NSOutlineViewDropOnItemIndex];
    NSPasteboard* pasteboard = info.draggingPasteboard;
    NSString* movingBin =
        [pasteboard stringForType:kCutmachineBinPasteboardType];
    if (movingBin)
        return [self bin:movingBin canMoveInto:target] ? NSDragOperationMove
                                                       : NSDragOperationNone;
    if ([pasteboard stringForType:kCutmachineMediaPasteboardType])
        return NSDragOperationMove;
    if ([pasteboard availableTypeFromArray:@[ NSPasteboardTypeFileURL ]])
        return NSDragOperationCopy;
    (void)index;
    return NSDragOperationNone;
}

- (BOOL)outlineView:(NSOutlineView*)outlineView
         acceptDrop:(id<NSDraggingInfo>)info
               item:(id)item
         childIndex:(NSInteger)index {
    (void)outlineView;
    (void)index;
    NSString* target = [self dropTargetBinForItem:item];
    if (!target) return NO;
    NSPasteboard* pasteboard = info.draggingPasteboard;
    NSArray<NSURL*>* urls = [pasteboard
        readObjectsForClasses:@[ NSURL.class ]
                      options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
    if (urls.count > 0) return [self importMediaURLs:urls intoBin:target];

    NSString* media = [pasteboard stringForType:kCutmachineMediaPasteboardType];
    NSString* movingBin =
        [pasteboard stringForType:kCutmachineBinPasteboardType];
    Operation operation;
    if (media)
        operation = SetMediaBinOperation{media.UTF8String ?: "",
                                         target.UTF8String ?: ""};
    else if (movingBin && [self bin:movingBin canMoveInto:target])
        operation = MoveBinOperation{movingBin.UTF8String ?: "",
                                     target.UTF8String ?: ""};
    else
        return NO;

    EditError error = EditError::None;
    std::string message;
    if (!self.state->editLog.Apply(self.state->document, std::move(operation),
                                   error, message)) {
        self.binSummaryLabel.stringValue = [NSString
            stringWithFormat:@"Déplacement refusé : %s", message.c_str()];
        return NO;
    }
    if (![self persistEdits:message]) {
        self.binSummaryLabel.stringValue =
            [NSString stringWithFormat:@"Enregistrement impossible : %s",
                                       message.c_str()];
        return NO;
    }
    NSString* selected = target.length == 0 ? @"__root__" : target;
    [self refreshBinControlsSelecting:selected];
    if (target.length > 0) [self.binOutline expandItem:target];
    return YES;
}

- (NSView*)outlineView:(NSOutlineView*)outlineView
    viewForTableColumn:(NSTableColumn*)tableColumn
                  item:(id)item {
    (void)outlineView;
    (void)tableColumn;
    NSTextField* label = [NSTextField labelWithString:@""];
    NSString* identifier = item;
    if ([identifier isEqualToString:@"__all__"])
        label.stringValue = @"▣ Tous les objets";
    else if ([identifier isEqualToString:@"__root__"])
        label.stringValue = @"⌂ Sans chutier";
    else {
        const DocumentBin* bin =
            self.state->document.FindBin(identifier.UTF8String ?: "");
        label.stringValue =
            bin ? [NSString stringWithUTF8String:bin->name.c_str()]
                : @"Chutier manquant";
        if (bin) {
            label.editable = YES;
            label.selectable = YES;
            label.delegate = self;
            label.identifier = [@"bin:" stringByAppendingString:identifier];
        }
    }
    label.lineBreakMode = NSLineBreakByTruncatingTail;
    return label;
}

- (void)outlineViewSelectionDidChange:(NSNotification*)notification {
    if (notification.object == self.binOutline)
        [self binSelectionChanged:self.binOutline];
}

- (void)rebuildMediaList {
    self.visibleMediaIds = [NSMutableArray array];
    NSString* query = self.mediaSearchField.stringValue.lowercaseString ?: @"";
    const std::string selected(self.selectedBinId.UTF8String ?: "__all__");
    for (const DocumentSequence& sequence : self.state->project.timelines) {
        const auto placement =
            self.state->project.timeline_bin_ids.find(sequence.id);
        const Ulid binId =
            placement == self.state->project.timeline_bin_ids.end()
                ? Ulid{}
                : placement->second;
        if (selected != "__all__" &&
            ((selected == "__root__" && !binId.empty()) ||
             (selected != "__root__" && binId != selected)))
            continue;
        NSString* sequenceName =
            [NSString stringWithUTF8String:sequence.name.c_str()];
        if (query.length == 0 ||
            [sequenceName.lowercaseString rangeOfString:query].location !=
                NSNotFound) {
            [self.visibleMediaIds
                addObject:[NSString stringWithUTF8String:sequence.id.c_str()]];
        }
    }
    std::vector<const LibraryMedia*> mediaItems;
    for (const LibraryMedia& media : self.state->document.library) {
        if (selected != "__all__" &&
            ((selected == "__root__" && !media.bin_id.empty()) ||
             (selected != "__root__" && media.bin_id != selected)))
            continue;
        NSString* searchable = [NSString
            stringWithFormat:@"%s %s %s %s %dx%d", media.filename.c_str(),
                             media.path.c_str(), media.codec.c_str(),
                             media.orientation.c_str(), media.width,
                             media.height];
        if (query.length > 0 &&
            [searchable.lowercaseString rangeOfString:query].location ==
                NSNotFound)
            continue;
        mediaItems.push_back(&media);
    }
    std::stable_sort(mediaItems.begin(), mediaItems.end(),
                     [](const LibraryMedia* left, const LibraryMedia* right) {
                         return left->filename < right->filename;
                     });
    for (const LibraryMedia* media : mediaItems)
        [self.visibleMediaIds
            addObject:[NSString stringWithUTF8String:media->id.c_str()]];
    [self.mediaTable reloadData];
    [self.mediaCollection reloadData];
    const NSUInteger count = self.visibleMediaIds.count;
    self.binSummaryLabel.stringValue = [NSString
        stringWithFormat:@"%lu objet%@ affiché%@", (unsigned long)count,
                         count == 1 ? @"" : @"s", count == 1 ? @"" : @"s"];
    self.assignMediaButton.enabled = YES;
}

- (void)mediaViewChanged:(id)sender {
    (void)sender;
    const BOOL icons = self.mediaViewToggle.selectedSegment == 1;
    self.mediaListScroll.hidden = icons;
    self.mediaIconScroll.hidden = !icons;
    [self rebuildMediaList];
}

- (NSString*)selectedMediaId {
    return [self selectedMediaIds].firstObject;
}

- (NSArray<NSString*>*)selectedMediaIds {
    NSMutableArray<NSString*>* identifiers = [NSMutableArray array];
    if (self.mediaViewToggle.selectedSegment == 1) {
        NSArray<NSIndexPath*>* paths =
            [self.mediaCollection.selectionIndexPaths.allObjects
                sortedArrayUsingComparator:^NSComparisonResult(
                    NSIndexPath* left, NSIndexPath* right) {
                  if (left.item < right.item) return NSOrderedAscending;
                  if (left.item > right.item) return NSOrderedDescending;
                  return NSOrderedSame;
                }];
        for (NSIndexPath* path in paths)
            if (path.item < self.visibleMediaIds.count)
                [identifiers addObject:self.visibleMediaIds[path.item]];
    } else {
        [self.mediaTable.selectedRowIndexes
            enumerateIndexesUsingBlock:^(NSUInteger row, BOOL* stop) {
              (void)stop;
              if (row < self.visibleMediaIds.count)
                  [identifiers addObject:self.visibleMediaIds[row]];
            }];
    }
    return identifiers;
}

- (NSInteger)collectionView:(NSCollectionView*)collectionView
     numberOfItemsInSection:(NSInteger)section {
    (void)collectionView;
    (void)section;
    return self.visibleMediaIds.count;
}

- (NSCollectionViewItem*)collectionView:(NSCollectionView*)collectionView
    itemForRepresentedObjectAtIndexPath:(NSIndexPath*)indexPath {
    MediaIconItem* item = [collectionView makeItemWithIdentifier:@"media-icon"
                                                    forIndexPath:indexPath];
    if (indexPath.item >= self.visibleMediaIds.count) return item;
    NSString* identifier = self.visibleMediaIds[indexPath.item];
    const DocumentSequence* timeline =
        self.state->project.FindTimeline(identifier.UTF8String ?: "");
    if (timeline) {
        const DocumentSequence& sequence = *timeline;
        item.textField.stringValue = [NSString
            stringWithFormat:@"%@%s",
                             sequence.id == self.state->activeTimelineId ? @"● "
                                                                         : @"",
                             sequence.name.c_str()];
        NSImage* symbol =
            [NSImage imageWithSystemSymbolName:@"timeline.selection"
                      accessibilityDescription:@"Séquence"];
        symbol.size = NSMakeSize(44.0, 44.0);
        item.imageView.image = symbol;
        item.view.toolTip = [NSString
            stringWithFormat:@"Séquence · %dx%d · %d/%d fps · %lu pistes",
                             sequence.width, sequence.height,
                             sequence.frame_rate.num, sequence.frame_rate.den,
                             (unsigned long)sequence.tracks.size()];
        return item;
    }
    const LibraryMedia* media =
        self.state->document.FindLibraryMedia(identifier.UTF8String ?: "");
    if (!media) return item;
    item.textField.stringValue = [NSString
        stringWithFormat:@"%@%s", media->proxy_path.empty() ? @"" : @"P · ",
                         media->filename.c_str()];
    const bool offline = self.state->offlineSourceIds.count(media->id) != 0;
    NSImage* thumbnail = offline ? nil : self.mediaThumbnails[identifier];
    if (thumbnail) {
        item.imageView.image = thumbnail;
    } else {
        NSImage* symbol = [NSImage
            imageWithSystemSymbolName:(offline ? @"exclamationmark.triangle"
                                               : @"film")
             accessibilityDescription:(offline ? @"Média offline"
                                               : @"Média vidéo")];
        symbol.size = NSMakeSize(44.0, 44.0);
        item.imageView.image = symbol;
    }
    item.view.toolTip =
        offline ? [NSString stringWithFormat:@"%s · MÉDIA OFFLINE",
                                             media->filename.c_str()]
                : [NSString stringWithFormat:@"%s · %s · %dx%d · %@",
                                             media->filename.c_str(),
                                             media->codec.c_str(), media->width,
                                             media->height,
                                             TimeString(media->duration)];
    if (!offline && !media->proxy_path.empty())
        item.view.toolTip =
            [item.view.toolTip stringByAppendingString:@" · PROXY disponible"];
    return item;
}

- (id<NSPasteboardWriting>)collectionView:(NSCollectionView*)collectionView
       pasteboardWriterForItemAtIndexPath:(NSIndexPath*)indexPath {
    (void)collectionView;
    if (indexPath.item >= self.visibleMediaIds.count) return nil;
    if (self.state->project.FindTimeline(
            self.visibleMediaIds[indexPath.item].UTF8String ?: ""))
        return nil;
    NSPasteboardItem* item = [[NSPasteboardItem alloc] init];
    [item setString:self.visibleMediaIds[indexPath.item]
            forType:kCutmachineMediaPasteboardType];
    return item;
}

- (BOOL)tableView:(NSTableView*)tableView
    writeRowsWithIndexes:(NSIndexSet*)rowIndexes
            toPasteboard:(NSPasteboard*)pasteboard {
    if (tableView != self.mediaTable || rowIndexes.count != 1) return NO;
    const NSUInteger row = rowIndexes.firstIndex;
    if (row >= self.visibleMediaIds.count) return NO;
    if (self.state->project.FindTimeline(self.visibleMediaIds[row].UTF8String
                                             ?: ""))
        return NO;
    [pasteboard declareTypes:@[ kCutmachineMediaPasteboardType ] owner:nil];
    return [pasteboard setString:self.visibleMediaIds[row]
                         forType:kCutmachineMediaPasteboardType];
}

- (void)openSelectedMediaInSourceMonitor:(id)sender {
    (void)sender;
    NSString* identifier = [self selectedMediaId];
    if (!identifier) {
        const DocumentClip* clip = self.state->document.FindClip(
            self.state->interaction->SelectedClipId());
        if (clip)
            identifier =
                [NSString stringWithUTF8String:clip->source_id.c_str()];
    }
    if (!identifier) return;
    if (self.state->project.FindTimeline(identifier.UTF8String ?: "")) {
        [self activateTimelineIdentifier:identifier];
        return;
    }
    [self openMediaIdentifierInSourceMonitor:identifier];
}

- (BOOL)activateTimelineIdentifier:(NSString*)identifier {
    const Ulid timelineId(identifier.UTF8String ?: "");
    if (!self.state->project.FindTimeline(timelineId)) return NO;
    [self setPlaybackDirection:0];
    self.state->sourceMonitor = false;
    self.state->sourceMonitorActive = false;
    self.state->sourceIn.reset();
    self.state->sourceOut.reset();
    if (timelineId == self.state->activeTimelineId) {
        [self requestResolvedPosition:self.state->requestedPosition];
        return YES;
    }

    const HistoryDomain historyBeforeNavigation = self.state->lastHistoryDomain;
    std::string message;
    if (![self persistEdits:message]) {
        self.infoLabel.stringValue = [NSString
            stringWithFormat:@"Changement de timeline impossible : %s",
                             message.c_str()];
        return NO;
    }
    self.state->lastHistoryDomain = historyBeforeNavigation;
    const Ulid previousId = self.state->activeTimelineId;
    self.state->timelinePositions[previousId] = self.state->requestedPosition;
    self.state->timelineEditLogs[previousId] = self.state->editLog;
    self.state->timelineTargetTracks[previousId] = self.state->targetedTrackIds;

    EditLog nextLog;
    const auto storedLog = self.state->timelineEditLogs.find(timelineId);
    if (storedLog != self.state->timelineEditLogs.end())
        nextLog = storedLog->second;
    self.state->activeTimelineId = timelineId;
    self.state->document = self.state->project.MakeDocument(timelineId);
    self.state->editLog = std::move(nextLog);
    self.state->timelineEditLogs[timelineId] = self.state->editLog;
    self.state->targetedTrackIds.clear();
    const auto storedTargets =
        self.state->timelineTargetTracks.find(timelineId);
    if (storedTargets != self.state->timelineTargetTracks.end()) {
        for (const Ulid& id : storedTargets->second)
            if (self.state->document.FindTrack(id))
                self.state->targetedTrackIds.insert(id);
    } else {
        for (const DocumentTrack& track : self.state->document.sequence.tracks)
            self.state->targetedTrackIds.insert(track.id);
    }
    self.state->timelineTargetTracks[timelineId] = self.state->targetedTrackIds;
    self.state->timeline = std::make_unique<Timeline>(self.state->document);
    self.state->interaction = std::make_unique<TimelineInteraction>(
        self.state->document, self.state->editLog, self.state->viewport);
    self.state->interaction->SetLinkedSelectionEnabled(
        self.state->linkedSelection);
    self.state->timelineIn.reset();
    self.state->timelineOut.reset();
    self.state->sourceRendered = {};
    self.state->rendered.clear();
    RationalTime position{0, self.state->document.sequence.frame_rate.num};
    const auto storedPosition = self.state->timelinePositions.find(timelineId);
    if (storedPosition != self.state->timelinePositions.end())
        position = storedPosition->second;
    self.programMonitorTitleLabel.stringValue =
        [NSString stringWithFormat:@"RECORD — %s",
                                   self.state->document.sequence.name.c_str()];
    [self refreshTimelineAfterEditFromPosition:position];
    [self refreshBinControlsSelecting:self.selectedBinId ?: @"__all__"];
    [self updateSelectionInfo];
    self.window.title =
        [NSString stringWithFormat:@"CUTMACHINE — %s — %s",
                                   self.state->project.name.c_str(),
                                   self.state->document.sequence.name.c_str()];
    self.infoLabel.stringValue =
        [NSString stringWithFormat:@"Timeline active : %s",
                                   self.state->document.sequence.name.c_str()];
    self.state->overlayDirty = true;
    return YES;
}

- (void)createTimelinePressed:(id)sender {
    (void)sender;
    NSAlert* alert = [NSAlert new];
    alert.messageText = @"Nouvelle timeline";
    alert.informativeText =
        @"La nouvelle timeline utilisera le format de la timeline active.";
    [alert addButtonWithTitle:@"Créer"];
    [alert addButtonWithTitle:@"Annuler"];
    NSTextField* nameField =
        [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 320, 24)];
    nameField.placeholderString = @"Nom de la timeline";
    nameField.stringValue = [NSString
        stringWithFormat:@"Timeline %lu",
                         (unsigned long)self.state->project.timelines.size() +
                             1];
    alert.accessoryView = nameField;
    if ([alert runModal] != NSAlertFirstButtonReturn) return;
    NSString* trimmed = [nameField.stringValue
        stringByTrimmingCharactersInSet:NSCharacterSet
                                            .whitespaceAndNewlineCharacterSet];
    if (trimmed.length == 0) return;

    std::string message;
    if (![self persistEdits:message]) return;
    Project candidate = self.state->project;
    ProjectEditLog projectLog = self.state->projectEditLog;
    const DocumentSequence& current = self.state->document.sequence;
    EditError editError = EditError::None;
    ProjectOperation operation = AddProjectTimelineOperation{
        trimmed.UTF8String ?: "New Timeline", current.width, current.height,
        current.frame_rate};
    if (!projectLog.Apply(candidate, std::move(operation), editError,
                          message)) {
        self.infoLabel.stringValue = [NSString
            stringWithFormat:@"Création refusée (%s) : %s",
                             EditErrorName(editError), message.c_str()];
        return;
    }
    const auto& applied = std::get<AddProjectTimelineOperation>(
        projectLog.AppliedEntries().back().op);
    const Ulid createdId = applied.timeline_id;
    if (![self commitProjectCandidate:candidate
                              editLog:self.state->editLog
                           projectLog:projectLog
                              message:message]) {
        self.infoLabel.stringValue = [NSString
            stringWithFormat:@"Création impossible : %s", message.c_str()];
        return;
    }
    self.state->project = std::move(candidate);
    self.state->projectEditLog = std::move(projectLog);
    self.state->lastHistoryDomain = HistoryDomain::Project;
    [self rebuildMediaList];
    [self
        activateTimelineIdentifier:[NSString
                                       stringWithUTF8String:createdId.c_str()]];
}

- (BOOL)validateMenuItem:(NSMenuItem*)item {
    const SEL action = item.action;
    if (action == @selector(exportFinalVideo:))
        return self.state && !self.state->exportRunning;
    if (action == @selector(menuUndo:))
        return self.state && (self.state->editLog.AppliedCount() > 0 ||
                              self.state->projectEditLog.AppliedCount() > 0);
    if (action == @selector(menuRedo:))
        return self.state && (self.state->editLog.UndoneCount() > 0 ||
                              self.state->projectEditLog.UndoneCount() > 0);
    if (action == @selector(menuCopyTimelineClips:))
        return self.state && self.state->interaction &&
               !self.state->interaction->SelectedClipIds().empty();
    if (action == @selector(menuPasteTimelineClips:))
        return self.state && !self.state->timelineClipboard.empty();
    if (action == @selector(menuAddCrossDissolve:))
        return self.state && self.state->interaction;
    if (action == @selector(menuRemoveCrossDissolve:))
        return self.state && !self.state->document.sequence.transitions.empty();
    if (action == @selector(menuInsertSource:) || action == @selector
                                                      (menuOverwriteSource:))
        return self.state && self.state->sourceMonitor &&
               self.state->document.FindSource(self.state->sourceMonitorId) !=
                   nullptr;
    if (action == @selector(menuToggleSnapping:)) {
        if (!self.state || !self.state->interaction) return NO;
        item.state = self.state->interaction->SnappingEnabled()
                         ? NSControlStateValueOn
                         : NSControlStateValueOff;
        return YES;
    }
    if (action == @selector(menuToggleLinkedSelection:)) {
        if (!self.state || !self.state->interaction) return NO;
        item.state = self.state->linkedSelection ? NSControlStateValueOn
                                                 : NSControlStateValueOff;
        return YES;
    }
    if (action == @selector(menuToggleProxies:)) {
        if (!self.state) return NO;
        item.state = self.state->proxiesEnabled ? NSControlStateValueOn
                                                : NSControlStateValueOff;
        return YES;
    }
    if (action == @selector(renameBinPressed:) || action == @selector
                                                      (deleteBinPressed:))
        return self.state &&
               self.state->document.FindBin(self.selectedBinId.UTF8String
                                                ?: "") != nullptr;
    if (action == @selector(assignMediaToBinPressed:))
        return self.state &&
               self.state->document.FindLibraryMedia(
                   [self selectedMediaId].UTF8String ?: "") != nullptr;
    if (action == @selector(relinkSelectedMedia:))
        return self.state && self.state->document.FindLibraryMedia(
                                 [self selectedMediaId].UTF8String ?: "");
    if (action == @selector(batchRelinkOfflineMedia:))
        return self.state && !self.state->offlineSourceIds.empty() &&
               self.state->pendingBatchRelinks.empty();
    if (action == @selector(generateSelectedMediaProxy:) ||
        action == @selector(regenerateSelectedMediaThumbnail:))
        return self.state && self.state->document.FindLibraryMedia(
                                 [self selectedMediaId].UTF8String ?: "");
    if (action == @selector(removeSelectedMediaProxy:)) {
        const LibraryMedia* media =
            self.state ? self.state->document.FindLibraryMedia(
                             [self selectedMediaId].UTF8String ?: "")
                       : nullptr;
        return media && !media->proxy_path.empty();
    }
    if (action == @selector(removeContextTrackPressed:))
        return self.state &&
               self.state->document.FindTrack(self.state->contextTrackId);
    if (action == @selector(removeEmptyTracksPressed:))
        return self.state &&
               std::any_of(self.state->document.sequence.tracks.begin(),
                           self.state->document.sequence.tracks.end(),
                           [](const DocumentTrack& track) {
                               return track.clips.empty();
                           });
    if (action == @selector(menuCutSelectedAtPlayhead:)) {
        if (!self.state || !self.state->interaction) return NO;
        const DocumentClip* clip = self.state->document.FindClip(
            self.state->interaction->SelectedClipId());
        return clip && self.state->requestedPosition > clip->timeline_in &&
               self.state->requestedPosition <
                   clip->timeline_in.add(clip->duration);
    }
    return YES;
}

- (void)openMediaIdentifierInSourceMonitor:(NSString*)identifier {
    const Ulid sourceId(identifier.UTF8String ?: "");
    const DocumentSource* source = self.state->document.FindSource(sourceId);
    const LibraryMedia* media = self.state->document.FindLibraryMedia(sourceId);
    const auto worker = self.state->workers.find(sourceId);
    if (!source || !media) {
        self.binSummaryLabel.stringValue =
            @"Ce média doit être réingéré pour devenir une source montable.";
        return;
    }
    [self setPlaybackDirection:0];
    if (self.state->sourceMonitorId != sourceId) {
        self.state->sourceIn.reset();
        self.state->sourceOut.reset();
    }
    self.state->sourceMonitor = true;
    self.state->sourceMonitorActive = true;
    self.state->sourceMonitorId = sourceId;
    self.state->sourceMonitorPosition = {0, source->duration.rate};
    self.state->sourceRendered = {};
    self.sourceMonitorTitleLabel.stringValue =
        [NSString stringWithFormat:@"SOURCE — %s", media->filename.c_str()];
    [self updateSourceZoneLabel];
    [self.window makeFirstResponder:self.sourceMonitorView];
    if (worker == self.state->workers.end()) {
        self.sourceOfflineMediaLabel.hidden = NO;
        self.state->overlayDirty = true;
        self.infoLabel.stringValue = [NSString
            stringWithFormat:@"MONITEUR SOURCE    %s    MÉDIA OFFLINE",
                             media->filename.c_str()];
        return;
    }
    self.sourceOfflineMediaLabel.hidden = YES;
    worker->second->RequestFrame(0);
    self.state->overlayDirty = true;
    self.infoLabel.stringValue = [NSString
        stringWithFormat:@"MONITEUR SOURCE    %s    durée %@    %d/%d fps",
                         media->filename.c_str(), TimeString(source->duration),
                         source->rate.num, source->rate.den];
}

- (void)monitorZoomChanged:(NSPopUpButton*)sender {
    const double zoom =
        sender.selectedItem.tag <= 0 ? 0.0 : sender.selectedItem.tag / 100.0;
    if (sender == self.sourceMonitorZoomPopup)
        self.state->sourceMonitorZoom = zoom;
    else if (sender == self.programMonitorZoomPopup)
        self.state->programMonitorZoom = zoom;
    else
        return;
    self.state->overlayDirty = true;
}

- (void)toggleSourceMonitor:(NSButton*)sender {
    if (self.state->sourceMonitorVisible) {
        self.state->sourceMonitorPanelWidth =
            self.sourceMonitorPanel.frame.size.width;
        self.state->sourceMonitorVisible = false;
        self.state->sourceMonitorActive = false;
        self.sourceMonitorPanel.hidden = YES;
        sender.title = @"Source : OFF";
        sender.image =
            SystemSymbol(@"rectangle", @"Moniteur Record seul", 11.0);
    } else {
        self.state->sourceMonitorVisible = true;
        self.sourceMonitorPanel.hidden = NO;
        const double available = self.monitorSplitView.bounds.size.width -
                                 self.monitorSplitView.dividerThickness;
        const double restored = self.state->sourceMonitorPanelWidth > 0.0
                                    ? self.state->sourceMonitorPanelWidth
                                    : available * 0.5;
        [self.monitorSplitView
                 setPosition:std::clamp(restored, 320.0,
                                        std::max(320.0, available - 320.0))
            ofDividerAtIndex:0];
        sender.title = @"Source : ON";
        sender.image = SystemSymbol(@"rectangle.split.2x1",
                                    @"Afficher le moniteur Source", 11.0);
    }
    [self.monitorSplitView adjustSubviews];
    self.state->overlayDirty = true;
}

- (void)revealSelectedMediaInFinder:(id)sender {
    (void)sender;
    NSString* identifier = [self selectedMediaId];
    if (!identifier) return;
    const LibraryMedia* media =
        self.state->document.FindLibraryMedia(identifier.UTF8String ?: "");
    if (!media) return;
    std::filesystem::path path(media->path);
    if (path.is_relative()) {
        path = std::filesystem::absolute(
                   std::filesystem::path(self.documentPath.UTF8String ?: ""))
                   .parent_path() /
               path;
    }
    NSURL* url = [NSURL
        fileURLWithPath:[NSString stringWithUTF8String:path.lexically_normal()
                                                           .c_str()]];
    [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:@[ url ]];
}

- (void)relinkSelectedMedia:(id)sender {
    (void)sender;
    NSString* identifier = [self selectedMediaId];
    if (!identifier) return;
    const Ulid mediaId(identifier.UTF8String ?: "");
    const LibraryMedia* existing =
        self.state->document.FindLibraryMedia(mediaId);
    if (!existing) return;
    for (const auto& pending : self.state->pendingRelinks) {
        if (pending.second.media_id == mediaId) {
            self.binSummaryLabel.stringValue =
                @"Une reconnexion est déjà en cours pour ce média.";
            return;
        }
    }
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.title = [NSString
        stringWithFormat:@"Reconnecter %s", existing->filename.c_str()];
    panel.prompt = @"Reconnecter";
    panel.allowsMultipleSelection = NO;
    panel.canChooseFiles = YES;
    panel.canChooseDirectories = NO;
    panel.resolvesAliases = YES;
    panel.allowedContentTypes = @[ UTTypeMovie ];
    if ([panel runModal] != NSModalResponseOK || !panel.URL) return;

    std::error_code pathError;
    const std::filesystem::path absolute = std::filesystem::weakly_canonical(
        std::filesystem::path(panel.URL.fileSystemRepresentation ?: ""),
        pathError);
    if (pathError) {
        self.binSummaryLabel.stringValue =
            [NSString stringWithFormat:@"Fichier inaccessible : %s",
                                       pathError.message().c_str()];
        return;
    }
    const std::filesystem::path projectPath = std::filesystem::absolute(
        std::filesystem::path(self.documentPath.UTF8String ?: ""));
    const std::filesystem::path base = projectPath.parent_path();
    std::string stored = std::filesystem::relative(absolute, base, pathError)
                             .lexically_normal()
                             .string();
    if (pathError) stored = absolute.string();
    auto result = std::make_shared<RelinkProbeResult>();
    result->media.id = mediaId;
    result->media.path = stored;
    result->media.filename = absolute.filename().string();
    const Document snapshot = self.state->document;
    const Ulid taskId = self.state->mediaTasks->Enqueue(
        MediaTaskKind::Relink, "Relink " + existing->filename,
        [absolute, mediaId, snapshot, result](MediaTaskContext& context,
                                              std::string& taskError) {
            if (context.Cancelled()) {
                taskError = "relink cancelled";
                return false;
            }
            context.SetProgress(0.1, "Analyse du remplacement");
            if (!ProbeMediaMetadata(absolute.string(), result->media,
                                    taskError)) {
                result->error = taskError;
                return false;
            }
            if (context.Cancelled()) {
                taskError = "relink cancelled";
                return false;
            }
            context.SetProgress(0.8, "Vérification de compatibilité");
            if (!ValidateRelinkCandidate(snapshot, mediaId, result->media,
                                         taskError)) {
                result->error = taskError;
                return false;
            }
            context.SetProgress(1.0, "Remplacement compatible");
            return true;
        });
    self.state->pendingRelinks.emplace(
        taskId, PendingRelink{mediaId, absolute, stored, result});
    self.binSummaryLabel.stringValue = [NSString
        stringWithFormat:@"Vérification de %s…", absolute.filename().c_str()];
    [self refreshMediaTaskStatus];
}

- (void)batchRelinkOfflineMedia:(id)sender {
    (void)sender;
    if (!self.state || self.state->offlineSourceIds.empty() ||
        !self.state->pendingBatchRelinks.empty())
        return;
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.title = @"Dossier contenant les médias originaux";
    panel.prompt = @"Rechercher et reconnecter";
    panel.allowsMultipleSelection = NO;
    panel.canChooseFiles = NO;
    panel.canChooseDirectories = YES;
    panel.resolvesAliases = YES;
    if ([panel runModal] != NSModalResponseOK || !panel.URL) return;

    std::error_code pathError;
    const std::filesystem::path folder = std::filesystem::weakly_canonical(
        std::filesystem::path(panel.URL.fileSystemRepresentation ?: ""),
        pathError);
    if (pathError) {
        self.binSummaryLabel.stringValue =
            [NSString stringWithFormat:@"Dossier inaccessible : %s",
                                       pathError.message().c_str()];
        return;
    }
    const Document snapshot = self.state->document;
    const std::vector<Ulid> offlineIds(self.state->offlineSourceIds.begin(),
                                       self.state->offlineSourceIds.end());
    const std::filesystem::path projectPath = std::filesystem::absolute(
        std::filesystem::path(self.documentPath.UTF8String ?: ""));
    const std::filesystem::path base = projectPath.parent_path();
    auto result = std::make_shared<BatchRelinkResult>();
    const Ulid taskId = self.state->mediaTasks->Enqueue(
        MediaTaskKind::Relink, "Relink dossier " + folder.filename().string(),
        [folder, base, snapshot, offlineIds, result](MediaTaskContext& context,
                                                     std::string& taskError) {
            const auto keyFor = [](std::string value) {
                std::transform(
                    value.begin(), value.end(), value.begin(),
                    [](unsigned char character) {
                        return static_cast<char>(std::tolower(character));
                    });
                return value;
            };
            std::map<std::string, std::vector<std::filesystem::path>> files;
            std::error_code scanError;
            std::filesystem::recursive_directory_iterator iterator(
                folder,
                std::filesystem::directory_options::skip_permission_denied,
                scanError),
                end;
            while (!scanError && iterator != end) {
                if (context.Cancelled()) {
                    taskError = "batch relink cancelled";
                    return false;
                }
                std::error_code fileError;
                if (iterator->is_regular_file(fileError) && !fileError)
                    files[keyFor(iterator->path().filename().string())]
                        .push_back(iterator->path());
                iterator.increment(scanError);
            }
            if (scanError) {
                taskError =
                    "unable to scan relink folder: " + scanError.message();
                return false;
            }
            for (size_t index = 0; index < offlineIds.size(); ++index) {
                if (context.Cancelled()) {
                    taskError = "batch relink cancelled";
                    return false;
                }
                const LibraryMedia* original =
                    snapshot.FindLibraryMedia(offlineIds[index]);
                if (!original) continue;
                const auto found = files.find(keyFor(original->filename));
                if (found == files.end()) {
                    ++result->unmatched;
                } else if (found->second.size() != 1) {
                    ++result->ambiguous;
                } else {
                    std::error_code canonicalError;
                    const std::filesystem::path absolute =
                        std::filesystem::weakly_canonical(found->second.front(),
                                                          canonicalError);
                    if (canonicalError) {
                        ++result->incompatible;
                        result->last_error = canonicalError.message();
                    } else {
                        std::error_code relativeError;
                        std::string stored = std::filesystem::relative(
                                                 absolute, base, relativeError)
                                                 .lexically_normal()
                                                 .string();
                        if (relativeError) stored = absolute.string();
                        LibraryMedia replacement;
                        replacement.id = original->id;
                        replacement.path = stored;
                        replacement.filename = absolute.filename().string();
                        std::string probeError;
                        if (!ProbeMediaMetadata(absolute.string(), replacement,
                                                probeError) ||
                            !ValidateRelinkCandidate(snapshot, original->id,
                                                     replacement, probeError)) {
                            ++result->incompatible;
                            result->last_error = probeError;
                        } else {
                            result->replacements.push_back(
                                {original->id, std::move(replacement), stored});
                        }
                    }
                }
                context.SetProgress(
                    static_cast<double>(index + 1) / offlineIds.size(),
                    original->filename);
            }
            return true;
        });
    self.state->pendingBatchRelinks.emplace(taskId, PendingBatchRelink{result});
    self.binSummaryLabel.stringValue =
        [NSString stringWithFormat:@"Recherche de %lu média%@ offline…",
                                   (unsigned long)offlineIds.size(),
                                   offlineIds.size() == 1 ? @"" : @"s"];
    [self refreshMediaTaskStatus];
}

- (NSInteger)numberOfRowsInTableView:(NSTableView*)tableView {
    return tableView == self.mediaTable ? self.visibleMediaIds.count : 0;
}

- (NSView*)tableView:(NSTableView*)tableView
    viewForTableColumn:(NSTableColumn*)tableColumn
                   row:(NSInteger)row {
    if (tableView != self.mediaTable || row < 0 ||
        row >= (NSInteger)self.visibleMediaIds.count)
        return nil;
    NSString* identifier = self.visibleMediaIds[row];
    const LibraryMedia* media =
        self.state->document.FindLibraryMedia(identifier.UTF8String ?: "");
    NSTextField* label = [NSTextField labelWithString:@""];
    label.lineBreakMode = NSLineBreakByTruncatingTail;
    if (!media) {
        const DocumentSequence* found =
            self.state->project.FindTimeline(identifier.UTF8String ?: "");
        if (!found) return label;
        const DocumentSequence& sequence = *found;
        if ([tableColumn.identifier isEqualToString:@"name"])
            label.stringValue = [NSString
                stringWithFormat:@"%@◫ %s",
                                 sequence.id == self.state->activeTimelineId
                                     ? @"● "
                                     : @"",
                                 sequence.name.c_str()];
        else if ([tableColumn.identifier isEqualToString:@"format"])
            label.stringValue =
                [NSString stringWithFormat:@"Séquence %dx%d", sequence.width,
                                           sequence.height];
        else {
            Document timelineDocument =
                self.state->project.MakeDocument(sequence.id);
            timelineDocument.sequence = sequence;
            label.stringValue =
                TimeString(Timeline(timelineDocument).Duration());
        }
        return label;
    }
    if ([tableColumn.identifier isEqualToString:@"name"])
        label.stringValue = [NSString
            stringWithFormat:@"%@%s", media->proxy_path.empty() ? @"" : @"P · ",
                             media->filename.c_str()];
    else if ([tableColumn.identifier isEqualToString:@"format"])
        label.stringValue =
            self.state->offlineSourceIds.count(media->id) ? @"Média offline"
            : media->metadata_complete
                ? [NSString stringWithFormat:@"%@%s %dx%d",
                                             media->proxy_path.empty()
                                                 ? @""
                                                 : @"Proxy · ",
                                             media->codec.c_str(), media->width,
                                             media->height]
                : @"—";
    else
        label.stringValue =
            [NSString stringWithFormat:@"%@", TimeString(media->duration)];
    return label;
}

- (void)tableViewSelectionDidChange:(NSNotification*)notification {
    if (notification.object != self.mediaTable) return;
    (void)[self selectedMediaId];
}

- (void)collectionView:(NSCollectionView*)collectionView
    didSelectItemsAtIndexPaths:(NSSet<NSIndexPath*>*)indexPaths {
    (void)collectionView;
    (void)indexPaths;
    (void)[self selectedMediaId];
}

- (void)mediaSearchChanged:(id)sender {
    (void)sender;
    [self rebuildMediaList];
}

- (void)importMediaPressed:(id)sender {
    (void)sender;
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.title = @"Importer des rushes";
    panel.prompt = @"Importer";
    panel.allowsMultipleSelection = YES;
    panel.canChooseFiles = YES;
    panel.canChooseDirectories = YES;
    panel.resolvesAliases = YES;
    NSString* target =
        self.state->document.FindBin(self.selectedBinId.UTF8String ?: "")
            ? self.selectedBinId
            : @"";
    if ([panel runModal] == NSModalResponseOK)
        [self importMediaURLs:panel.URLs intoBin:target];
}

- (BOOL)importMediaURLs:(NSArray<NSURL*>*)urls intoBin:(NSString*)binId {
    const std::string targetBin(binId.UTF8String ?: "");
    if (!targetBin.empty() && !self.state->document.FindBin(targetBin))
        return NO;

    std::vector<std::filesystem::path> candidates;
    for (NSURL* url in urls) {
        if (!url.isFileURL) continue;
        std::filesystem::path path(url.path.UTF8String ?: "");
        std::error_code error;
        if (std::filesystem::is_directory(path, error) && !error) {
            std::filesystem::recursive_directory_iterator iterator(
                path,
                std::filesystem::directory_options::skip_permission_denied,
                error),
                end;
            for (; !error && iterator != end; iterator.increment(error)) {
                if (iterator->is_regular_file(error) && !error)
                    candidates.push_back(iterator->path());
            }
        } else if (!error && std::filesystem::is_regular_file(path, error)) {
            candidates.push_back(path);
        }
    }
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()),
                     candidates.end());
    if (candidates.empty()) {
        self.binSummaryLabel.stringValue = @"Aucun fichier à importer.";
        return NO;
    }

    auto batch = std::make_shared<ProbedImportBatch>();
    batch->document_path = std::filesystem::absolute(
        std::filesystem::path(self.documentPath.UTF8String ?: ""));
    batch->target_bin = targetBin;
    batch->items.resize(candidates.size());
    const std::filesystem::path projectDirectory =
        batch->document_path.parent_path();
    const std::string label = "Import " + std::to_string(candidates.size()) +
                              (candidates.size() == 1 ? " file" : " files");
    const Ulid taskId = self.state->mediaTasks->Enqueue(
        MediaTaskKind::Probe, label,
        [batch, candidates, projectDirectory](MediaTaskContext& context,
                                              std::string& error) {
            for (size_t index = 0; index < candidates.size(); ++index) {
                if (context.Cancelled()) {
                    error = "import cancelled";
                    return false;
                }
                ProbedImportItem& item = batch->items[index];
                std::error_code pathError;
                item.absolute_path = std::filesystem::weakly_canonical(
                    candidates[index], pathError);
                if (pathError) {
                    item.error = pathError.message();
                } else {
                    LibraryMedia media;
                    media.id = GenerateUlid();
                    media.filename = item.absolute_path.filename().string();
                    std::filesystem::path relative = std::filesystem::relative(
                        item.absolute_path, projectDirectory, pathError);
                    media.path = pathError
                                     ? item.absolute_path.string()
                                     : relative.lexically_normal().string();
                    if (ProbeMediaMetadata(item.absolute_path.string(), media,
                                           item.error))
                        item.media = std::move(media);
                }
                context.SetProgress(
                    static_cast<double>(index + 1) / candidates.size(),
                    item.absolute_path.filename().string());
            }
            return true;
        });
    self.state->pendingImports[taskId] = std::move(batch);
    self.binSummaryLabel.stringValue =
        [NSString stringWithFormat:@"Analyse de %lu fichier%@ en arrière-plan…",
                                   (unsigned long)candidates.size(),
                                   candidates.size() == 1 ? @"" : @"s"];
    [self refreshMediaTaskStatus];
    return YES;
}

- (void)generateSelectedMediaProxy:(id)sender {
    (void)sender;
    NSString* identifier = [self selectedMediaId];
    [self enqueueProxyForMediaIdentifier:identifier];
}

- (void)regenerateSelectedMediaThumbnail:(id)sender {
    (void)sender;
    NSString* identifier = [self selectedMediaId];
    if (!identifier) return;
    [self.mediaThumbnails removeObjectForKey:identifier];
    [self enqueueThumbnailForMediaIdentifier:identifier];
}

- (void)enqueueProxyForMediaIdentifier:(NSString*)identifier {
    if (!identifier) return;
    const Ulid mediaId(identifier.UTF8String ?: "");
    const LibraryMedia* media = self.state->document.FindLibraryMedia(mediaId);
    const DocumentSource* source = self.state->document.FindSource(mediaId);
    if (!media || !source) return;
    for (const auto& pending : self.state->pendingProxies) {
        if (pending.second.media_id == mediaId) {
            self.binSummaryLabel.stringValue = @"Un proxy est déjà en cours.";
            return;
        }
    }
    const std::filesystem::path projectPath = std::filesystem::absolute(
        std::filesystem::path(self.documentPath.UTF8String ?: ""));
    const std::filesystem::path base = projectPath.parent_path();
    std::filesystem::path input(media->path);
    if (input.is_relative()) input = base / input;
    input = input.lexically_normal();
    const std::filesystem::path output =
        base / ".cutmachine" / "proxies" / (mediaId + ".mov");
    std::error_code error;
    std::string stored = std::filesystem::relative(output, base, error)
                             .lexically_normal()
                             .string();
    if (error) stored = output.string();
    ProxySettings settings;
    const Ulid taskId = self.state->mediaTasks->Enqueue(
        MediaTaskKind::Proxy, "Proxy " + media->filename,
        [input, output, duration = source->duration, settings](
            MediaTaskContext& context, std::string& taskError) {
            return GenerateMediaProxy(input.string(), output.string(), duration,
                                      settings, context, taskError);
        });
    self.state->pendingProxies.emplace(
        taskId, PendingProxy{mediaId, output, std::move(stored)});
    self.binSummaryLabel.stringValue =
        [NSString stringWithFormat:@"Génération du proxy de %s…",
                                   media->filename.c_str()];
    [self refreshMediaTaskStatus];
}

- (void)removeSelectedMediaProxy:(id)sender {
    (void)sender;
    NSString* identifier = [self selectedMediaId];
    const Ulid mediaId(identifier.UTF8String ?: "");
    LibraryMedia* media = self.state->document.FindLibraryMedia(mediaId);
    if (!media || media->proxy_path.empty()) return;
    for (const auto& pending : self.state->pendingProxies)
        if (pending.second.media_id == mediaId)
            self.state->mediaTasks->Cancel(pending.first);
    const std::filesystem::path projectPath = std::filesystem::absolute(
        std::filesystem::path(self.documentPath.UTF8String ?: ""));
    const std::filesystem::path base = projectPath.parent_path();
    const std::string previousProxyPath = media->proxy_path;
    std::filesystem::path stored(previousProxyPath);
    if (stored.is_relative()) stored = base / stored;
    const std::filesystem::path expected =
        base / ".cutmachine" / "proxies" / (mediaId + ".mov");
    media->proxy_path.clear();
    std::string message;
    if (![self persistEdits:message]) {
        media->proxy_path = previousProxyPath;
        self.binSummaryLabel.stringValue =
            [NSString stringWithFormat:@"Suppression du proxy impossible : %s",
                                       message.c_str()];
        return;
    }
    if (stored.lexically_normal() == expected.lexically_normal()) {
        std::error_code ignored;
        std::filesystem::remove(expected, ignored);
    }
    [self reloadDecodeWorkers];
    [self rebuildMediaList];
    self.binSummaryLabel.stringValue = @"Proxy supprimé · original actif";
}

- (void)menuToggleProxies:(id)sender {
    (void)sender;
    self.state->proxiesEnabled = !self.state->proxiesEnabled;
    [self reloadDecodeWorkers];
    self.infoLabel.stringValue =
        self.state->proxiesEnabled ? @"Proxies activés" : @"Originaux activés";
}

- (void)createBinPressed:(id)sender {
    (void)sender;
    const std::string parent =
        self.state->document.FindBin(self.selectedBinId.UTF8String ?: "")
            ? std::string(self.selectedBinId.UTF8String ?: "")
            : std::string{};
    std::string name = "Nouveau chutier";
    for (int suffix = 2;; ++suffix) {
        const bool exists = std::any_of(
            self.state->document.bins.begin(), self.state->document.bins.end(),
            [&](const DocumentBin& bin) {
                return bin.parent_id == parent && bin.name == name;
            });
        if (!exists) break;
        name = "Nouveau chutier " + std::to_string(suffix);
    }
    const Ulid binId = GenerateUlid();
    EditError error = EditError::None;
    std::string message;
    if (!self.state->editLog.Apply(
            self.state->document,
            Operation{AddBinOperation{binId, name, parent}}, error, message)) {
        self.binSummaryLabel.stringValue = [NSString
            stringWithFormat:@"Création refusée : %s", message.c_str()];
        return;
    }
    if (![self persistEdits:message])
        std::fprintf(stderr, "Unable to persist bin creation: %s\n",
                     message.c_str());
    NSString* identifier = [NSString stringWithUTF8String:binId.c_str()];
    [self refreshBinControlsSelecting:identifier];
    [self beginEditingBin:identifier];
}

- (void)deleteBinPressed:(id)sender {
    (void)sender;
    const std::string binId(self.selectedBinId.UTF8String ?: "");
    if (!self.state->document.FindBin(binId)) return;
    EditError error = EditError::None;
    std::string message;
    if (!self.state->editLog.Apply(self.state->document,
                                   Operation{RemoveBinOperation{binId, "", ""}},
                                   error, message)) {
        self.binSummaryLabel.stringValue = [NSString
            stringWithFormat:@"Suppression refusée : %s", message.c_str()];
        return;
    }
    if (![self persistEdits:message])
        std::fprintf(stderr, "Unable to persist bin deletion: %s\n",
                     message.c_str());
    [self refreshBinControlsSelecting:@"__all__"];
}

- (void)renameBinPressed:(id)sender {
    (void)sender;
    if (!self.state->document.FindBin(self.selectedBinId.UTF8String ?: ""))
        return;
    [self beginEditingBin:self.selectedBinId];
}

- (void)beginEditingBin:(NSString*)binId {
    dispatch_async(dispatch_get_main_queue(), ^{
      const NSInteger row = [self.binOutline rowForItem:binId];
      if (row < 0) return;
      [self.binOutline selectRowIndexes:[NSIndexSet indexSetWithIndex:row]
                   byExtendingSelection:NO];
      [self.binOutline editColumn:0 row:row withEvent:nil select:YES];
    });
}

- (void)controlTextDidEndEditing:(NSNotification*)notification {
    NSTextField* field = notification.object;
    if (![field isKindOfClass:NSTextField.class] ||
        ![field.identifier hasPrefix:@"bin:"])
        return;
    NSString* identifier = [field.identifier substringFromIndex:4];
    const DocumentBin* bin =
        self.state->document.FindBin(identifier.UTF8String ?: "");
    if (!bin) return;
    NSString* name = [field.stringValue
        stringByTrimmingCharactersInSet:NSCharacterSet
                                            .whitespaceAndNewlineCharacterSet];
    if (name.length == 0 || bin->name == (name.UTF8String ?: "")) {
        [self.binOutline reloadItem:identifier];
        return;
    }
    EditError error = EditError::None;
    std::string message;
    if (!self.state->editLog.Apply(
            self.state->document,
            Operation{RenameBinOperation{identifier.UTF8String ?: "",
                                         name.UTF8String ?: ""}},
            error, message)) {
        self.binSummaryLabel.stringValue = [NSString
            stringWithFormat:@"Renommage refusé : %s", message.c_str()];
        return;
    }
    if (![self persistEdits:message])
        std::fprintf(stderr, "Unable to persist bin rename: %s\n",
                     message.c_str());
    [self refreshBinControlsSelecting:identifier];
}

- (void)assignMediaToBinPressed:(id)sender {
    (void)sender;
    NSMutableArray<NSString*>* mediaIds = [NSMutableArray array];
    for (NSString* identifier in [self selectedMediaIds])
        if (self.state->document.FindLibraryMedia(identifier.UTF8String ?: ""))
            [mediaIds addObject:identifier];
    if (mediaIds.count == 0) return;
    const LibraryMedia* selected = self.state->document.FindLibraryMedia(
        mediaIds.firstObject.UTF8String ?: "");

    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText =
        mediaIds.count == 1 ? @"Déplacer le rush" : @"Déplacer les rushes";
    alert.informativeText = [NSString
        stringWithFormat:
            @"Choisissez le chutier de destination pour %lu rush%@.",
            (unsigned long)mediaIds.count, mediaIds.count == 1 ? @"" : @"es"];
    [alert addButtonWithTitle:@"Déplacer"];
    [alert addButtonWithTitle:@"Annuler"];
    NSPopUpButton* destinations =
        [[NSPopUpButton alloc] initWithFrame:NSMakeRect(0, 0, 320, 26)];
    [destinations addItemWithTitle:@"Sans chutier"];
    destinations.lastItem.representedObject = @"";
    std::vector<const DocumentBin*> bins;
    for (const DocumentBin& bin : self.state->document.bins)
        bins.push_back(&bin);
    std::stable_sort(bins.begin(), bins.end(),
                     [](const DocumentBin* left, const DocumentBin* right) {
                         return left->name < right->name;
                     });
    for (const DocumentBin* bin : bins) {
        std::string path = bin->name;
        const DocumentBin* parent =
            self.state->document.FindBin(bin->parent_id);
        while (parent) {
            path = parent->name + " / " + path;
            parent = self.state->document.FindBin(parent->parent_id);
        }
        [destinations
            addItemWithTitle:[NSString stringWithUTF8String:path.c_str()]];
        destinations.lastItem.representedObject =
            [NSString stringWithUTF8String:bin->id.c_str()];
        if (bin->id == selected->bin_id)
            [destinations selectItem:destinations.lastItem];
    }
    if (selected->bin_id.empty()) [destinations selectItemAtIndex:0];
    alert.accessoryView = destinations;
    if ([alert runModal] != NSAlertFirstButtonReturn) return;
    NSString* bin = destinations.selectedItem.representedObject ?: @"";
    EditError error = EditError::None;
    std::string message;
    for (NSString* media in mediaIds) {
        if (!self.state->editLog.Apply(
                self.state->document,
                Operation{SetMediaBinOperation{media.UTF8String ?: "",
                                               bin.UTF8String ?: ""}},
                error, message)) {
            self.binSummaryLabel.stringValue = [NSString
                stringWithFormat:@"Classement refusé : %s", message.c_str()];
            return;
        }
    }
    if (![self persistEdits:message])
        std::fprintf(stderr, "Unable to persist media bin: %s\n",
                     message.c_str());
    [self refreshBinControlsSelecting:bin.length == 0 ? @"__root__" : bin];
}

- (MediaRate)playheadFrameRate {
    if (!self.state->document.sources.empty())
        return self.state->document.sources.front().rate;
    return {25, 1};
}

- (int32_t)playheadInputRate {
    return self.state->playheadResolution == PlayheadResolution::Sample
               ? 48000
               : [self playheadFrameRate].num;
}

- (double)timelineHeight {
    return self.metalView.bounds.size.height;
}

- (double)videoHeight {
    return 0.0;
}

- (NSPoint)timelinePointForEvent:(NSEvent*)event {
    const NSPoint point = [self.metalView convertPoint:event.locationInWindow
                                              fromView:nil];
    return NSMakePoint(point.x, self.metalView.bounds.size.height - point.y -
                                    [self videoHeight]);
}

- (TimelineTool)effectiveTool {
    return self.state->spaceHand ? TimelineTool::Hand : self.state->tool;
}

- (void)applyToolCursor {
    switch ([self effectiveTool]) {
        case TimelineTool::Select:
            [NSCursor.arrowCursor set];
            break;
        case TimelineTool::Hand:
            [(self.state->navigationDragging ? NSCursor.closedHandCursor
                                             : NSCursor.openHandCursor) set];
            break;
        case TimelineTool::Zoom:
            [NSCursor.crosshairCursor set];
            break;
        case TimelineTool::Cut:
            [NSCursor.crosshairCursor set];
            break;
        case TimelineTool::Slip:
            [NSCursor.resizeLeftRightCursor set];
            break;
    }
}

- (void)setTimelineTool:(TimelineTool)tool {
    self.state->tool = tool;
    for (NSInteger index = 0; index < self.timelineToolIcons.count; ++index) {
        self.timelineToolIcons[index].contentTintColor =
            index == static_cast<NSInteger>(tool)
                ? CMAccentBlueColor()
                : NSColor.secondaryLabelColor;
    }
    self.state->interaction->CancelDrag();
    self.state->navigationDragging = false;
    self.state->scrubDragging = false;
    self.state->editDragging = false;
    self.state->lassoCandidate = false;
    self.state->lassoDragging = false;
    self.state->cutPreviewX.reset();
    self.state->cutPreviewY.reset();
    [self applyToolCursor];
    [self updateSelectionInfo];
    self.state->overlayDirty = true;
}

// Moves the playhead by `amount` frames (or samples, in Sample resolution)
// from its current position. Shared by the Left/Right-arrow keyboard
// shortcut and the Transport panel's frame-step buttons (F2.5) so both
// paths compute the same delta and land on the same
// -requestTimelinePosition: -> QuantizePlayheadPosition machinery every
// other playhead move already uses.
- (void)stepPlayheadFrames:(int64_t)amount {
    const RationalTime delta =
        self.state->playheadResolution == PlayheadResolution::Sample
            ? RationalTime{amount, 48000}
            : RationalTime{amount * [self playheadFrameRate].den,
                          [self playheadFrameRate].num};
    [self requestTimelinePosition:self.state->requestedPosition.add(delta)];
}

- (void)requestTimelinePosition:(RationalTime)position {
    if (position < RationalTime{0, 1}) position = {0, position.rate};
    if (position > self.state->duration) position = self.state->duration;
    RationalTime quantized = QuantizePlayheadPosition(
        position, self.state->playheadResolution, [self playheadFrameRate]);
    if (quantized > self.state->duration) {
        if (self.state->playheadResolution == PlayheadResolution::Sample) {
            quantized = {self.state->duration.to_frames(48000), 48000};
        } else {
            const MediaRate rate = [self playheadFrameRate];
            quantized = {
                self.state->duration.to_frames(rate.num, rate.den) * rate.den,
                rate.num};
        }
    }
    [self requestResolvedPosition:quantized];
    if (self.state->playbackDirection == 0 && self.state->audioPlayback) {
        std::string error;
        if (!self.state->audioPlayback->ScrubAt(quantized, error))
            std::fprintf(stderr, "Audio scrub failed: %s\n", error.c_str());
    }
    self.state->overlayDirty = true;
}

- (BOOL)timelineDropMedia:(NSString*)mediaId atViewPoint:(NSPoint)point {
    const double timelineY =
        self.metalView.bounds.size.height - point.y - [self videoHeight];
    if (point.x < self.state->viewport.header_width ||
        timelineY < kTimelineRulerHeight)
        return NO;
    const auto tracks = TimelineTracksInDisplayOrder(self.state->document);
    const NSInteger trackIndex = static_cast<NSInteger>(
        (timelineY - kTimelineRulerHeight) / self.state->viewport.track_height);
    if (trackIndex < 0 || trackIndex >= (NSInteger)tracks.size()) return NO;
    const DocumentTrack* track = tracks[trackIndex];
    if (!track || track->kind != "video") return NO;
    const Ulid sourceId(mediaId.UTF8String ?: "");
    const DocumentSource* source = self.state->document.FindSource(sourceId);
    if (!source) {
        self.binSummaryLabel.stringValue =
            @"Ce média doit être réingéré avant son montage.";
        return NO;
    }
    RationalTime timelineIn;
    try {
        timelineIn =
            self.state->viewport.XToTime(point.x, source->duration.rate);
        if (timelineIn.value < 0) timelineIn = {0, timelineIn.rate};
    } catch (const std::exception& exception) {
        self.binSummaryLabel.stringValue =
            [NSString stringWithFormat:@"Drop refusé : %s", exception.what()];
        return NO;
    }
    Document stagedDocument = self.state->document;
    EditLog stagedLog = self.state->editLog;
    EditError error = EditError::None;
    std::string message;
    Ulid videoClipId;
    Ulid audioClipId;
    const auto detected = self.state->mediaMetadata.find(sourceId);
    const LibraryMedia* media = detected == self.state->mediaMetadata.end()
                                    ? stagedDocument.FindLibraryMedia(sourceId)
                                    : &detected->second;
    Ulid audioTrackId;
    if (media && media->metadata_complete && media->has_audio) {
        for (const DocumentTrack* candidateTrack :
             TimelineTracksInDisplayOrder(stagedDocument)) {
            if (candidateTrack->kind == "audio" && !candidateTrack->locked &&
                self.state->targetedTrackIds.count(candidateTrack->id)) {
                audioTrackId = candidateTrack->id;
                break;
            }
        }
        if (audioTrackId.empty()) {
            for (const DocumentTrack* candidateTrack :
                 TimelineTracksInDisplayOrder(stagedDocument)) {
                if (candidateTrack->kind == "audio" &&
                    !candidateTrack->locked) {
                    audioTrackId = candidateTrack->id;
                    break;
                }
            }
        }
        if (audioTrackId.empty()) {
            int32_t index = 0;
            for (const DocumentTrack& candidateTrack :
                 stagedDocument.sequence.tracks)
                index = std::max(index, candidateTrack.index + 1);
            audioTrackId = GenerateUlid();
            if (!stagedLog.Apply(
                    stagedDocument,
                    Operation{AddTrackOperation{audioTrackId, "audio", index}},
                    error, message)) {
                self.binSummaryLabel.stringValue = [NSString
                    stringWithFormat:
                        @"Création de la piste audio refusée (%s) : %s",
                        EditErrorName(error), message.c_str()];
                return NO;
            }
        }
    }

    std::optional<DeleteGapOperation> sourceEdit = TimelineSourceEditOperation(
        stagedDocument, sourceId, {0, source->duration.rate}, source->duration,
        timelineIn, track->id, true,
        audioTrackId.empty() ? std::vector<Ulid>{}
                             : std::vector<Ulid>{audioTrackId});
    if (!sourceEdit ||
        !stagedLog.Apply(stagedDocument, Operation{std::move(*sourceEdit)},
                         error, message)) {
        self.binSummaryLabel.stringValue =
            [NSString stringWithFormat:@"Insertion refusée (%s) : %s",
                                       EditErrorName(error), message.c_str()];
        return NO;
    }
    if (const DocumentTrack* videoTrack = stagedDocument.FindTrack(track->id)) {
        for (const DocumentClip& clip : videoTrack->clips)
            if (clip.source_id == sourceId && clip.timeline_in == timelineIn &&
                clip.source_in == RationalTime{0, source->duration.rate} &&
                clip.duration == source->duration)
                videoClipId = clip.id;
    }
    if (const DocumentTrack* audioTrack =
            stagedDocument.FindTrack(audioTrackId)) {
        for (const DocumentClip& clip : audioTrack->clips)
            if (clip.source_id == sourceId && clip.timeline_in == timelineIn &&
                clip.source_in == RationalTime{0, source->duration.rate} &&
                clip.duration == source->duration)
                audioClipId = clip.id;
    }

    if (![self persistStagedDocument:stagedDocument
                             editLog:stagedLog
                             message:message]) {
        self.binSummaryLabel.stringValue =
            [NSString stringWithFormat:@"Enregistrement impossible : %s",
                                       message.c_str()];
        return NO;
    }
    self.state->document = std::move(stagedDocument);
    self.state->editLog = std::move(stagedLog);
    if (!audioClipId.empty() && self.state->linkedSelection)
        self.state->interaction->SelectClips({videoClipId, audioClipId});
    else
        self.state->interaction->SelectClip(videoClipId);
    const RationalTime playhead = self.state->requestedPosition;
    [self refreshTimelineAfterEditFromPosition:playhead];
    [self updateSelectionInfo];
    self.state->overlayDirty = true;
    return YES;
}

- (NSString*)transportStatus {
    if (self.state->playbackDirection > 0) return @"Lecture ▶";
    if (self.state->playbackDirection < 0) return @"Lecture ◀";
    return @"Pause";
}

- (NSString*)playheadResolutionStatus {
    return self.state->playheadResolution == PlayheadResolution::Frame
               ? @"Image (M)"
               : @"Échantillon 48 kHz (M)";
}

- (void)setPlaybackDirection:(int)direction {
    self.state->playbackDirection = std::clamp(direction, -1, 1);
    self.state->playbackAnchor = self.state->requestedPosition;
    self.state->playbackStarted = std::chrono::steady_clock::now();
    [self.transportView setPlaying:self.state->playbackDirection != 0];
    if (self.state->audioPlayback) {
        self.state->audioPlayback->Stop();
        if (self.state->playbackDirection != 0) {
            std::string error;
            if (!self.state->audioPlayback->PlayFrom(
                    self.state->playbackAnchor, self.state->playbackDirection,
                    error))
                std::fprintf(stderr, "Audio playback failed: %s\n",
                             error.c_str());
        }
    }
    [self updateSelectionInfo];
}

- (void)updateSelectionInfo {
    // F2.2 -- this is the one hook already fired after every selection
    // change and every edit that could touch the selected clip (see this
    // method's call sites), so it doubles as the Inspector's refresh
    // trigger. SelectedClipId() is empty for zero or multiple selected
    // clips, which CMInspectorView already renders as "no selection".
    [self.inspectorView reloadWithDocument:self.state->document
                            selectedClipId:self.state->interaction
                                               ->SelectedClipId()];
    const size_t selectionCount =
        self.state->interaction->SelectedClipIds().size();
    if (selectionCount > 1) {
        self.infoLabel.stringValue = [NSString
            stringWithFormat:@"%@    Playhead %@    Outil %@    %zu clips "
                             @"sélectionnés    Liens %@",
                             [self transportStatus],
                             [self playheadResolutionStatus],
                             ToolName(self.state->tool), selectionCount,
                             self.state->linkedSelection ? @"ON" : @"OFF"];
        return;
    }
    const DocumentClip* clip = self.state->document.FindClip(
        self.state->interaction->SelectedClipId());
    if (!clip) {
        if (const auto& gap = self.state->interaction->SelectedGap()) {
            self.infoLabel.stringValue = [NSString
                stringWithFormat:@"%@    Playhead %@    Outil %@    Aimant %@  "
                                 @"  Trou sélectionné    piste %s    début %@  "
                                 @"  durée %@    Delete pour raccorder",
                                 [self transportStatus],
                                 [self playheadResolutionStatus],
                                 ToolName(self.state->tool),
                                 self.state->interaction->SnappingEnabled()
                                     ? @"ON (N)"
                                     : @"OFF (N)",
                                 gap->track_id.c_str(), TimeString(gap->start),
                                 TimeString(gap->duration)];
            return;
        }
        self.infoLabel.stringValue = [NSString
            stringWithFormat:@"%@    Playhead %@    Outil %@    Aimant %@    "
                             @"Aucun clip sélectionné",
                             [self transportStatus],
                             [self playheadResolutionStatus],
                             ToolName(self.state->tool),
                             self.state->interaction->SnappingEnabled()
                                 ? @"ON (N)"
                                 : @"OFF (N)"];
        return;
    }
    const DocumentSource* source =
        self.state->document.FindSource(clip->source_id);
    const DocumentTrack* selectedTrack =
        self.state->document.FindTrackForClip(clip->id);
    const auto metadata = self.state->mediaMetadata.find(clip->source_id);
    const LibraryMedia* media = metadata == self.state->mediaMetadata.end()
                                    ? nullptr
                                    : &metadata->second;
    NSString* sourceText =
        source ? [NSString stringWithFormat:@"%s (%s)", source->path.c_str(),
                                            source->id.c_str()]
               : [NSString stringWithUTF8String:clip->source_id.c_str()];
    NSString* metadataText = @"métadonnées indisponibles";
    if (media && media->metadata_complete) {
        metadataText = [NSString
            stringWithFormat:@"%dx%d %@  %s/%s  range %s  matrix %s  rot %d°  "
                             @"%d/%d fps  audio %@",
                             media->width, media->height,
                             [NSString stringWithUTF8String:media->orientation
                                                                .c_str()],
                             media -> pixel_format.c_str(),
                             media->color_transfer.c_str(),
                             media->color_range.c_str(),
                             media->color_space.c_str(),
                             media->rotation_degrees, media->rate.num,
                             media->rate.den,
                             media->has_audio ? @"oui" : @"non"];
    }
    NSString* roleText = @"clip";
    if (selectedTrack && selectedTrack->kind == "audio")
        roleText = @"audio séparé";
    else if (selectedTrack && selectedTrack->kind == "video")
        roleText =
            clip->sync_anchor_clip_id.empty() ? @"vidéo muette" : @"vidéo liée";
    NSString* syncText = @"";
    if (selectedTrack && selectedTrack->kind == "audio") {
        const auto drift = ClipSyncDrift(self.state->document, clip->id);
        if (drift && drift->value != 0) {
            const std::string label =
                SyncDriftLabel(*drift, [self playheadFrameRate]);
            syncText = [NSString
                stringWithFormat:@"    Décalage sync %s", label.c_str()];
        }
    }
    self.infoLabel.stringValue = [NSString
        stringWithFormat:
            @"%@    Playhead %@    Outil %@    Aimant %@    %@%@    source %@  "
            @"  %@    source_in %@    durée %@    timeline_in %@",
            [self transportStatus], [self playheadResolutionStatus],
            ToolName(self.state->tool),
            self.state->interaction->SnappingEnabled() ? @"ON (N)" : @"OFF (N)",
            roleText, syncText, sourceText, metadataText,
            TimeString(clip->source_in), TimeString(clip->duration),
            TimeString(clip->timeline_in)];
}

// F2.2 -- CMInspectorViewDelegate. Every grading slider commit arrives
// here as a ready-to-apply SetClipEffectsOperation; this is the only place
// that actually calls EditLog::Apply for it, matching every other edit in
// this file (e.g. -menuAddCrossDissolve: above) -- CMInspectorView itself
// never touches Document/DocumentClip directly (PHILOSOPHY.md principle
// 2/3). No refreshTimelineAfterEditFromPosition here: a grade change
// leaves every clip's position/duration untouched, it only changes what
// the next render composites (see main.mm's ResolveColorGrade call site),
// so marking overlayDirty is the only redraw trigger this edit needs.
- (void)inspectorView:(CMInspectorView*)inspectorView
    didCommitClipEffects:(SetClipEffectsOperation)operation {
    (void)inspectorView;
    EditError error = EditError::None;
    std::string message;
    if (!self.state->editLog.Apply(self.state->document,
                                   Operation{std::move(operation)}, error,
                                   message)) {
        std::fprintf(stderr, "Grading edit rejected (%s): %s\n",
                     EditErrorName(error), message.c_str());
        return;
    }
    self.state->overlayDirty = true;
    if (![self persistEdits:message])
        std::fprintf(stderr, "Unable to persist grading edit: %s\n",
                     message.c_str());
}

- (void)linkedSelectionPressed:(NSButton*)sender {
    self.state->linkedSelection = sender.state == NSControlStateValueOn;
    self.state->interaction->SetLinkedSelectionEnabled(
        self.state->linkedSelection);
    sender.title = self.state->linkedSelection ? @"Sélection liée : ON"
                                               : @"Sélection liée : OFF";
    const std::vector<Ulid> selected =
        self.state->interaction->SelectedClipIds();
    if (self.state->linkedSelection) {
        self.state->interaction->SelectClips(
            ExpandLinkedClipSelection(self.state->document, selected));
    } else if (selected.size() > 1) {
        self.state->interaction->SelectClip(selected.front());
    }
    [self.window makeFirstResponder:self.metalView];
    [self updateSelectionInfo];
    self.state->overlayDirty = true;
}

- (void)timelineMouseDown:(NSEvent*)event {
    if (self.window.firstResponder == self.sourceMonitorView) {
        self.state->sourceMonitorActive = true;
        [self scrubSourceMonitorEvent:event];
        return;
    }
    if (self.window.firstResponder == self.programMonitorView) {
        self.state->sourceMonitorActive = false;
        return;
    }
    self.state->sourceMonitorActive = false;
    self.state->duplicateDragging = false;
    self.state->duplicateDragClipboard.clear();
    const NSPoint point = [self timelinePointForEvent:event];
    if (point.y < 0.0 || point.y > [self timelineHeight]) return;
    const double addTrackY =
        kTimelineRulerHeight + self.state->document.sequence.tracks.size() *
                                   self.state->viewport.track_height;
    if (point.y >= kTimelineRulerHeight && point.y < addTrackY &&
        point.x >= 50.0 && point.x < 74.0) {
        const auto tracks = TimelineTracksInDisplayOrder(self.state->document);
        const size_t index =
            static_cast<size_t>((point.y - kTimelineRulerHeight) /
                                self.state->viewport.track_height);
        if (index < tracks.size()) {
            const Ulid id = tracks[index]->id;
            if (self.state->targetedTrackIds.count(id))
                self.state->targetedTrackIds.erase(id);
            else
                self.state->targetedTrackIds.insert(id);
            self.state->timelineTargetTracks[self.state->activeTimelineId] =
                self.state->targetedTrackIds;
            self.infoLabel.stringValue = self.state->targetedTrackIds.count(id)
                                             ? @"Piste ciblée"
                                             : @"Piste exclue du ciblage";
            self.state->overlayDirty = true;
        }
        return;
    }
    if (point.y >= kTimelineRulerHeight && point.y < addTrackY &&
        point.x >= 104.0 && point.x < 120.0) {
        const auto tracks = TimelineTracksInDisplayOrder(self.state->document);
        const size_t index =
            static_cast<size_t>((point.y - kTimelineRulerHeight) /
                                self.state->viewport.track_height);
        if (index < tracks.size()) {
            EditError error = EditError::None;
            std::string message;
            const RationalTime playhead = self.state->requestedPosition;
            if (self.state->editLog.Apply(
                    self.state->document,
                    Operation{SetTrackLockOperation{tracks[index]->id,
                                                    !tracks[index]->locked}},
                    error, message)) {
                [self refreshTimelineAfterEditFromPosition:playhead];
                [self persistEdits:message];
                self.state->overlayDirty = true;
            }
        }
        return;
    }
    if (point.y >= kTimelineRulerHeight && point.y < addTrackY &&
        point.x >= 127.0 && point.x < 143.0) {
        const auto tracks = TimelineTracksInDisplayOrder(self.state->document);
        const size_t index =
            static_cast<size_t>((point.y - kTimelineRulerHeight) /
                                self.state->viewport.track_height);
        if (index < tracks.size()) {
            EditError error = EditError::None;
            std::string message;
            const RationalTime playhead = self.state->requestedPosition;
            if (self.state->editLog.Apply(
                    self.state->document,
                    Operation{SetTrackSyncLockOperation{
                        tracks[index]->id, !tracks[index]->sync_lock}},
                    error, message)) {
                [self refreshTimelineAfterEditFromPosition:playhead];
                [self persistEdits:message];
                self.state->overlayDirty = true;
            }
        }
        return;
    }
    if (point.y >= addTrackY && point.y < addTrackY + kAddTrackRowHeight &&
        point.x >= 0.0 && point.x < self.state->viewport.header_width) {
        [self addTrack:(point.x >= self.state->viewport.header_width * 0.5)];
        return;
    }
    if (point.y < kTimelineRulerHeight && point.x >= 0.0 &&
        point.x < self.state->viewport.header_width) {
        constexpr double toolStart = 108.0;
        constexpr double toolWidth = 17.0;
        if (point.x >= toolStart) {
            const int index = std::clamp(
                static_cast<int>((point.x - toolStart) / toolWidth), 0, 4);
            [self setTimelineTool:static_cast<TimelineTool>(index)];
        }
        return;
    }
    const TimelineTool tool = [self effectiveTool];
    self.state->lassoCandidate = false;
    self.state->lassoDragging = false;
    if (tool == TimelineTool::Hand) {
        if (self.state->spaceHand) self.state->spaceUsedForPan = true;
        self.state->navigationDragging = true;
        self.state->navigationLastX = point.x;
        [self applyToolCursor];
        return;
    }
    if (tool == TimelineTool::Zoom) {
        const bool zoomOut =
            (event.modifierFlags & NSEventModifierFlagOption) != 0;
        self.state->viewport.ZoomAroundX(point.x, zoomOut ? 0.5 : 2.0,
                                         self.state->duration.rate);
        self.state->overlayDirty = true;
        return;
    }
    if (tool == TimelineTool::Cut) {
        [self setPlaybackDirection:0];
        const auto hit =
            HitTestTimeline(self.state->document, self.state->viewport, point.x,
                            point.y, self.metalView.bounds.size.width);
        if (!hit) return;
        const DocumentClip* clip = self.state->document.FindClip(hit->clip_id);
        if (!clip) return;
        const bool linkedCut =
            self.state->linkedSelection &&
            (event.modifierFlags & NSEventModifierFlagOption) == 0;
        Operation operation = TimelineCutOperationForClip(
            self.state->document, *clip,
            self.state->viewport.XToTime(point.x, clip->duration.rate),
            linkedCut);
        EditError error = EditError::None;
        std::string message;
        const RationalTime playhead = self.state->requestedPosition;
        if (self.state->editLog.Apply(self.state->document,
                                      std::move(operation), error, message)) {
            if (linkedCut)
                self.state->interaction->SelectClips(ExpandLinkedClipSelection(
                    self.state->document, {hit->clip_id}));
            else
                self.state->interaction->SelectClip(hit->clip_id);
            [self refreshTimelineAfterEditFromPosition:playhead];
            if (![self persistEdits:message])
                std::fprintf(stderr, "Unable to persist cut: %s\n",
                             message.c_str());
            [self updateSelectionInfo];
            self.state->overlayDirty = true;
        } else if (error != EditError::InvalidTimelineIn) {
            std::fprintf(stderr, "Cut rejected (%s): %s\n",
                         EditErrorName(error), message.c_str());
        }
        return;
    }
    const auto selectionHit =
        HitTestTimeline(self.state->document, self.state->viewport, point.x,
                        point.y, self.metalView.bounds.size.width);
    if (tool == TimelineTool::Select && !selectionHit &&
        point.x >= self.state->viewport.header_width &&
        point.y >= kTimelineRulerHeight && point.y < addTrackY) {
        self.state->lassoCandidate = true;
        self.state->lassoStartX = self.state->lassoCurrentX = point.x;
        self.state->lassoStartY = self.state->lassoCurrentY = point.y;
    }
    [self setPlaybackDirection:0];
    const bool optionGesture =
        (event.modifierFlags & NSEventModifierFlagOption) != 0;
    const bool duplicateGesture =
        tool == TimelineTool::Select && selectionHit &&
        selectionHit->edge == TimelineHitEdge::None && optionGesture;
    self.state->linkedSelectionGesture =
        self.state->linkedSelection && (!optionGesture || duplicateGesture);
    self.state->interaction->SetLinkedSelectionEnabled(
        self.state->linkedSelectionGesture);
    const TimelineTrimMode trimMode =
        tool == TimelineTool::Slip ? TimelineTrimMode::Slip
        : (event.modifierFlags & NSEventModifierFlagCommand) != 0
            ? TimelineTrimMode::Roll
        : (event.modifierFlags & NSEventModifierFlagShift) != 0
            ? TimelineTrimMode::Ripple
            : TimelineTrimMode::Normal;
    self.state->interaction->SetTrimMode(trimMode);
    self.state->interaction->PointerDown(point.x, point.y,
                                         self.metalView.bounds.size.width,
                                         [self playheadInputRate]);
    if (self.state->linkedSelectionGesture && selectionHit) {
        self.state->interaction->SelectClips(ExpandLinkedClipSelection(
            self.state->document, {selectionHit->clip_id}));
    }
    self.state->editDragging = self.state->interaction->HasActiveDrag();
    self.state->duplicateDragging =
        duplicateGesture && self.state->editDragging;
    if (self.state->duplicateDragging)
        self.state->duplicateDragClipboard = CopyTimelineClips(
            self.state->document, self.state->interaction->SelectedClipIds());
    self.state->scrubDragging =
        self.state->interaction->RequestedPlayhead().has_value();
    if (self.state->interaction->RequestedPlayhead()) {
        [self requestTimelinePosition:*self.state->interaction
                                           ->RequestedPlayhead()];
        self.state->interaction->ClearRequestedPlayhead();
    }
    [self updateSelectionInfo];
    self.state->overlayDirty = true;
}

- (void)timelineMouseDragged:(NSEvent*)event {
    if (self.state->sourceMonitorActive &&
        self.window.firstResponder == self.sourceMonitorView) {
        [self scrubSourceMonitorEvent:event];
        return;
    }
    const NSPoint point = [self timelinePointForEvent:event];
    if (self.state->navigationDragging) {
        const double delta = self.state->navigationLastX - point.x;
        self.state->viewport.ScrollByPixels(delta, self.state->duration.rate);
        self.state->navigationLastX = point.x;
        self.state->overlayDirty = true;
        return;
    }
    if (self.state->lassoCandidate) {
        self.state->lassoCurrentX = point.x;
        self.state->lassoCurrentY = point.y;
        const double deltaX = point.x - self.state->lassoStartX;
        const double deltaY = point.y - self.state->lassoStartY;
        if (!self.state->lassoDragging && std::hypot(deltaX, deltaY) >= 4.0) {
            self.state->lassoDragging = true;
            self.state->scrubDragging = false;
        }
        if (self.state->lassoDragging) {
            std::vector<Ulid> selected = LassoHitTestTimeline(
                self.state->document, self.state->viewport,
                self.state->lassoStartX, self.state->lassoStartY,
                self.state->lassoCurrentX, self.state->lassoCurrentY,
                self.metalView.bounds.size.width);
            if (self.state->linkedSelectionGesture)
                selected =
                    ExpandLinkedClipSelection(self.state->document, selected);
            self.state->interaction->SelectClips(selected);
            [self updateSelectionInfo];
            self.state->overlayDirty = true;
            return;
        }
    }
    if (self.state->scrubDragging) {
        [self requestTimelinePosition:self.state->viewport.XToTime(
                                          point.x, [self playheadInputRate])];
        return;
    }
    self.state->interaction->PointerDrag(point.x, point.y,
                                         self.metalView.bounds.size.width);
    // AppKit may report Option only after the mouse-down event. Promote an
    // active body move to a duplicate as soon as the modifier appears, which
    // matches the interaction users expect from Premiere/Resolve.
    if (!self.state->duplicateDragging && self.state->editDragging &&
        [self effectiveTool] == TimelineTool::Select &&
        (event.modifierFlags & NSEventModifierFlagOption) != 0 &&
        self.state->interaction->MovePreview()) {
        self.state->duplicateDragging = true;
        self.state->duplicateDragClipboard = CopyTimelineClips(
            self.state->document, self.state->interaction->SelectedClipIds());
    }
    self.state->overlayDirty = true;
}

- (void)timelineMouseMoved:(NSEvent*)event {
    [self applyToolCursor];
    self.state->cutPreviewX.reset();
    self.state->cutPreviewY.reset();
    if ([self effectiveTool] == TimelineTool::Select) {
        const NSPoint point = [self timelinePointForEvent:event];
        const auto hit =
            HitTestTimeline(self.state->document, self.state->viewport, point.x,
                            point.y, self.metalView.bounds.size.width);
        if (hit && hit->edge != TimelineHitEdge::None) {
            [NSCursor.resizeLeftRightCursor set];
            if ((event.modifierFlags & NSEventModifierFlagCommand) != 0)
                self.infoLabel.stringValue = @"ROLL EDIT · Cmd-glisser";
            else if ((event.modifierFlags & NSEventModifierFlagShift) != 0)
                self.infoLabel.stringValue = @"RIPPLE TRIM · Shift-glisser";
            else
                self.infoLabel.stringValue = @"TRIM · glisser le raccord";
        } else if (hit &&
                   (event.modifierFlags & NSEventModifierFlagOption) != 0) {
            self.infoLabel.stringValue = @"DUPLIQUER · Option-glisser le clip";
        }
        self.state->overlayDirty = true;
        return;
    }
    if ([self effectiveTool] == TimelineTool::Slip) {
        const NSPoint point = [self timelinePointForEvent:event];
        const auto hit =
            HitTestTimeline(self.state->document, self.state->viewport, point.x,
                            point.y, self.metalView.bounds.size.width);
        if (hit)
            self.infoLabel.stringValue = @"SLIP · glisser le contenu source";
        self.state->overlayDirty = true;
        return;
    }
    if ([self effectiveTool] != TimelineTool::Cut) {
        self.state->overlayDirty = true;
        return;
    }
    const NSPoint point = [self timelinePointForEvent:event];
    if (point.y < kTimelineRulerHeight || point.y > [self timelineHeight]) {
        self.state->overlayDirty = true;
        return;
    }
    const auto hit =
        HitTestTimeline(self.state->document, self.state->viewport, point.x,
                        point.y, self.metalView.bounds.size.width);
    const DocumentClip* clip =
        hit ? self.state->document.FindClip(hit->clip_id) : nullptr;
    if (clip) {
        const RationalTime cut =
            self.state->viewport.XToTime(point.x, clip->duration.rate);
        if (cut > clip->timeline_in &&
            cut < clip->timeline_in.add(clip->duration)) {
            self.state->cutPreviewX = self.state->viewport.TimeToX(cut);
            const auto tracks =
                TimelineTracksInDisplayOrder(self.state->document);
            const auto track = std::find_if(
                tracks.begin(), tracks.end(), [&](const DocumentTrack* value) {
                    return value->id == hit->track_id;
                });
            if (track != tracks.end())
                self.state->cutPreviewY = kTimelineRulerHeight +
                                          std::distance(tracks.begin(), track) *
                                              self.state->viewport.track_height;
        }
    }
    self.state->overlayDirty = true;
}

- (BOOL)persistEdits:(std::string&)message {
    const BOOL persisted = [self persistStagedDocument:self.state->document
                                               editLog:self.state->editLog
                                               message:message];
    if (persisted) self.state->lastHistoryDomain = HistoryDomain::Timeline;
    return persisted;
}

- (BOOL)persistStagedDocument:(const Document&)document
                      editLog:(const EditLog&)editLog
                      message:(std::string&)message {
    Project candidate = self.state->project;
    if (!candidate.CommitDocument(self.state->activeTimelineId, document,
                                  message))
        return NO;
    if (![self commitProjectCandidate:candidate
                              editLog:editLog
                              message:message])
        return NO;
    self.state->project = std::move(candidate);
    self.state->timelineEditLogs[self.state->activeTimelineId] = editLog;
    return YES;
}

- (BOOL)commitProjectCandidate:(const Project&)project
                       editLog:(const EditLog&)editLog
                       message:(std::string&)message {
    return [self commitProjectCandidate:project
                                editLog:editLog
                             projectLog:self.state->projectEditLog
                                message:message];
}

- (BOOL)commitProjectCandidate:(const Project&)project
                       editLog:(const EditLog&)editLog
                    projectLog:(const ProjectEditLog&)projectLog
                       message:(std::string&)message {
    const char* path = self.documentPath.UTF8String;
    if (!ProjectRecovery::WriteAutosave(path ? path : "", project, message))
        return NO;
    std::map<std::string, EditLog> timelineLogs = self.state->timelineEditLogs;
    timelineLogs[self.state->activeTimelineId] = editLog;
    if (!CommitStoredProjectAndLogs(path ? path : "", project, timelineLogs,
                                    projectLog, message))
        return NO;
    std::string discardError;
    if (!ProjectRecovery::DiscardAutosave(path ? path : "", discardError))
        std::fprintf(stderr, "Unable to discard committed autosave: %s\n",
                     discardError.c_str());
    return YES;
}

- (void)rebuildVideoTrackIds {
    std::vector<const DocumentTrack*> tracks;
    for (const DocumentTrack& track : self.state->document.sequence.tracks)
        if (track.kind == "video") tracks.push_back(&track);
    std::sort(tracks.begin(), tracks.end(),
              [](const DocumentTrack* left, const DocumentTrack* right) {
                  return left->index < right->index;
              });
    self.state->videoTrackIds.clear();
    for (const DocumentTrack* track : tracks)
        self.state->videoTrackIds.push_back(track->id);
    self.state->requested.assign(tracks.size(), {});
    self.state->rendered.assign(tracks.size(), {});
}

- (void)addTrack:(BOOL)audio {
    int32_t index = 0;
    for (const DocumentTrack& track : self.state->document.sequence.tracks)
        index = std::max(index, track.index + 1);
    EditError error = EditError::None;
    std::string message;
    const RationalTime playhead = self.state->requestedPosition;
    const Ulid trackId = GenerateUlid();
    Operation operation =
        AddTrackOperation{trackId, audio ? "audio" : "video", index};
    if (self.state->editLog.Apply(self.state->document, std::move(operation),
                                  error, message)) {
        self.state->targetedTrackIds.insert(trackId);
        self.state->timelineTargetTracks[self.state->activeTimelineId] =
            self.state->targetedTrackIds;
        [self refreshTimelineAfterEditFromPosition:playhead];
        if (![self persistEdits:message])
            std::fprintf(stderr, "Unable to persist track creation: %s\n",
                         message.c_str());
        [self updateSelectionInfo];
        self.state->overlayDirty = true;
    } else {
        std::fprintf(stderr, "Track creation rejected (%s): %s\n",
                     EditErrorName(error), message.c_str());
    }
}

- (void)refreshTimelineAfterEditFromPosition:(RationalTime)position {
    self.state->interaction->ClearGapSelection();
    [self rebuildVideoTrackIds];
    if (self.state->audioPlayback)
        self.state->audioPlayback->RebuildTimeline(self.state->document);
    self.state->duration = self.state->timeline->Duration();
    if (position < RationalTime{0, 1}) position = {0, 1};
    if (position > self.state->duration) position = self.state->duration;
    RationalTime quantized = QuantizePlayheadPosition(
        position, self.state->playheadResolution, [self playheadFrameRate]);
    if (quantized > self.state->duration) quantized = self.state->duration;
    [self refreshTimelineChrome];
    [self requestResolvedPosition:quantized];
}

- (void)timelineMouseUp:(NSEvent*)event {
    if (self.state->sourceMonitorActive &&
        self.window.firstResponder == self.sourceMonitorView)
        return;
    (void)event;
    if (self.state->lassoCandidate) {
        const bool completedLasso = self.state->lassoDragging;
        self.state->lassoCandidate = false;
        self.state->lassoDragging = false;
        if (completedLasso) {
            self.state->scrubDragging = false;
            self.state->interaction->SetLinkedSelectionEnabled(
                self.state->linkedSelection);
            [self updateSelectionInfo];
            self.state->overlayDirty = true;
            return;
        }
    }
    if (self.state->navigationDragging || self.state->scrubDragging) {
        self.state->navigationDragging = false;
        self.state->scrubDragging = false;
        self.state->interaction->SetLinkedSelectionEnabled(
            self.state->linkedSelection);
        [self applyToolCursor];
        return;
    }
    if (!self.state->editDragging) {
        self.state->duplicateDragging = false;
        self.state->duplicateDragClipboard.clear();
        self.state->interaction->SetLinkedSelectionEnabled(
            self.state->linkedSelection);
        return;
    }
    self.state->editDragging = false;
    const RationalTime playhead = self.state->requestedPosition;
    EditError error = EditError::None;
    std::string message;
    if (self.state->duplicateDragging) {
        const auto preview = self.state->interaction->MovePreview();
        self.state->interaction->CancelDrag();
        self.state->duplicateDragging = false;
        const auto operation =
            preview ? PasteTimelineClipboardAtMoves(
                          self.state->duplicateDragClipboard, *preview)
                    : std::nullopt;
        self.state->duplicateDragClipboard.clear();
        if (operation)
            [self applyTimelinePaste:*operation label:@"Clips dupliqués"];
        else
            self.infoLabel.stringValue = @"Duplication annulée";
    } else if (self.state->interaction->PointerUp(error, message)) {
        [self refreshTimelineAfterEditFromPosition:playhead];
        if (![self persistEdits:message])
            std::fprintf(stderr, "Unable to persist edit: %s\n",
                         message.c_str());
    } else if (error != EditError::None) {
        std::fprintf(stderr, "Edit rejected (%s): %s\n", EditErrorName(error),
                     message.c_str());
    }
    [self updateSelectionInfo];
    self.state->interaction->SetLinkedSelectionEnabled(
        self.state->linkedSelection);
    self.state->overlayDirty = true;
}

- (void)timelineScroll:(NSEvent*)event {
    if (self.window.firstResponder == self.sourceMonitorView) return;
    const NSPoint point = [self timelinePointForEvent:event];
    if (point.y < 0.0 || point.y > [self timelineHeight]) return;
    const double delta = std::abs(event.scrollingDeltaX) > 0.01
                             ? event.scrollingDeltaX
                             : event.scrollingDeltaY;
    try {
        if ((event.modifierFlags & NSEventModifierFlagCommand) != 0) {
            self.state->viewport.ZoomAroundX(point.x, std::exp(-delta * 0.01),
                                             self.state->duration.rate);
        } else {
            self.state->viewport.ScrollByPixels(delta,
                                                self.state->duration.rate);
        }
        self.state->overlayDirty = true;
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "Viewport update rejected: %s\n",
                     exception.what());
    }
}

- (BOOL)timelineKeyDown:(NSEvent*)event {
    const NSEventModifierFlags modifiers =
        event.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask;
    if (self.state->sourceMonitorActive && self.state->sourceMonitor) {
        const DocumentSource* source =
            self.state->document.FindSource(self.state->sourceMonitorId);
        if (source && (event.keyCode == 123 || event.keyCode == 124)) {
            const int64_t amount =
                (modifiers & NSEventModifierFlagShift) != 0 ? 10 : 1;
            const int64_t direction = event.keyCode == 123 ? -1 : 1;
            [self requestSourcePosition:self.state->sourceMonitorPosition.add(
                                            {direction * amount *
                                                 source->rate.den,
                                             source->rate.num})];
            return YES;
        }
        if (source && event.keyCode == 115) {
            [self requestSourcePosition:{0, source->duration.rate}];
            return YES;
        }
        if (source && event.keyCode == 119) {
            [self requestSourcePosition:source->duration];
            return YES;
        }
    }
    NSString* characters = event.charactersIgnoringModifiers.lowercaseString;
    if ([self event:event matchesShortcut:@"view.fullscreen"]) {
        [self toggleFullscreen:nil];
        return YES;
    }
    if ([self event:event matchesShortcut:@"play.toggle"]) {
        if ([characters isEqualToString:@" "]) {
            if (!event.isARepeat) self.state->spaceUsedForPan = false;
            self.state->spaceHand = true;
            [self applyToolCursor];
        } else if (!event.isARepeat) {
            [self menuPlayPause:nil];
        }
        return YES;
    }
    if ([self event:event matchesShortcut:@"timeline.linked"]) {
        [self menuToggleLinkedSelection:nil];
        return YES;
    }
    if ([self event:event matchesShortcut:@"edit.copy"]) {
        [self menuCopyTimelineClips:nil];
        return YES;
    }
    if ([self event:event matchesShortcut:@"edit.paste"]) {
        [self menuPasteTimelineClips:nil];
        return YES;
    }
    if ([self event:event matchesShortcut:@"transition.add"]) {
        [self menuAddCrossDissolve:nil];
        return YES;
    }
    if ([self event:event matchesShortcut:@"edit.ripple"]) {
        [self deleteCurrentTimelineSelectionRipple:YES];
        return YES;
    }
    if ([self event:event matchesShortcut:@"edit.delete"]) {
        [self deleteCurrentTimelineSelectionRipple:NO];
        return YES;
    }
    if ([self event:event matchesShortcut:@"mark.in"]) {
        [self menuMarkTimelineIn:nil];
        return YES;
    }
    if ([self event:event matchesShortcut:@"mark.out"]) {
        [self menuMarkTimelineOut:nil];
        return YES;
    }
    if ([self event:event matchesShortcut:@"mark.clear"]) {
        [self menuClearTimelineRange:nil];
        return YES;
    }
    if ([self event:event matchesShortcut:@"source.insert"]) {
        [self menuInsertSource:nil];
        return YES;
    }
    if ([self event:event matchesShortcut:@"source.overwrite"]) {
        [self menuOverwriteSource:nil];
        return YES;
    }
    if ([self event:event matchesShortcut:@"tool.select"]) {
        [self setTimelineTool:TimelineTool::Select];
        return YES;
    }
    if ([self event:event matchesShortcut:@"tool.hand"]) {
        [self setTimelineTool:TimelineTool::Hand];
        return YES;
    }
    if ([self event:event matchesShortcut:@"tool.zoom"]) {
        [self setTimelineTool:TimelineTool::Zoom];
        return YES;
    }
    if ([self event:event matchesShortcut:@"tool.cut"] ||
        [self event:event matchesShortcut:@"tool.cut.alternate"]) {
        [self setTimelineTool:TimelineTool::Cut];
        return YES;
    }
    if ([self event:event matchesShortcut:@"tool.slip"]) {
        [self setTimelineTool:TimelineTool::Slip];
        return YES;
    }
    if ([self event:event matchesShortcut:@"timeline.fit"]) {
        [self menuFitTimeline:nil];
        return YES;
    }
    if ([self event:event matchesShortcut:@"timeline.snapping"]) {
        [self menuToggleSnapping:nil];
        return YES;
    }
    if ([self event:event matchesShortcut:@"play.reverse"]) {
        [self menuPlayReverse:nil];
        return YES;
    }
    if ([self event:event matchesShortcut:@"play.stop"]) {
        [self menuStop:nil];
        return YES;
    }
    if ([self event:event matchesShortcut:@"play.forward"]) {
        [self menuPlayForward:nil];
        return YES;
    }
    if ((modifiers & NSEventModifierFlagCommand) != 0) {
        if ([characters isEqualToString:@"t"] &&
            (modifiers & NSEventModifierFlagShift) != 0) {
            [self addTrack:(modifiers & NSEventModifierFlagOption) != 0];
            return YES;
        }
        if (![characters isEqualToString:@"z"]) return NO;
        if ((modifiers & NSEventModifierFlagShift) != 0)
            [self menuRedo:nil];
        else
            [self menuUndo:nil];
        return YES;
    }
    if ([characters isEqualToString:@"m"]) {
        self.state->playheadResolution =
            self.state->playheadResolution == PlayheadResolution::Frame
                ? PlayheadResolution::Sample
                : PlayheadResolution::Frame;
        if (self.state->playbackDirection == 0)
            [self requestTimelinePosition:self.state->requestedPosition];
        [self updateSelectionInfo];
        self.state->overlayDirty = true;
        return YES;
    }
    if ([characters isEqualToString:@"+"] ||
        [characters isEqualToString:@"="] ||
        [characters isEqualToString:@"-"]) {
        const bool zoomOut = [characters isEqualToString:@"-"];
        const double center = (self.state->viewport.header_width +
                               self.metalView.bounds.size.width) /
                              2.0;
        self.state->viewport.ZoomAroundX(center, zoomOut ? (1.0 / 1.5) : 1.5,
                                         self.state->duration.rate);
        self.state->overlayDirty = true;
        return YES;
    }
    if (event.keyCode == 115) {  // Home
        [self requestTimelinePosition:{0, self.state->duration.rate}];
        return YES;
    }
    if (event.keyCode == 119) {  // End
        [self requestTimelinePosition:self.state->duration];
        return YES;
    }
    if (event.keyCode == 123 || event.keyCode == 124) {  // Left / right
        const int64_t amount =
            (modifiers & NSEventModifierFlagShift) != 0 ? 10 : 1;
        const int64_t direction = event.keyCode == 123 ? -1 : 1;
        [self stepPlayheadFrames:direction * amount];
        return YES;
    }
    if (event.keyCode == 53) {  // Escape
        self.state->interaction->CancelDrag();
        self.state->navigationDragging = false;
        self.state->scrubDragging = false;
        self.state->editDragging = false;
        self.state->duplicateDragging = false;
        self.state->duplicateDragClipboard.clear();
        self.state->lassoCandidate = false;
        self.state->lassoDragging = false;
        self.state->overlayDirty = true;
        return YES;
    }
    return NO;
}

- (void)timelineKeyUp:(NSEvent*)event {
    if ([event.charactersIgnoringModifiers isEqualToString:@" "] &&
        [self event:event matchesShortcut:@"play.toggle"]) {
        self.state->spaceHand = false;
        [self applyToolCursor];
        if (!self.state->spaceUsedForPan) {
            [self setPlaybackDirection:self.state->playbackDirection == 0 ? 1
                                                                          : 0];
        }
    }
}

- (void)menuCopyTimelineClips:(id)sender {
    (void)sender;
    std::vector<Ulid> clipIds = self.state->interaction->SelectedClipIds();
    if (self.state->linkedSelection)
        clipIds = ExpandLinkedClipSelection(self.state->document, clipIds);
    self.state->timelineClipboard =
        CopyTimelineClips(self.state->document, clipIds);
    if (self.state->timelineClipboard.empty()) {
        self.infoLabel.stringValue = @"Aucun clip à copier";
        return;
    }
    self.infoLabel.stringValue = [NSString
        stringWithFormat:@"%lu clip%@ copié%@",
                         (unsigned long)self.state->timelineClipboard.size(),
                         self.state->timelineClipboard.size() == 1 ? @"" : @"s",
                         self.state->timelineClipboard.size() == 1 ? @""
                                                                   : @"s"];
}

- (BOOL)applyTimelinePaste:(PasteClipsOperation)operation
                     label:(NSString*)label {
    EditError error = EditError::None;
    std::string message;
    const RationalTime playhead = self.state->requestedPosition;
    if (!self.state->editLog.Apply(self.state->document,
                                   Operation{std::move(operation)}, error,
                                   message)) {
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"%@ refusé (%s) : %s", label,
                                       EditErrorName(error), message.c_str()];
        return NO;
    }
    const auto& stored = std::get<PasteClipsOperation>(
        self.state->editLog.AppliedEntries().back().op);
    std::vector<Ulid> pastedIds;
    pastedIds.reserve(stored.clips.size());
    for (const PastedClip& clip : stored.clips)
        pastedIds.push_back(clip.clip_id);
    self.state->interaction->SelectClips(pastedIds);
    [self refreshTimelineAfterEditFromPosition:playhead];
    if (![self persistEdits:message]) {
        std::fprintf(stderr, "Unable to persist paste: %s\n", message.c_str());
        self.infoLabel.stringValue = [NSString
            stringWithFormat:@"%@ non sauvegardé : %s", label, message.c_str()];
        return NO;
    }
    [self updateSelectionInfo];
    self.infoLabel.stringValue =
        [NSString stringWithFormat:@"%@ · %lu clip%@", label,
                                   (unsigned long)pastedIds.size(),
                                   pastedIds.size() == 1 ? @"" : @"s"];
    self.state->overlayDirty = true;
    return YES;
}

- (void)menuPasteTimelineClips:(id)sender {
    (void)sender;
    const auto operation = PasteTimelineClipboardAt(
        self.state->document, self.state->timelineClipboard,
        self.state->requestedPosition);
    if (!operation) {
        self.infoLabel.stringValue =
            @"Collage impossible : aucune piste compatible";
        return;
    }
    PasteClipsOperation overwrite = *operation;
    overwrite.overwrite = true;
    [self applyTimelinePaste:std::move(overwrite)
                       label:@"Clips collés en overwrite"];
}

- (void)menuUndo:(id)sender {
    (void)sender;
    EditError error = EditError::None;
    std::string message;
    const bool projectHistory =
        (self.state->lastHistoryDomain == HistoryDomain::Project &&
         self.state->projectEditLog.AppliedCount() > 0) ||
        (self.state->editLog.AppliedCount() == 0 &&
         self.state->projectEditLog.AppliedCount() > 0);
    if (projectHistory) {
        Project candidate = self.state->project;
        ProjectEditLog projectLog = self.state->projectEditLog;
        if (!projectLog.Undo(candidate, error, message)) {
            if (error != EditError::EmptyUndo)
                std::fprintf(stderr, "Project undo failed (%s): %s\n",
                             EditErrorName(error), message.c_str());
            return;
        }
        if (![self commitProjectCandidate:candidate
                                  editLog:self.state->editLog
                               projectLog:projectLog
                                  message:message]) {
            std::fprintf(stderr, "Unable to persist project undo: %s\n",
                         message.c_str());
            return;
        }
        self.state->project = std::move(candidate);
        self.state->projectEditLog = std::move(projectLog);
        self.state->lastHistoryDomain = HistoryDomain::Project;
        [self rebuildMediaList];
        [self refreshAfterProjectMutation];
        return;
    }
    const RationalTime playhead = self.state->requestedPosition;
    if (self.state->editLog.Undo(self.state->document, error, message)) {
        [self refreshTimelineAfterEditFromPosition:playhead];
        [self refreshBinControlsSelecting:nil];
        if (![self persistEdits:message])
            std::fprintf(stderr, "Unable to persist undo: %s\n",
                         message.c_str());
        [self updateSelectionInfo];
        self.state->overlayDirty = true;
    } else if (error != EditError::EmptyUndo) {
        std::fprintf(stderr, "Undo failed (%s): %s\n", EditErrorName(error),
                     message.c_str());
    }
}

- (void)menuRedo:(id)sender {
    (void)sender;
    EditError error = EditError::None;
    std::string message;
    const bool projectHistory =
        (self.state->lastHistoryDomain == HistoryDomain::Project &&
         self.state->projectEditLog.UndoneCount() > 0) ||
        (self.state->editLog.UndoneCount() == 0 &&
         self.state->projectEditLog.UndoneCount() > 0);
    if (projectHistory) {
        Project candidate = self.state->project;
        ProjectEditLog projectLog = self.state->projectEditLog;
        if (!projectLog.Redo(candidate, error, message)) {
            if (error != EditError::EmptyRedo)
                std::fprintf(stderr, "Project redo failed (%s): %s\n",
                             EditErrorName(error), message.c_str());
            return;
        }
        if (![self commitProjectCandidate:candidate
                                  editLog:self.state->editLog
                               projectLog:projectLog
                                  message:message]) {
            std::fprintf(stderr, "Unable to persist project redo: %s\n",
                         message.c_str());
            return;
        }
        self.state->project = std::move(candidate);
        self.state->projectEditLog = std::move(projectLog);
        self.state->lastHistoryDomain = HistoryDomain::Project;
        [self rebuildMediaList];
        [self refreshAfterProjectMutation];
        return;
    }
    const RationalTime playhead = self.state->requestedPosition;
    if (self.state->editLog.Redo(self.state->document, error, message)) {
        [self refreshTimelineAfterEditFromPosition:playhead];
        [self refreshBinControlsSelecting:nil];
        if (![self persistEdits:message])
            std::fprintf(stderr, "Unable to persist redo: %s\n",
                         message.c_str());
        [self updateSelectionInfo];
        self.state->overlayDirty = true;
    } else if (error != EditError::EmptyRedo) {
        std::fprintf(stderr, "Redo failed (%s): %s\n", EditErrorName(error),
                     message.c_str());
    }
}

- (BOOL)hasValidTimelineRange {
    return self.state->timelineIn && self.state->timelineOut &&
           *self.state->timelineOut > *self.state->timelineIn;
}

- (BOOL)deleteTimelineRangeRipple:(BOOL)ripple {
    if (![self hasValidTimelineRange]) return NO;
    std::optional<DeleteGapOperation> rangeOperation =
        TimelineRangeDeleteOperation(
            self.state->document, *self.state->timelineIn,
            *self.state->timelineOut, ripple,
            std::vector<Ulid>(self.state->targetedTrackIds.begin(),
                              self.state->targetedTrackIds.end()));
    if (!rangeOperation) return NO;
    EditError error = EditError::None;
    std::string message;
    const RationalTime playhead = *self.state->timelineIn;
    if (!self.state->editLog.Apply(self.state->document,
                                   Operation{std::move(*rangeOperation)}, error,
                                   message)) {
        std::fprintf(stderr, "Range delete rejected (%s): %s\n",
                     EditErrorName(error), message.c_str());
        return NO;
    }
    self.state->timelineIn.reset();
    self.state->timelineOut.reset();
    self.state->interaction->SelectClip("");
    [self refreshTimelineAfterEditFromPosition:playhead];
    [self persistEdits:message];
    self.infoLabel.stringValue = ripple ? @"Zone In/Out extraite avec ripple"
                                        : @"Zone In/Out effacée (gap conservé)";
    self.state->overlayDirty = true;
    return YES;
}

- (BOOL)deleteCurrentTimelineSelectionRipple:(BOOL)ripple {
    if ([self hasValidTimelineRange])
        return [self deleteTimelineRangeRipple:ripple];
    const auto gap = self.state->interaction->SelectedGap();
    const std::vector<Ulid> ids = self.state->interaction->SelectedClipIds();
    if (!gap && ids.empty()) return NO;
    Operation operation =
        gap ? Operation{GapDeleteOperationForSelection(
                  self.state->document, *gap, self.state->linkedSelection)}
            : (ripple ? Operation{RemoveClipOperation{ids.front(), {}}}
                      : Operation{ClearClipOperation{ids.front(), {}}});
    if (!gap && self.state->linkedSelection && ids.size() > 1) {
        const DocumentClip* first = self.state->document.FindClip(ids.front());
        if (first && !first->link_group_id.empty())
            operation = ripple ? Operation{RemoveLinkedClipsOperation{
                                     first->link_group_id, ids, {}}}
                               : Operation{ClearLinkedClipsOperation{
                                     first->link_group_id, ids, {}}};
    }
    EditError error = EditError::None;
    std::string message;
    const RationalTime playhead = self.state->requestedPosition;
    if (!self.state->editLog.Apply(self.state->document, std::move(operation),
                                   error, message))
        return NO;
    self.state->interaction->SelectClip("");
    [self refreshTimelineAfterEditFromPosition:playhead];
    [self persistEdits:message];
    [self updateSelectionInfo];
    return YES;
}

- (BOOL)deleteCurrentTimelineSelection {
    return [self deleteCurrentTimelineSelectionRipple:NO];
}

- (void)menuDeleteSelection:(id)sender {
    (void)sender;
    if (self.window.firstResponder == self.binOutline)
        [self deleteBinPressed:nil];
    else
        [self deleteCurrentTimelineSelection];
}

- (void)menuRippleDeleteSelection:(id)sender {
    (void)sender;
    [self deleteCurrentTimelineSelectionRipple:YES];
}

- (void)menuMarkTimelineIn:(id)sender {
    (void)sender;
    if (self.state->sourceMonitorActive && self.state->sourceMonitor) {
        self.state->sourceIn = self.state->sourceMonitorPosition;
        [self updateSourceZoneLabel];
        const DocumentSource* source =
            self.state->document.FindSource(self.state->sourceMonitorId);
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"SOURCE IN  %@",
                                       TimelineTimecode(
                                           *self.state->sourceIn,
                                           source ? source->rate
                                                  : [self playheadFrameRate])];
        self.state->overlayDirty = true;
        return;
    }
    self.state->timelineIn = self.state->requestedPosition;
    self.infoLabel.stringValue = [NSString
        stringWithFormat:@"IN  %@", TimelineTimecode(*self.state->timelineIn,
                                                     [self playheadFrameRate])];
    self.state->overlayDirty = true;
}

- (void)menuMarkTimelineOut:(id)sender {
    (void)sender;
    if (self.state->sourceMonitorActive && self.state->sourceMonitor) {
        const DocumentSource* source =
            self.state->document.FindSource(self.state->sourceMonitorId);
        self.state->sourceOut = self.state->sourceMonitorPosition;
        if (source) {
            self.state->sourceOut = self.state->sourceOut->add(
                {source->rate.den, source->rate.num});
            if (*self.state->sourceOut > source->duration)
                self.state->sourceOut = source->duration;
        }
        [self updateSourceZoneLabel];
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"SOURCE OUT %@",
                                       TimelineTimecode(
                                           *self.state->sourceOut,
                                           source ? source->rate
                                                  : [self playheadFrameRate])];
        self.state->overlayDirty = true;
        return;
    }
    self.state->timelineOut = self.state->requestedPosition;
    self.infoLabel.stringValue = [NSString
        stringWithFormat:@"OUT %@", TimelineTimecode(*self.state->timelineOut,
                                                     [self playheadFrameRate])];
    self.state->overlayDirty = true;
}

- (void)menuClearTimelineRange:(id)sender {
    (void)sender;
    if (self.state->sourceMonitorActive && self.state->sourceMonitor) {
        self.state->sourceIn.reset();
        self.state->sourceOut.reset();
        [self updateSourceZoneLabel];
        self.infoLabel.stringValue = @"Points Source In/Out effacés";
        self.state->overlayDirty = true;
        return;
    }
    self.state->timelineIn.reset();
    self.state->timelineOut.reset();
    self.infoLabel.stringValue = @"Points In/Out effacés";
    self.state->overlayDirty = true;
}

- (BOOL)performSourceEditInsert:(BOOL)insert {
    const DocumentSource* source =
        self.state->document.FindSource(self.state->sourceMonitorId);
    if (!self.state->sourceMonitor || !source) {
        self.infoLabel.stringValue = @"Chargez d’abord un rush dans SOURCE";
        return NO;
    }
    const DocumentTrack* target = nullptr;
    for (const DocumentTrack& track : self.state->document.sequence.tracks) {
        if (track.kind != "video" || track.locked ||
            !self.state->targetedTrackIds.count(track.id))
            continue;
        if (!target || track.index < target->index) target = &track;
    }
    if (!target) {
        self.infoLabel.stringValue =
            @"Ciblez au moins une piste vidéo déverrouillée";
        return NO;
    }
    const LibraryMedia* media = nullptr;
    const auto probed = self.state->mediaMetadata.find(source->id);
    if (probed != self.state->mediaMetadata.end())
        media = &probed->second;
    else
        media = self.state->document.FindLibraryMedia(source->id);
    const bool sourceHasAudio =
        media && media->metadata_complete && media->has_audio;
    const DocumentTrack* audioTarget = nullptr;
    if (sourceHasAudio) {
        for (const DocumentTrack& track :
             self.state->document.sequence.tracks) {
            if (track.kind != "audio" || track.locked ||
                !self.state->targetedTrackIds.count(track.id))
                continue;
            if (!audioTarget || track.index < audioTarget->index)
                audioTarget = &track;
        }
        if (!audioTarget) {
            for (const DocumentTrack& track :
                 self.state->document.sequence.tracks) {
                if (track.kind != "audio" || track.locked) continue;
                if (!audioTarget || track.index < audioTarget->index)
                    audioTarget = &track;
            }
        }
    }

    RationalTime sourceIn =
        self.state->sourceIn.value_or(RationalTime{0, source->duration.rate});
    RationalTime sourceOut = self.state->sourceOut.value_or(source->duration);
    RationalTime timelineIn =
        self.state->timelineIn.value_or(self.state->requestedPosition);
    if (self.state->timelineIn && self.state->timelineOut &&
        *self.state->timelineOut > *self.state->timelineIn) {
        const RationalTime recordDuration =
            self.state->timelineOut->sub(*self.state->timelineIn);
        timelineIn = *self.state->timelineIn;
        if (self.state->sourceIn && !self.state->sourceOut)
            sourceOut = sourceIn.add(recordDuration);
        else if (!self.state->sourceIn && self.state->sourceOut)
            sourceIn = sourceOut.sub(recordDuration);
        else if (!self.state->sourceIn && !self.state->sourceOut)
            sourceOut = sourceIn.add(recordDuration);
        else if (sourceOut.sub(sourceIn) != recordDuration) {
            self.infoLabel.stringValue = @"Montage 4 points ambigu : les "
                                         @"durées Source et Record diffèrent";
            return NO;
        }
    } else if (!self.state->timelineIn && self.state->timelineOut) {
        timelineIn = self.state->timelineOut->sub(sourceOut.sub(sourceIn));
    }
    Document stagedDocument = self.state->document;
    EditLog stagedLog = self.state->editLog;
    EditError error = EditError::None;
    std::string message;
    Ulid audioTargetId = audioTarget ? audioTarget->id : Ulid{};
    if (sourceHasAudio && audioTargetId.empty()) {
        int32_t index = 0;
        for (const DocumentTrack& track : stagedDocument.sequence.tracks)
            index = std::max(index, track.index + 1);
        audioTargetId = GenerateUlid();
        if (!stagedLog.Apply(
                stagedDocument,
                Operation{AddTrackOperation{audioTargetId, "audio", index}},
                error, message)) {
            self.infoLabel.stringValue = [NSString
                stringWithFormat:@"Création audio refusée (%s) : %s",
                                 EditErrorName(error), message.c_str()];
            return NO;
        }
    }
    std::optional<DeleteGapOperation> operation = TimelineSourceEditOperation(
        stagedDocument, self.state->sourceMonitorId, sourceIn, sourceOut,
        timelineIn, target->id, insert,
        audioTargetId.empty() ? std::vector<Ulid>{}
                              : std::vector<Ulid>{audioTargetId});
    if (!operation) {
        self.infoLabel.stringValue =
            @"Zone Source/Record invalide pour ce montage";
        return NO;
    }
    if (!stagedLog.Apply(stagedDocument, Operation{std::move(*operation)},
                         error, message)) {
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"Montage refusé (%s) : %s",
                                       EditErrorName(error), message.c_str()];
        return NO;
    }
    const RationalTime next = timelineIn.add(sourceOut.sub(sourceIn));
    if (![self persistStagedDocument:stagedDocument
                             editLog:stagedLog
                             message:message]) {
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"Enregistrement impossible : %s",
                                       message.c_str()];
        return NO;
    }
    self.state->document = std::move(stagedDocument);
    self.state->editLog = std::move(stagedLog);
    if (!audioTargetId.empty()) {
        self.state->targetedTrackIds.insert(audioTargetId);
        self.state->timelineTargetTracks[self.state->activeTimelineId] =
            self.state->targetedTrackIds;
    }
    [self refreshTimelineAfterEditFromPosition:next];
    self.infoLabel.stringValue = [NSString
        stringWithFormat:@"Rush %@ depuis SOURCE%@",
                         insert ? @"inséré" : @"écrasé",
                         !audioTargetId.empty() ? @" · audio séparé et lié"
                                                : @""];
    self.state->overlayDirty = true;
    return YES;
}

- (void)menuInsertSource:(id)sender {
    (void)sender;
    [self performSourceEditInsert:YES];
}

- (void)menuOverwriteSource:(id)sender {
    (void)sender;
    [self performSourceEditInsert:NO];
}

- (void)menuCutSelectedAtPlayhead:(id)sender {
    (void)sender;
    const Ulid clipId = self.state->interaction->SelectedClipId();
    if (clipId.empty()) return;
    self.state->contextClipId = clipId;
    self.state->contextTime = self.state->requestedPosition;
    [self contextCutClip:nil];
}

- (void)menuAddCrossDissolve:(id)sender {
    (void)sender;
    const DocumentTrack* chosenTrack = nullptr;
    const DocumentClip* left = nullptr;
    const DocumentClip* right = nullptr;
    const Ulid selected = self.state->interaction->SelectedClipId();
    const auto choosePair = [&](const DocumentTrack& track, size_t index) {
        if (track.kind != "video" || index + 1 >= track.clips.size())
            return false;
        const DocumentClip& candidateLeft = track.clips[index];
        const DocumentClip& candidateRight = track.clips[index + 1];
        if (candidateLeft.timeline_in.add(candidateLeft.duration) !=
            candidateRight.timeline_in)
            return false;
        chosenTrack = &track;
        left = &candidateLeft;
        right = &candidateRight;
        return true;
    };
    if (!selected.empty()) {
        for (const DocumentTrack& track :
             self.state->document.sequence.tracks) {
            for (size_t index = 0; index < track.clips.size(); ++index) {
                if (track.clips[index].id != selected) continue;
                if (!choosePair(track, index) && index > 0)
                    choosePair(track, index - 1);
                break;
            }
            if (left) break;
        }
    }
    if (!left) {
        for (const DocumentTrack& track :
             self.state->document.sequence.tracks) {
            for (size_t index = 0; index + 1 < track.clips.size(); ++index) {
                if (track.clips[index + 1].timeline_in ==
                        self.state->requestedPosition &&
                    choosePair(track, index))
                    break;
            }
            if (left) break;
        }
    }
    if (!left || !right || !chosenTrack) {
        self.infoLabel.stringValue = @"Placez le playhead sur un cut vidéo ou "
                                     @"sélectionnez un clip adjacent";
        return;
    }
    for (const DocumentTransition& existing :
         self.state->document.sequence.transitions) {
        if (existing.left_clip_id == left->id &&
            existing.right_clip_id == right->id) {
            self.infoLabel.stringValue = @"Ce cut possède déjà une transition";
            return;
        }
    }
    const DocumentSource* leftSource =
        self.state->document.FindSource(left->source_id);
    if (!leftSource) return;
    const MediaRate rate = self.state->document.sequence.frame_rate;
    const RationalTime tail =
        leftSource->duration.sub(left->source_in.add(left->duration));
    const int64_t leftHandle = tail.to_frames(rate.num, rate.den);
    const int64_t rightHandle = right->source_in.to_frames(rate.num, rate.den);
    const int64_t halfFrames = std::min<int64_t>({6, leftHandle, rightHandle});
    if (halfFrames < 1) {
        self.infoLabel.stringValue =
            @"Fondu impossible : il manque des poignées média de chaque côté";
        return;
    }
    DocumentTransition transition;
    transition.track_id = chosenTrack->id;
    transition.left_clip_id = left->id;
    transition.right_clip_id = right->id;
    transition.duration = {halfFrames * 2 * rate.den, rate.num};
    EditError error = EditError::None;
    std::string message;
    if (!self.state->editLog.Apply(
            self.state->document, Operation{AddTransitionOperation{transition}},
            error, message)) {
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"Transition refusée (%s) : %s",
                                       EditErrorName(error), message.c_str()];
        return;
    }
    [self refreshTimelineAfterEditFromPosition:self.state->requestedPosition];
    if (![self persistEdits:message])
        std::fprintf(stderr, "Unable to persist transition: %s\n",
                     message.c_str());
    self.infoLabel.stringValue =
        [NSString stringWithFormat:@"Fondu enchaîné · %lld images",
                                   static_cast<long long>(halfFrames * 2)];
    self.state->overlayDirty = true;
}

- (void)menuRemoveCrossDissolve:(id)sender {
    (void)sender;
    const Ulid selected = self.state->interaction->SelectedClipId();
    const DocumentTransition* chosen = nullptr;
    for (const DocumentTransition& transition :
         self.state->document.sequence.transitions) {
        const DocumentClip* right =
            self.state->document.FindClip(transition.right_clip_id);
        const bool selectedCut =
            !selected.empty() && (transition.left_clip_id == selected ||
                                  transition.right_clip_id == selected);
        const bool playheadCut =
            right && right->timeline_in == self.state->requestedPosition;
        if (selectedCut || playheadCut) {
            chosen = &transition;
            break;
        }
    }
    if (!chosen) {
        self.infoLabel.stringValue =
            @"Aucun fondu au raccord ou au clip sélectionné";
        return;
    }
    const Ulid transitionId = chosen->id;
    EditError error = EditError::None;
    std::string message;
    if (!self.state->editLog.Apply(
            self.state->document,
            Operation{RemoveTransitionOperation{transitionId}}, error,
            message)) {
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"Suppression refusée (%s) : %s",
                                       EditErrorName(error), message.c_str()];
        return;
    }
    [self refreshTimelineAfterEditFromPosition:self.state->requestedPosition];
    if (![self persistEdits:message])
        std::fprintf(stderr, "Unable to persist transition removal: %s\n",
                     message.c_str());
    self.infoLabel.stringValue = @"Fondu enchaîné supprimé";
    self.state->overlayDirty = true;
}

- (void)menuSelectTool:(id)sender {
    (void)sender;
    [self setTimelineTool:TimelineTool::Select];
}
- (void)menuHandTool:(id)sender {
    (void)sender;
    [self setTimelineTool:TimelineTool::Hand];
}
- (void)menuZoomTool:(id)sender {
    (void)sender;
    [self setTimelineTool:TimelineTool::Zoom];
}
- (void)menuCutTool:(id)sender {
    (void)sender;
    [self setTimelineTool:TimelineTool::Cut];
}
- (void)menuSlipTool:(id)sender {
    (void)sender;
    [self setTimelineTool:TimelineTool::Slip];
}
- (void)menuToggleSnapping:(id)sender {
    (void)sender;
    self.state->interaction->SetSnappingEnabled(
        !self.state->interaction->SnappingEnabled());
    [self updateSelectionInfo];
    self.state->overlayDirty = true;
}
- (void)menuToggleLinkedSelection:(id)sender {
    (void)sender;
    self.linkedSelectionButton.state = self.state->linkedSelection
                                           ? NSControlStateValueOff
                                           : NSControlStateValueOn;
    [self linkedSelectionPressed:self.linkedSelectionButton];
}
- (void)menuFitTimeline:(id)sender {
    (void)sender;
    self.state->viewport.FitDuration(self.state->duration,
                                     self.metalView.bounds.size.width);
    self.state->overlayDirty = true;
}
- (void)menuAddVideoTrack:(id)sender {
    (void)sender;
    [self addTrack:NO];
}
- (void)menuAddAudioTrack:(id)sender {
    (void)sender;
    [self addTrack:YES];
}

- (void)removeContextTrackPressed:(id)sender {
    (void)sender;
    const DocumentTrack* track =
        self.state->document.FindTrack(self.state->contextTrackId);
    if (!track) return;
    if (!track->clips.empty()) {
        NSAlert* alert = [[NSAlert alloc] init];
        alert.alertStyle = NSAlertStyleWarning;
        alert.messageText = @"Supprimer cette piste et tous ses clips ?";
        alert.informativeText = [NSString
            stringWithFormat:@"La piste contient %lu clip%@. Cette action "
                             @"reste annulable avec ⌘Z.",
                             (unsigned long)track->clips.size(),
                             track->clips.size() == 1 ? @"" : @"s"];
        [alert addButtonWithTitle:@"Supprimer la piste"];
        [alert addButtonWithTitle:@"Annuler"];
        if ([alert runModal] != NSAlertFirstButtonReturn) return;
    }
    EditError error = EditError::None;
    std::string message;
    const RationalTime playhead = self.state->requestedPosition;
    if (!self.state->editLog.Apply(self.state->document,
                                   Operation{RemoveTrackOperation{track->id}},
                                   error, message)) {
        self.infoLabel.stringValue = [NSString
            stringWithFormat:@"Suppression refusée : %s", message.c_str()];
        return;
    }
    self.state->interaction->SelectClip("");
    [self refreshTimelineAfterEditFromPosition:playhead];
    if (![self persistEdits:message])
        std::fprintf(stderr, "Unable to persist track removal: %s\n",
                     message.c_str());
    [self updateSelectionInfo];
    self.state->overlayDirty = true;
}

- (void)removeEmptyTracksPressed:(id)sender {
    (void)sender;
    std::vector<Ulid> emptyTrackIds;
    for (const DocumentTrack& track : self.state->document.sequence.tracks)
        if (track.clips.empty()) emptyTrackIds.push_back(track.id);
    if (emptyTrackIds.empty()) return;

    Document stagedDocument = self.state->document;
    EditLog stagedLog = self.state->editLog;
    EditError error = EditError::None;
    std::string message;
    for (const Ulid& trackId : emptyTrackIds) {
        if (!stagedLog.Apply(stagedDocument,
                             Operation{RemoveTrackOperation{trackId}}, error,
                             message)) {
            self.infoLabel.stringValue = [NSString
                stringWithFormat:@"Nettoyage refusé : %s", message.c_str()];
            return;
        }
    }
    self.state->document = std::move(stagedDocument);
    self.state->editLog = std::move(stagedLog);
    const RationalTime playhead = self.state->requestedPosition;
    [self refreshTimelineAfterEditFromPosition:playhead];
    if (![self persistEdits:message])
        std::fprintf(stderr, "Unable to persist empty track removal: %s\n",
                     message.c_str());
    self.infoLabel.stringValue =
        [NSString stringWithFormat:@"%lu piste%@ vide%@ supprimée%@",
                                   (unsigned long)emptyTrackIds.size(),
                                   emptyTrackIds.size() == 1 ? @"" : @"s",
                                   emptyTrackIds.size() == 1 ? @"" : @"s",
                                   emptyTrackIds.size() == 1 ? @"" : @"s"];
    self.state->overlayDirty = true;
}
- (void)menuPlayPause:(id)sender {
    (void)sender;
    [self setPlaybackDirection:self.state->playbackDirection == 0 ? 1 : 0];
}
- (void)menuPlayReverse:(id)sender {
    (void)sender;
    [self setPlaybackDirection:-1];
}
- (void)menuStop:(id)sender {
    (void)sender;
    [self setPlaybackDirection:0];
}
- (void)menuPlayForward:(id)sender {
    (void)sender;
    [self setPlaybackDirection:1];
}

// F2.5 -- Transport panel actions. Frame-step reuses -stepPlayheadFrames:,
// the same helper the Left/Right-arrow shortcut calls. Play/pause reuses
// -menuPlayPause: directly (wired as the transport button's action in
// -applicationDidFinishLaunching:), so there is exactly one play/pause
// implementation, not two.
- (void)transportStepBack:(id)sender {
    (void)sender;
    [self stepPlayheadFrames:-1];
}
- (void)transportStepForward:(id)sender {
    (void)sender;
    [self stepPlayheadFrames:1];
}
- (void)transportScrubChanged:(NSSlider*)sender {
    const ScrubBarRange range{self.state->duration};
    const RationalTime target =
        range.FractionToTime(sender.doubleValue, [self playheadInputRate]);
    [self requestTimelinePosition:target];
}

- (NSMenu*)timelineMenuForEvent:(NSEvent*)event {
    const NSPoint viewPoint =
        [self.metalView convertPoint:event.locationInWindow fromView:nil];
    const double timelineY =
        self.metalView.bounds.size.height - viewPoint.y - [self videoHeight];
    self.state->contextClipId.clear();
    self.state->contextTrackId.clear();
    self.state->contextGap.reset();
    if (timelineY < 0.0 || timelineY > [self timelineHeight]) return nil;
    const RationalTime time =
        self.state->viewport.XToTime(viewPoint.x, self.state->duration.rate);
    self.state->contextTime = time;
    const auto hit =
        HitTestTimeline(self.state->document, self.state->viewport, viewPoint.x,
                        timelineY, self.metalView.bounds.size.width);
    NSMenu* menu = [[NSMenu alloc] initWithTitle:@"Timeline"];
    if (hit) {
        self.state->contextClipId = hit->clip_id;
        self.state->contextTrackId = hit->track_id;
        std::vector<Ulid> selection{hit->clip_id};
        if (self.state->linkedSelection)
            selection =
                ExpandLinkedClipSelection(self.state->document, selection);
        self.state->interaction->SelectClips(selection);
        [self updateSelectionInfo];
        [menu addItem:[self menuItem:@"Ouvrir dans le moniteur source"
                              action:@selector(contextOpenClipSource:)
                                 key:@""]];
        [menu addItem:[self menuItem:@"Retrouver dans la médiathèque"
                              action:@selector(contextFindClipInMediaPool:)
                                 key:@""]];
        [menu addItem:NSMenuItem.separatorItem];
        [menu addItem:[self menuItem:@"Couper ici"
                              action:@selector(contextCutClip:)
                                 key:@""]];
        [menu addItem:[self menuItem:@"Supprimer"
                              action:@selector(contextDeleteSelection:)
                                 key:@""]];
        [menu addItem:NSMenuItem.separatorItem];
        [menu addItem:[self menuItem:@"Supprimer la piste"
                              action:@selector(removeContextTrackPressed:)
                                 key:@""]];
        [menu addItem:[self menuItem:@"Supprimer les pistes vides"
                              action:@selector(removeEmptyTracksPressed:)
                                 key:@""]];
        return menu;
    }
    const auto gap = HitTestTimelineGap(
        self.state->document, self.state->viewport, viewPoint.x, timelineY,
        self.metalView.bounds.size.width, self.state->duration.rate);
    if (gap) {
        self.state->contextGap = gap;
        self.state->contextTrackId = gap->track_id;
        [menu addItem:[self menuItem:@"Fermer le gap"
                              action:@selector(contextCloseGap:)
                                 key:@""]];
        [menu addItem:NSMenuItem.separatorItem];
        [menu addItem:[self menuItem:@"Supprimer la piste"
                              action:@selector(removeContextTrackPressed:)
                                 key:@""]];
        [menu addItem:[self menuItem:@"Supprimer les pistes vides"
                              action:@selector(removeEmptyTracksPressed:)
                                 key:@""]];
        return menu;
    }
    const auto tracks = TimelineTracksInDisplayOrder(self.state->document);
    const NSInteger index = static_cast<NSInteger>(
        (timelineY - kTimelineRulerHeight) / self.state->viewport.track_height);
    if (index >= 0 && index < (NSInteger)tracks.size())
        self.state->contextTrackId = tracks[index]->id;
    if (!self.state->contextTrackId.empty()) {
        [menu addItem:[self menuItem:@"Supprimer la piste"
                              action:@selector(removeContextTrackPressed:)
                                 key:@""]];
        [menu addItem:[self menuItem:@"Supprimer les pistes vides"
                              action:@selector(removeEmptyTracksPressed:)
                                 key:@""]];
        [menu addItem:NSMenuItem.separatorItem];
    }
    [menu addItem:[self menuItem:@"Ajouter une piste vidéo"
                          action:@selector(menuAddVideoTrack:)
                             key:@""]];
    [menu addItem:[self menuItem:@"Ajouter une piste audio"
                          action:@selector(menuAddAudioTrack:)
                             key:@""]];
    [menu addItem:NSMenuItem.separatorItem];
    [menu addItem:[self menuItem:@"Cadrer toute la timeline"
                          action:@selector(menuFitTimeline:)
                             key:@""]];
    return menu;
}

- (void)contextOpenClipSource:(id)sender {
    (void)sender;
    const DocumentClip* clip =
        self.state->document.FindClip(self.state->contextClipId);
    if (clip)
        [self openMediaIdentifierInSourceMonitor:
                  [NSString stringWithUTF8String:clip->source_id.c_str()]];
}

- (void)contextFindClipInMediaPool:(id)sender {
    (void)sender;
    const DocumentClip* clip =
        self.state->document.FindClip(self.state->contextClipId);
    if (!clip) return;
    self.mediaSearchField.stringValue = @"";
    [self refreshBinControlsSelecting:@"__all__"];
    NSString* source = [NSString stringWithUTF8String:clip->source_id.c_str()];
    const NSUInteger row = [self.visibleMediaIds indexOfObject:source];
    if (row != NSNotFound) {
        self.mediaViewToggle.selectedSegment = 0;
        [self mediaViewChanged:nil];
        [self.mediaTable selectRowIndexes:[NSIndexSet indexSetWithIndex:row]
                     byExtendingSelection:NO];
        [self.mediaTable scrollRowToVisible:row];
        [self.window makeFirstResponder:self.mediaTable];
    }
}

- (void)contextCutClip:(id)sender {
    (void)sender;
    const DocumentClip* clip =
        self.state->document.FindClip(self.state->contextClipId);
    if (!clip || self.state->contextTime <= clip->timeline_in ||
        self.state->contextTime >= clip->timeline_in.add(clip->duration))
        return;
    Operation operation = TimelineCutOperationForClip(
        self.state->document, *clip, self.state->contextTime,
        self.state->linkedSelection);
    EditError error = EditError::None;
    std::string message;
    const RationalTime playhead = self.state->requestedPosition;
    if (self.state->editLog.Apply(self.state->document, std::move(operation),
                                  error, message)) {
        [self refreshTimelineAfterEditFromPosition:playhead];
        [self persistEdits:message];
        [self updateSelectionInfo];
    }
}

- (void)contextDeleteSelection:(id)sender {
    (void)sender;
    [self deleteCurrentTimelineSelection];
}

- (void)contextCloseGap:(id)sender {
    (void)sender;
    if (!self.state->contextGap) return;
    const TimelineGapSelection gap = *self.state->contextGap;
    self.state->interaction->SelectClip("");
    Operation operation = GapDeleteOperationForSelection(
        self.state->document, gap, self.state->linkedSelection);
    EditError error = EditError::None;
    std::string message;
    const RationalTime playhead = self.state->requestedPosition;
    if (self.state->editLog.Apply(self.state->document, std::move(operation),
                                  error, message)) {
        [self refreshTimelineAfterEditFromPosition:playhead];
        [self persistEdits:message];
        [self updateSelectionInfo];
    }
}

- (void)advancePlayback {
    if (self.state->playbackDirection == 0) return;
    const auto now = std::chrono::steady_clock::now();
    const int64_t elapsedUs =
        std::chrono::duration_cast<std::chrono::microseconds>(
            now - self.state->playbackStarted)
            .count();
    if (elapsedUs <= 0) return;
    const RationalTime delta{
        elapsedUs * static_cast<int64_t>(self.state->playbackDirection),
        1000000};
    const RationalTime next = self.state->playbackAnchor.add(delta);
    if (next <= RationalTime{0, 1}) {
        [self requestTimelinePosition:{0, self.state->duration.rate}];
        [self setPlaybackDirection:0];
    } else if (next >= self.state->duration) {
        [self requestTimelinePosition:self.state->duration];
        [self setPlaybackDirection:0];
    } else {
        [self requestTimelinePosition:next];
    }
}

- (void)displayTick:(NSTimer*)timer {
    (void)timer;
    [self advancePlayback];
    [self processCompletedMediaImports];
    [self processCompletedMediaProxies];
    [self processCompletedMediaWaveforms];
    [self processCompletedMediaThumbnails];
    [self processCompletedMediaRelinks];
    [self processCompletedBatchRelinks];
    [self refreshMediaTaskStatus];
    [self presentNearestFrameAtDeadline:YES];
}

- (void)refreshMediaTaskStatus {
    if (!self.state || !self.state->mediaTasks || !self.mediaTaskLabel) return;
    const std::vector<MediaTaskSnapshot> tasks =
        self.state->mediaTasks->Snapshot();
    const MediaTaskSnapshot* shown = nullptr;
    for (auto item = tasks.rbegin(); item != tasks.rend(); ++item) {
        if (item->state == MediaTaskState::Queued ||
            item->state == MediaTaskState::Running) {
            shown = &*item;
            break;
        }
    }
    if (!shown && !tasks.empty()) shown = &tasks.back();
    if (!shown) {
        self.displayedMediaTaskId = nil;
        self.mediaTaskLabel.stringValue = @"Tâches média : prêt";
        self.mediaTaskProgress.doubleValue = 0.0;
        self.mediaTaskCancelButton.enabled = NO;
        return;
    }
    self.displayedMediaTaskId =
        [NSString stringWithUTF8String:shown->id.c_str()];
    NSString* state = @"en attente";
    switch (shown->state) {
        case MediaTaskState::Queued:
            state = @"en attente";
            break;
        case MediaTaskState::Running:
            state = @"en cours";
            break;
        case MediaTaskState::Succeeded:
            state = @"terminée";
            break;
        case MediaTaskState::Failed:
            state = @"échec";
            break;
        case MediaTaskState::Cancelled:
            state = @"annulée";
            break;
    }
    self.mediaTaskLabel.stringValue =
        [NSString stringWithFormat:@"%@ · %s · %s", state, shown->label.c_str(),
                                   shown->error.empty() ? shown->detail.c_str()
                                                        : shown->error.c_str()];
    self.mediaTaskProgress.doubleValue = shown->progress * 100.0;
    self.mediaTaskCancelButton.enabled =
        shown->state == MediaTaskState::Queued ||
        shown->state == MediaTaskState::Running;
}

- (void)cancelDisplayedMediaTask:(id)sender {
    (void)sender;
    if (!self.displayedMediaTaskId || !self.state->mediaTasks) return;
    self.state->mediaTasks->Cancel(self.displayedMediaTaskId.UTF8String ?: "");
    [self refreshMediaTaskStatus];
}

- (void)processCompletedMediaImports {
    if (!self.state || self.state->pendingImports.empty()) return;
    const std::vector<MediaTaskSnapshot> snapshots =
        self.state->mediaTasks->Snapshot();
    std::map<Ulid, MediaTaskSnapshot> byId;
    for (const MediaTaskSnapshot& task : snapshots) byId[task.id] = task;
    for (auto item = self.state->pendingImports.begin();
         item != self.state->pendingImports.end();) {
        const auto task = byId.find(item->first);
        if (task == byId.end() ||
            task->second.state == MediaTaskState::Queued ||
            task->second.state == MediaTaskState::Running) {
            ++item;
            continue;
        }
        if (task->second.state == MediaTaskState::Succeeded) {
            [self commitMediaImportBatch:item->second.get()];
        } else if (task->second.state == MediaTaskState::Cancelled) {
            self.binSummaryLabel.stringValue = @"Import média annulé.";
        } else {
            self.binSummaryLabel.stringValue =
                [NSString stringWithFormat:@"Import média échoué : %s",
                                           task->second.error.c_str()];
        }
        item = self.state->pendingImports.erase(item);
    }
}

- (void)processCompletedMediaProxies {
    if (!self.state || self.state->pendingProxies.empty()) return;
    const std::vector<MediaTaskSnapshot> snapshots =
        self.state->mediaTasks->Snapshot();
    std::map<Ulid, MediaTaskSnapshot> byId;
    for (const MediaTaskSnapshot& task : snapshots) byId[task.id] = task;
    bool reload = false;
    for (auto item = self.state->pendingProxies.begin();
         item != self.state->pendingProxies.end();) {
        const auto task = byId.find(item->first);
        if (task == byId.end() ||
            task->second.state == MediaTaskState::Queued ||
            task->second.state == MediaTaskState::Running) {
            ++item;
            continue;
        }
        if (task->second.state == MediaTaskState::Succeeded) {
            LibraryMedia* media =
                self.state->document.FindLibraryMedia(item->second.media_id);
            if (media) {
                const std::string previousProxyPath = media->proxy_path;
                media->proxy_path = item->second.stored_path;
                std::string message;
                if ([self persistEdits:message]) {
                    self.binSummaryLabel.stringValue =
                        [NSString stringWithFormat:@"Proxy prêt · %s",
                                                   media->filename.c_str()];
                    reload = true;
                } else {
                    media->proxy_path = previousProxyPath;
                    std::error_code ignored;
                    std::filesystem::remove(item->second.absolute_path,
                                            ignored);
                    self.binSummaryLabel.stringValue = [NSString
                        stringWithFormat:@"Proxy créé mais non attaché : %s",
                                         message.c_str()];
                }
            }
        } else if (task->second.state == MediaTaskState::Cancelled) {
            self.binSummaryLabel.stringValue = @"Génération proxy annulée.";
        } else {
            self.binSummaryLabel.stringValue =
                [NSString stringWithFormat:@"Échec du proxy : %s",
                                           task->second.error.c_str()];
        }
        item = self.state->pendingProxies.erase(item);
    }
    if (reload) {
        [self reloadDecodeWorkers];
        [self rebuildMediaList];
    }
}

- (void)loadOrEnqueueWaveformForMediaIdentifier:(NSString*)identifier {
    if (!identifier || !self.state) return;
    const Ulid mediaId(identifier.UTF8String ?: "");
    if (self.state->waveforms.count(mediaId) != 0) return;
    const std::filesystem::path projectPath = std::filesystem::absolute(
        std::filesystem::path(self.documentPath.UTF8String ?: ""));
    const std::filesystem::path output = projectPath.parent_path() /
                                         ".cutmachine" / "waveforms" /
                                         (mediaId + ".waveform");
    AudioWaveform waveform;
    std::string error;
    if (LoadAudioWaveform(output.string(), waveform, error)) {
        self.state->waveforms.emplace(mediaId, std::move(waveform));
        self.state->overlayDirty = true;
        return;
    }
    [self enqueueWaveformForMediaIdentifier:identifier];
}

- (void)enqueueWaveformForMediaIdentifier:(NSString*)identifier {
    if (!identifier || !self.state) return;
    const Ulid mediaId(identifier.UTF8String ?: "");
    const DocumentSource* source = self.state->document.FindSource(mediaId);
    if (!source) return;
    for (const auto& pending : self.state->pendingWaveforms)
        if (pending.second.media_id == mediaId) return;
    const std::filesystem::path projectPath = std::filesystem::absolute(
        std::filesystem::path(self.documentPath.UTF8String ?: ""));
    const std::filesystem::path base = projectPath.parent_path();
    std::filesystem::path input(source->path);
    if (input.is_relative()) input = base / input;
    const std::filesystem::path output =
        base / ".cutmachine" / "waveforms" / (mediaId + ".waveform");
    const LibraryMedia* media = self.state->document.FindLibraryMedia(mediaId);
    const std::string label =
        "Waveform " + (media ? media->filename : input.filename().string());
    WaveformSettings settings;
    const Ulid taskId = self.state->mediaTasks->Enqueue(
        MediaTaskKind::Waveform, label,
        [input = input.lexically_normal(), output, duration = source->duration,
         settings](MediaTaskContext& context, std::string& taskError) {
            return GenerateAudioWaveform(input.string(), output.string(),
                                         duration, settings, context,
                                         taskError);
        });
    self.state->pendingWaveforms.emplace(taskId,
                                         PendingWaveform{mediaId, output});
    [self refreshMediaTaskStatus];
}

- (void)processCompletedMediaWaveforms {
    if (!self.state || self.state->pendingWaveforms.empty()) return;
    const std::vector<MediaTaskSnapshot> snapshots =
        self.state->mediaTasks->Snapshot();
    std::map<Ulid, MediaTaskSnapshot> byId;
    for (const MediaTaskSnapshot& task : snapshots) byId[task.id] = task;
    for (auto item = self.state->pendingWaveforms.begin();
         item != self.state->pendingWaveforms.end();) {
        const auto task = byId.find(item->first);
        if (task == byId.end() ||
            task->second.state == MediaTaskState::Queued ||
            task->second.state == MediaTaskState::Running) {
            ++item;
            continue;
        }
        if (task->second.state == MediaTaskState::Succeeded) {
            AudioWaveform waveform;
            std::string error;
            if (LoadAudioWaveform(item->second.absolute_path.string(), waveform,
                                  error)) {
                self.state->waveforms[item->second.media_id] =
                    std::move(waveform);
                self.state->overlayDirty = true;
            } else {
                self.binSummaryLabel.stringValue = [NSString
                    stringWithFormat:@"Waveform illisible : %s", error.c_str()];
            }
        } else if (task->second.state == MediaTaskState::Failed) {
            self.binSummaryLabel.stringValue =
                [NSString stringWithFormat:@"Échec waveform : %s",
                                           task->second.error.c_str()];
        }
        item = self.state->pendingWaveforms.erase(item);
    }
}

- (void)loadOrEnqueueThumbnailForMediaIdentifier:(NSString*)identifier {
    if (!identifier || !self.state || self.mediaThumbnails[identifier]) return;
    const Ulid mediaId(identifier.UTF8String ?: "");
    const std::filesystem::path projectPath = std::filesystem::absolute(
        std::filesystem::path(self.documentPath.UTF8String ?: ""));
    const std::filesystem::path output = projectPath.parent_path() /
                                         ".cutmachine" / "thumbnails" /
                                         (mediaId + ".png");
    NSImage* image = [[NSImage alloc]
        initWithContentsOfFile:[NSString stringWithUTF8String:output.c_str()]];
    if (image) {
        self.mediaThumbnails[identifier] = image;
        [self.mediaCollection reloadData];
        return;
    }
    [self enqueueThumbnailForMediaIdentifier:identifier];
}

- (void)enqueueThumbnailForMediaIdentifier:(NSString*)identifier {
    if (!identifier || !self.state) return;
    const Ulid mediaId(identifier.UTF8String ?: "");
    const DocumentSource* source = self.state->document.FindSource(mediaId);
    if (!source) return;
    for (const auto& pending : self.state->pendingThumbnails)
        if (pending.second.media_id == mediaId) return;
    const std::filesystem::path projectPath = std::filesystem::absolute(
        std::filesystem::path(self.documentPath.UTF8String ?: ""));
    const std::filesystem::path base = projectPath.parent_path();
    std::filesystem::path input(source->path);
    if (input.is_relative()) input = base / input;
    const std::filesystem::path output =
        base / ".cutmachine" / "thumbnails" / (mediaId + ".png");
    const LibraryMedia* media = self.state->document.FindLibraryMedia(mediaId);
    const std::string label =
        "Thumbnail " + (media ? media->filename : input.filename().string());
    ThumbnailSettings settings;
    const Ulid taskId = self.state->mediaTasks->Enqueue(
        MediaTaskKind::Thumbnail, label,
        [input = input.lexically_normal(), output, duration = source->duration,
         settings](MediaTaskContext& context, std::string& taskError) {
            return GenerateMediaThumbnail(input.string(), output.string(),
                                          duration, settings, context,
                                          taskError);
        });
    self.state->pendingThumbnails.emplace(taskId,
                                          PendingThumbnail{mediaId, output});
    [self refreshMediaTaskStatus];
}

- (void)processCompletedMediaThumbnails {
    if (!self.state || self.state->pendingThumbnails.empty()) return;
    const std::vector<MediaTaskSnapshot> snapshots =
        self.state->mediaTasks->Snapshot();
    std::map<Ulid, MediaTaskSnapshot> byId;
    for (const MediaTaskSnapshot& task : snapshots) byId[task.id] = task;
    bool reload = false;
    for (auto item = self.state->pendingThumbnails.begin();
         item != self.state->pendingThumbnails.end();) {
        const auto task = byId.find(item->first);
        if (task == byId.end() ||
            task->second.state == MediaTaskState::Queued ||
            task->second.state == MediaTaskState::Running) {
            ++item;
            continue;
        }
        if (task->second.state == MediaTaskState::Succeeded) {
            NSString* identifier =
                [NSString stringWithUTF8String:item->second.media_id.c_str()];
            NSImage* image = [[NSImage alloc]
                initWithContentsOfFile:
                    [NSString stringWithUTF8String:item->second.absolute_path
                                                       .c_str()]];
            if (image) {
                self.mediaThumbnails[identifier] = image;
                reload = true;
            } else {
                self.binSummaryLabel.stringValue =
                    @"La vignette générée est illisible.";
            }
        } else if (task->second.state == MediaTaskState::Failed) {
            self.binSummaryLabel.stringValue =
                [NSString stringWithFormat:@"Échec vignette : %s",
                                           task->second.error.c_str()];
        }
        item = self.state->pendingThumbnails.erase(item);
    }
    if (reload) [self.mediaCollection reloadData];
}

- (void)processCompletedMediaRelinks {
    if (!self.state || self.state->pendingRelinks.empty()) return;
    const std::vector<MediaTaskSnapshot> snapshots =
        self.state->mediaTasks->Snapshot();
    std::map<Ulid, MediaTaskSnapshot> byId;
    for (const MediaTaskSnapshot& task : snapshots) byId[task.id] = task;
    for (auto item = self.state->pendingRelinks.begin();
         item != self.state->pendingRelinks.end();) {
        const auto task = byId.find(item->first);
        if (task == byId.end() ||
            task->second.state == MediaTaskState::Queued ||
            task->second.state == MediaTaskState::Running) {
            ++item;
            continue;
        }
        PendingRelink pending = item->second;
        item = self.state->pendingRelinks.erase(item);
        if (task->second.state != MediaTaskState::Succeeded) {
            if (task->second.state == MediaTaskState::Failed)
                self.binSummaryLabel.stringValue =
                    [NSString stringWithFormat:@"Reconnexion refusée : %s",
                                               task->second.error.c_str()];
            continue;
        }

        const LibraryMedia* previousMedia =
            self.state->document.FindLibraryMedia(pending.media_id);
        if (!previousMedia) continue;
        const LibraryMedia previous = *previousMedia;
        Project stagedProject = self.state->project;
        ProjectEditLog stagedProjectLog = self.state->projectEditLog;
        std::string message;
        EditError editError = EditError::None;
        ProjectOperation relink = RelinkProjectMediaOperation{
            {{pending.media_id, pending.result->media, pending.stored_path}},
            std::nullopt};
        if (!stagedProjectLog.Apply(stagedProject, std::move(relink), editError,
                                    message) ||
            ![self commitProjectCandidate:stagedProject
                                  editLog:self.state->editLog
                               projectLog:stagedProjectLog
                                  message:message]) {
            self.binSummaryLabel.stringValue = [NSString
                stringWithFormat:@"Reconnexion impossible (%s) : %s",
                                 EditErrorName(editError), message.c_str()];
            continue;
        }
        self.state->project = std::move(stagedProject);
        self.state->projectEditLog = std::move(stagedProjectLog);
        self.state->lastHistoryDomain = HistoryDomain::Project;
        self.state->document =
            self.state->project.MakeDocument(self.state->activeTimelineId);

        for (auto pendingTask = self.state->pendingProxies.begin();
             pendingTask != self.state->pendingProxies.end();) {
            if (pendingTask->second.media_id == pending.media_id) {
                self.state->mediaTasks->Cancel(pendingTask->first);
                pendingTask = self.state->pendingProxies.erase(pendingTask);
            } else {
                ++pendingTask;
            }
        }
        for (auto pendingTask = self.state->pendingWaveforms.begin();
             pendingTask != self.state->pendingWaveforms.end();) {
            if (pendingTask->second.media_id == pending.media_id) {
                self.state->mediaTasks->Cancel(pendingTask->first);
                pendingTask = self.state->pendingWaveforms.erase(pendingTask);
            } else {
                ++pendingTask;
            }
        }
        for (auto pendingTask = self.state->pendingThumbnails.begin();
             pendingTask != self.state->pendingThumbnails.end();) {
            if (pendingTask->second.media_id == pending.media_id) {
                self.state->mediaTasks->Cancel(pendingTask->first);
                pendingTask = self.state->pendingThumbnails.erase(pendingTask);
            } else {
                ++pendingTask;
            }
        }

        const std::filesystem::path projectPath = std::filesystem::absolute(
            std::filesystem::path(self.documentPath.UTF8String ?: ""));
        const std::filesystem::path base = projectPath.parent_path();
        const std::filesystem::path expectedProxy =
            base / ".cutmachine" / "proxies" / (pending.media_id + ".mov");
        std::filesystem::path oldProxy(previous.proxy_path);
        if (oldProxy.is_relative()) oldProxy = base / oldProxy;
        std::error_code ignored;
        if (!previous.proxy_path.empty() &&
            oldProxy.lexically_normal() == expectedProxy.lexically_normal())
            std::filesystem::remove(expectedProxy, ignored);
        std::filesystem::remove(base / ".cutmachine" / "waveforms" /
                                    (pending.media_id + ".waveform"),
                                ignored);
        std::filesystem::remove(
            base / ".cutmachine" / "thumbnails" / (pending.media_id + ".png"),
            ignored);
        self.state->waveforms.erase(pending.media_id);
        NSString* identifier =
            [NSString stringWithUTF8String:pending.media_id.c_str()];
        [self.mediaThumbnails removeObjectForKey:identifier];
        if (const LibraryMedia* media =
                self.state->document.FindLibraryMedia(pending.media_id))
            self.state->mediaMetadata[pending.media_id] = *media;

        [self reloadDecodeWorkers];
        auto audio = std::make_unique<AudioPlayback>();
        std::string audioError;
        const bool audioReady =
            audio->Open(self.state->document, base.string(), audioError);
        self.state->audioPlayback = std::move(audio);
        [self rebuildMediaList];
        if (self.state->sourceMonitorId == pending.media_id) {
            const LibraryMedia* media =
                self.state->document.FindLibraryMedia(pending.media_id);
            if (media)
                self.sourceMonitorTitleLabel.stringValue = [NSString
                    stringWithFormat:@"SOURCE — %s", media->filename.c_str()];
            self.sourceOfflineMediaLabel.hidden =
                self.state->workers.count(pending.media_id) != 0;
        }
        [self loadOrEnqueueThumbnailForMediaIdentifier:identifier];
        const LibraryMedia* replacement =
            self.state->document.FindLibraryMedia(pending.media_id);
        if (replacement && replacement->has_audio)
            [self loadOrEnqueueWaveformForMediaIdentifier:identifier];
        if (replacement) {
            std::string codec = replacement->codec;
            std::transform(
                codec.begin(), codec.end(), codec.begin(),
                [](unsigned char character) {
                    return static_cast<char>(std::tolower(character));
                });
            if (replacement->width > 1920 || codec == "hevc" ||
                codec == "h265" || codec == "av1")
                [self enqueueProxyForMediaIdentifier:identifier];
        }
        self.binSummaryLabel.stringValue =
            audioReady
                ? @"Média reconnecté · caches en cours de régénération"
                : [NSString stringWithFormat:
                                @"Média reconnecté · audio indisponible : %s",
                                audioError.c_str()];
    }
}

- (void)processCompletedBatchRelinks {
    if (!self.state || self.state->pendingBatchRelinks.empty()) return;
    const std::vector<MediaTaskSnapshot> snapshots =
        self.state->mediaTasks->Snapshot();
    std::map<Ulid, MediaTaskSnapshot> byId;
    for (const MediaTaskSnapshot& task : snapshots) byId[task.id] = task;
    for (auto item = self.state->pendingBatchRelinks.begin();
         item != self.state->pendingBatchRelinks.end();) {
        const auto task = byId.find(item->first);
        if (task == byId.end() ||
            task->second.state == MediaTaskState::Queued ||
            task->second.state == MediaTaskState::Running) {
            ++item;
            continue;
        }
        std::shared_ptr<BatchRelinkResult> result = item->second.result;
        item = self.state->pendingBatchRelinks.erase(item);
        if (task->second.state != MediaTaskState::Succeeded) {
            if (task->second.state == MediaTaskState::Failed)
                self.binSummaryLabel.stringValue =
                    [NSString stringWithFormat:@"Relink en lot échoué : %s",
                                               task->second.error.c_str()];
            continue;
        }
        if (result->replacements.empty()) {
            self.binSummaryLabel.stringValue = [NSString
                stringWithFormat:@"Aucun média reconnecté · %lu introuvable%@ "
                                 @"· %lu ambigu%@ · %lu incompatible%@",
                                 (unsigned long)result->unmatched,
                                 result->unmatched == 1 ? @"" : @"s",
                                 (unsigned long)result->ambiguous,
                                 result->ambiguous == 1 ? @"" : @"s",
                                 (unsigned long)result->incompatible,
                                 result->incompatible == 1 ? @"" : @"s"];
            continue;
        }

        std::map<Ulid, LibraryMedia> previousMedia;
        for (const RelinkReplacement& replacement : result->replacements) {
            const LibraryMedia* previous =
                self.state->document.FindLibraryMedia(replacement.media_id);
            if (previous) previousMedia[replacement.media_id] = *previous;
        }
        Project stagedProject = self.state->project;
        ProjectEditLog stagedProjectLog = self.state->projectEditLog;
        std::string message;
        std::vector<ProjectRelinkItem> projectReplacements;
        projectReplacements.reserve(result->replacements.size());
        for (const RelinkReplacement& replacement : result->replacements)
            projectReplacements.push_back({replacement.media_id,
                                           replacement.media,
                                           replacement.stored_path});
        EditError editError = EditError::None;
        ProjectOperation relink = RelinkProjectMediaOperation{
            std::move(projectReplacements), std::nullopt};
        if (!stagedProjectLog.Apply(stagedProject, std::move(relink), editError,
                                    message) ||
            ![self commitProjectCandidate:stagedProject
                                  editLog:self.state->editLog
                               projectLog:stagedProjectLog
                                  message:message]) {
            self.binSummaryLabel.stringValue = [NSString
                stringWithFormat:@"Relink en lot annulé (%s) : %s",
                                 EditErrorName(editError), message.c_str()];
            continue;
        }
        self.state->project = std::move(stagedProject);
        self.state->projectEditLog = std::move(stagedProjectLog);
        self.state->lastHistoryDomain = HistoryDomain::Project;
        self.state->document =
            self.state->project.MakeDocument(self.state->activeTimelineId);
        std::set<Ulid> relinkedIds;
        for (const RelinkReplacement& replacement : result->replacements)
            relinkedIds.insert(replacement.media_id);

        for (auto pendingTask = self.state->pendingProxies.begin();
             pendingTask != self.state->pendingProxies.end();) {
            if (relinkedIds.count(pendingTask->second.media_id)) {
                self.state->mediaTasks->Cancel(pendingTask->first);
                pendingTask = self.state->pendingProxies.erase(pendingTask);
            } else {
                ++pendingTask;
            }
        }
        for (auto pendingTask = self.state->pendingWaveforms.begin();
             pendingTask != self.state->pendingWaveforms.end();) {
            if (relinkedIds.count(pendingTask->second.media_id)) {
                self.state->mediaTasks->Cancel(pendingTask->first);
                pendingTask = self.state->pendingWaveforms.erase(pendingTask);
            } else {
                ++pendingTask;
            }
        }
        for (auto pendingTask = self.state->pendingThumbnails.begin();
             pendingTask != self.state->pendingThumbnails.end();) {
            if (relinkedIds.count(pendingTask->second.media_id)) {
                self.state->mediaTasks->Cancel(pendingTask->first);
                pendingTask = self.state->pendingThumbnails.erase(pendingTask);
            } else {
                ++pendingTask;
            }
        }

        const std::filesystem::path projectPath = std::filesystem::absolute(
            std::filesystem::path(self.documentPath.UTF8String ?: ""));
        const std::filesystem::path base = projectPath.parent_path();
        for (const Ulid& mediaId : relinkedIds) {
            const std::filesystem::path expectedProxy =
                base / ".cutmachine" / "proxies" / (mediaId + ".mov");
            const auto previous = previousMedia.find(mediaId);
            if (previous != previousMedia.end() &&
                !previous->second.proxy_path.empty()) {
                std::filesystem::path oldProxy(previous->second.proxy_path);
                if (oldProxy.is_relative()) oldProxy = base / oldProxy;
                std::error_code ignored;
                if (oldProxy.lexically_normal() ==
                    expectedProxy.lexically_normal())
                    std::filesystem::remove(expectedProxy, ignored);
            }
            std::error_code ignored;
            std::filesystem::remove(
                base / ".cutmachine" / "waveforms" / (mediaId + ".waveform"),
                ignored);
            std::filesystem::remove(
                base / ".cutmachine" / "thumbnails" / (mediaId + ".png"),
                ignored);
            self.state->waveforms.erase(mediaId);
            NSString* identifier =
                [NSString stringWithUTF8String:mediaId.c_str()];
            [self.mediaThumbnails removeObjectForKey:identifier];
            if (const LibraryMedia* media =
                    self.state->document.FindLibraryMedia(mediaId))
                self.state->mediaMetadata[mediaId] = *media;
        }

        [self reloadDecodeWorkers];
        auto audio = std::make_unique<AudioPlayback>();
        std::string audioError;
        const bool audioReady =
            audio->Open(self.state->document, base.string(), audioError);
        self.state->audioPlayback = std::move(audio);
        [self rebuildMediaList];
        for (const Ulid& mediaId : relinkedIds) {
            NSString* identifier =
                [NSString stringWithUTF8String:mediaId.c_str()];
            [self loadOrEnqueueThumbnailForMediaIdentifier:identifier];
            const LibraryMedia* media =
                self.state->document.FindLibraryMedia(mediaId);
            if (!media) continue;
            if (media->has_audio)
                [self loadOrEnqueueWaveformForMediaIdentifier:identifier];
            std::string codec = media->codec;
            std::transform(
                codec.begin(), codec.end(), codec.begin(),
                [](unsigned char character) {
                    return static_cast<char>(std::tolower(character));
                });
            if (media->width > 1920 || codec == "hevc" || codec == "h265" ||
                codec == "av1")
                [self enqueueProxyForMediaIdentifier:identifier];
            if (self.state->sourceMonitorId == mediaId) {
                self.sourceMonitorTitleLabel.stringValue = [NSString
                    stringWithFormat:@"SOURCE — %s", media->filename.c_str()];
                self.sourceOfflineMediaLabel.hidden =
                    self.state->workers.count(mediaId) != 0;
            }
        }
        self.binSummaryLabel.stringValue = [NSString
            stringWithFormat:@"%lu média%@ reconnecté%@ · %lu introuvable%@ · "
                             @"%lu ambigu%@ · %lu incompatible%@%@",
                             (unsigned long)relinkedIds.size(),
                             relinkedIds.size() == 1 ? @"" : @"s",
                             relinkedIds.size() == 1 ? @"" : @"s",
                             (unsigned long)result->unmatched,
                             result->unmatched == 1 ? @"" : @"s",
                             (unsigned long)result->ambiguous,
                             result->ambiguous == 1 ? @"" : @"s",
                             (unsigned long)result->incompatible,
                             result->incompatible == 1 ? @"" : @"s",
                             audioReady ? @"" : @" · audio indisponible"];
    }
}

- (void)refreshAfterProjectMutation {
    if (!self.state->project.FindTimeline(self.state->activeTimelineId))
        self.state->activeTimelineId = self.state->project.active_timeline_id;
    if (!self.state->project.FindTimeline(self.state->activeTimelineId) &&
        !self.state->project.timelines.empty())
        self.state->activeTimelineId = self.state->project.timelines.front().id;
    self.state->document =
        self.state->project.MakeDocument(self.state->activeTimelineId);
    const auto storedLog =
        self.state->timelineEditLogs.find(self.state->activeTimelineId);
    self.state->editLog = storedLog == self.state->timelineEditLogs.end()
                              ? EditLog{}
                              : storedLog->second;
    self.state->targetedTrackIds.clear();
    const auto storedTargets =
        self.state->timelineTargetTracks.find(self.state->activeTimelineId);
    if (storedTargets != self.state->timelineTargetTracks.end()) {
        for (const Ulid& id : storedTargets->second)
            if (self.state->document.FindTrack(id))
                self.state->targetedTrackIds.insert(id);
    }
    if (self.state->targetedTrackIds.empty())
        for (const DocumentTrack& track : self.state->document.sequence.tracks)
            self.state->targetedTrackIds.insert(track.id);
    self.state->timeline = std::make_unique<Timeline>(self.state->document);
    self.state->interaction = std::make_unique<TimelineInteraction>(
        self.state->document, self.state->editLog, self.state->viewport);
    self.state->interaction->SetLinkedSelectionEnabled(
        self.state->linkedSelection);
    [self reloadDecodeWorkers];
    [self refreshTimelineAfterEditFromPosition:RationalTime{
                                                   0,
                                                   self.state->document.sequence
                                                       .frame_rate.num}];
    [self refreshBinControlsSelecting:self.selectedBinId ?: @"__all__"];
    [self updateSelectionInfo];
    self.state->overlayDirty = true;
}

- (void)reloadDecodeWorkers {
    if (!self.state->frameCache) return;
    for (auto& worker : self.state->workers) worker.second->Stop();
    self.state->workers.clear();
    self.state->offlineSourceIds.clear();
    const std::filesystem::path projectPath = std::filesystem::absolute(
        std::filesystem::path(self.documentPath.UTF8String ?: ""));
    const std::filesystem::path base = projectPath.parent_path();
    for (const DocumentSource& source : self.state->document.sources) {
        const LibraryMedia* media =
            self.state->document.FindLibraryMedia(source.id);
        std::filesystem::path original(source.path);
        if (original.is_relative()) original = base / original;
        std::filesystem::path selected = original;
        if (self.state->proxiesEnabled && media && !media->proxy_path.empty()) {
            std::filesystem::path proxy(media->proxy_path);
            if (proxy.is_relative()) proxy = base / proxy;
            std::error_code existsError;
            if (std::filesystem::is_regular_file(proxy, existsError) &&
                !existsError)
                selected = proxy;
        }
        self.state->frameCache->ClearSource(source.id);
        const bool usingProxy = selected != original;
        auto worker =
            std::make_unique<DecodeWorker>(source.id, *self.state->frameCache,
                                           *self.state->performanceMetrics);
        if (!worker->Open(selected.lexically_normal().string(), 5)) {
            if (selected != original) {
                worker = std::make_unique<DecodeWorker>(
                    source.id, *self.state->frameCache,
                    *self.state->performanceMetrics);
            }
            if (selected == original ||
                !worker->Open(original.lexically_normal().string(), 5)) {
                self.state->offlineSourceIds.insert(source.id);
                continue;
            }
        }
        if (usingProxy && !DecodeWorkerMatchesSource(*worker, source)) {
            worker = std::make_unique<DecodeWorker>(
                source.id, *self.state->frameCache,
                *self.state->performanceMetrics);
            if (!worker->Open(original.lexically_normal().string(), 5)) {
                self.state->offlineSourceIds.insert(source.id);
                continue;
            }
        }
        if (!DecodeWorkerMatchesSource(*worker, source)) {
            std::fprintf(stderr,
                         "Source %s is incompatible with project metadata\n",
                         source.id.c_str());
            self.state->offlineSourceIds.insert(source.id);
            continue;
        }
        worker->Start();
        self.state->workers.emplace(source.id, std::move(worker));
    }
    self.state->rendered.clear();
    self.state->sourceRendered = {};
    [self requestResolvedPosition:self.state->requestedPosition];
    if (self.state->sourceMonitor)
        [self requestSourcePosition:self.state->sourceMonitorPosition];
    self.state->overlayDirty = true;
}

- (void)commitMediaImportBatch:(ProbedImportBatch*)batch {
    if (!batch) return;
    const Document before = self.state->document;
    const std::string targetBin =
        batch->target_bin.empty() ||
                self.state->document.FindBin(batch->target_bin)
            ? batch->target_bin
            : std::string{};
    std::map<std::string, size_t> known;
    for (size_t index = 0; index < self.state->document.library.size();
         ++index) {
        const LibraryMedia& media = self.state->document.library[index];
        std::filesystem::path path(media.path);
        if (path.is_relative())
            path = batch->document_path.parent_path() / path;
        std::error_code error;
        const auto canonical = std::filesystem::weakly_canonical(path, error);
        if (!error) known[canonical.string()] = index;
    }

    size_t added = 0;
    size_t moved = 0;
    size_t rejected = 0;
    std::vector<std::pair<Ulid, std::filesystem::path>> addedSources;
    for (const ProbedImportItem& item : batch->items) {
        if (!item.media) {
            ++rejected;
            continue;
        }
        const auto existing = known.find(item.absolute_path.string());
        if (existing != known.end()) {
            LibraryMedia& media =
                self.state->document.library[existing->second];
            if (media.bin_id != targetBin) {
                media.bin_id = targetBin;
                ++moved;
            }
            continue;
        }
        LibraryMedia media = *item.media;
        media.bin_id = targetBin;
        self.state->document.library.push_back(media);
        self.state->document.sources.push_back(
            {media.id, media.path, media.rate, media.duration});
        known[item.absolute_path.string()] =
            self.state->document.library.size() - 1;
        addedSources.push_back({media.id, item.absolute_path});
        ++added;
    }

    std::string message;
    if ((added || moved) && (!self.state->document.Validate(message) ||
                             ![self persistEdits:message])) {
        self.state->document = before;
        self.binSummaryLabel.stringValue = [NSString
            stringWithFormat:@"Import impossible : %s", message.c_str()];
        return;
    }
    for (const auto& source : addedSources) {
        if (const LibraryMedia* media =
                self.state->document.FindLibraryMedia(source.first))
            self.state->mediaMetadata[source.first] = *media;
        auto worker = std::make_unique<DecodeWorker>(
            source.first, *self.state->frameCache,
            *self.state->performanceMetrics);
        if (worker->Open(source.second.string(), 5)) {
            worker->Start();
            self.state->workers.emplace(source.first, std::move(worker));
        } else {
            self.state->offlineSourceIds.insert(source.first);
        }
    }
    if (!addedSources.empty()) {
        auto audio = std::make_unique<AudioPlayback>();
        std::string audioError;
        if (audio->Open(self.state->document,
                        batch->document_path.parent_path().string(),
                        audioError))
            self.state->audioPlayback = std::move(audio);
    }
    for (const auto& source : addedSources) {
        const LibraryMedia* media =
            self.state->document.FindLibraryMedia(source.first);
        if (!media) continue;
        std::string codec = media->codec;
        std::transform(codec.begin(), codec.end(), codec.begin(),
                       [](unsigned char character) {
                           return static_cast<char>(std::tolower(character));
                       });
        if (media->width > 1920 || codec == "hevc" || codec == "h265" ||
            codec == "av1")
            [self enqueueProxyForMediaIdentifier:
                      [NSString stringWithUTF8String:source.first.c_str()]];
        if (media->has_audio)
            [self loadOrEnqueueWaveformForMediaIdentifier:
                      [NSString stringWithUTF8String:source.first.c_str()]];
        [self loadOrEnqueueThumbnailForMediaIdentifier:
                  [NSString stringWithUTF8String:source.first.c_str()]];
    }
    NSString* selected =
        targetBin.empty() ? @"__root__"
                          : [NSString stringWithUTF8String:targetBin.c_str()];
    [self refreshBinControlsSelecting:selected];
    NSMutableArray<NSString*>* results = [NSMutableArray array];
    if (added)
        [results addObject:[NSString stringWithFormat:@"%lu rush%@ importé%@",
                                                      (unsigned long)added,
                                                      added == 1 ? @"" : @"es",
                                                      added == 1 ? @"" : @"s"]];
    if (moved)
        [results addObject:[NSString stringWithFormat:@"%lu rush%@ déplacé%@",
                                                      (unsigned long)moved,
                                                      moved == 1 ? @"" : @"es",
                                                      moved == 1 ? @"" : @"s"]];
    if (rejected)
        [results
            addObject:[NSString stringWithFormat:@"%lu refusé%@",
                                                 (unsigned long)rejected,
                                                 rejected == 1 ? @"" : @"s"]];
    if (results.count == 0) [results addObject:@"Aucun changement"];
    self.binSummaryLabel.stringValue =
        [results componentsJoinedByString:@" · "];
}

- (TimelineRenderData)timelineRenderData {
    TimelineRenderData data;
    data.color_management = self.state->document.color_management;
    data.sequence_width = self.state->document.sequence.width;
    data.sequence_height = self.state->document.sequence.height;
    const double width = self.metalView.bounds.size.width;
    const double timelineHeight = [self timelineHeight];
    data.video_height = [self videoHeight];
    const double top = data.video_height;
    auto add = [&](double x, double y, double w, double h, float r, float g,
                   float b, float a = 1.0f) {
        if (w > 0.0 && h > 0.0)
            data.rectangles.push_back({x, y, w, h, r, g, b, a});
    };
    const auto addTinyText = [&](double x, double y, const std::string& text) {
        const auto maskFor = [](char character) -> uint8_t {
            static constexpr uint8_t digits[] = {0x3f, 0x06, 0x5b, 0x4f, 0x66,
                                                 0x6d, 0x7d, 0x07, 0x7f, 0x6f};
            if (character >= '0' && character <= '9')
                return digits[character - '0'];
            if (character == 'f') return 0x71;
            if (character == 's') return 0x6d;
            if (character == 'm') return 0x37;
            if (character == 'p') return 0x73;
            if (character == '-') return 0x40;
            return 0;
        };
        double cursor = x;
        for (char character : text) {
            if (character == '+') {
                add(cursor + 1.0, y + 3.0, 3.0, 1.0, 1.0f, 0.93f, 0.84f);
                add(cursor + 2.0, y + 2.0, 1.0, 3.0, 1.0f, 0.93f, 0.84f);
                cursor += 6.0;
                continue;
            }
            const uint8_t mask = maskFor(character);
            const auto segment = [&](uint8_t bit, double sx, double sy,
                                     double sw, double sh) {
                if (mask & bit)
                    add(cursor + sx, y + sy, sw, sh, 1.0f, 0.93f, 0.84f);
            };
            segment(0x01, 1, 0, 3, 1);
            segment(0x02, 4, 1, 1, 2);
            segment(0x04, 4, 4, 1, 2);
            segment(0x08, 1, 6, 3, 1);
            segment(0x10, 0, 4, 1, 2);
            segment(0x20, 0, 1, 1, 2);
            segment(0x40, 1, 3, 3, 1);
            cursor += 6.0;
        }
    };

    const auto topmost = std::find_if(
        self.state->requested.rbegin(), self.state->requested.rend(),
        [](const ResolvedSlot& slot) { return slot.active; });
    if (topmost != self.state->requested.rend() &&
        self.state->offlineSourceIds.count(topmost->sourceId) != 0)
        add(0.0, 0.0, width, data.video_height, 0.025f, 0.025f, 0.025f);

    add(0.0, top - 2.0, width, 2.0, 0.025f, 0.027f, 0.030f);
    add(0.0, top, width, timelineHeight, 0.058f, 0.061f, 0.064f);
    add(0.0, top, width, kTimelineRulerHeight, 0.095f, 0.098f, 0.102f);
    constexpr double toolStart = 108.0;
    constexpr double toolWidth = 17.0;
    for (int index = 0; index < 4; ++index) {
        const bool active = static_cast<int>([self effectiveTool]) == index;
        const double x = toolStart + index * toolWidth;
        add(x, top + 4.0, toolWidth - 1.0, kTimelineRulerHeight - 8.0,
            active ? 0.16f : 0.075f, active ? 0.34f : 0.078f,
            active ? 0.46f : 0.082f);
    }
    if ([self hasValidTimelineRange]) {
        const double rawIn =
            self.state->viewport.TimeToX(*self.state->timelineIn);
        const double rawOut =
            self.state->viewport.TimeToX(*self.state->timelineOut);
        const double left = std::max(self.state->viewport.header_width, rawIn);
        const double right = std::min(width, rawOut);
        if (right > left) {
            add(left, top, right - left, kTimelineRulerHeight, 0.08f, 0.42f,
                0.62f, 0.62f);
            add(left, top + kTimelineRulerHeight, right - left,
                std::max(0.0, timelineHeight - kTimelineRulerHeight), 0.05f,
                0.28f, 0.42f, 0.12f);
        }
        if (rawIn >= self.state->viewport.header_width && rawIn <= width)
            add(rawIn, top, 2.0, timelineHeight, ui::theme::kAccentGreen.r,
                ui::theme::kAccentGreen.g, ui::theme::kAccentGreen.b);
        if (rawOut >= self.state->viewport.header_width && rawOut <= width)
            add(rawOut - 2.0, top, 2.0, timelineHeight,
                ui::theme::kAccentOrange.r, ui::theme::kAccentOrange.g,
                ui::theme::kAccentOrange.b);
    } else {
        if (self.state->timelineIn) {
            const double x =
                self.state->viewport.TimeToX(*self.state->timelineIn);
            if (x >= self.state->viewport.header_width && x <= width)
                add(x, top, 2.0, timelineHeight, ui::theme::kAccentGreen.r,
                    ui::theme::kAccentGreen.g, ui::theme::kAccentGreen.b);
        }
        if (self.state->timelineOut) {
            const double x =
                self.state->viewport.TimeToX(*self.state->timelineOut);
            if (x >= self.state->viewport.header_width && x <= width)
                add(x - 2.0, top, 2.0, timelineHeight,
                    ui::theme::kAccentOrange.r, ui::theme::kAccentOrange.g,
                    ui::theme::kAccentOrange.b);
        }
    }
    const auto tracks = TimelineTracksInDisplayOrder(self.state->document);
    bool sawVideoTrack = false;
    bool drewAudioDivider = false;
    for (size_t index = 0; index < tracks.size(); ++index) {
        const double y = top + kTimelineRulerHeight +
                         index * self.state->viewport.track_height;
        if (y >= top + timelineHeight) break;
        const bool video = tracks[index]->kind == "video";
        if (video) sawVideoTrack = true;
        if (!video && sawVideoTrack && !drewAudioDivider) {
            add(0.0, y - 2.0, width, 2.0, 0.19f, 0.20f, 0.21f);
            drewAudioDivider = true;
        }
        const float trackShade = index % 2 == 0 ? 0.070f : 0.078f;
        add(0.0, y, width, self.state->viewport.track_height, trackShade,
            trackShade + 0.002f, trackShade + 0.004f);
        add(0.0, y, self.state->viewport.header_width,
            self.state->viewport.track_height, 0.090f, 0.093f, 0.097f);
        {
            const ui::theme::Color kindTint = ui::theme::TrackTint(video);
            add(0.0, y, 3.0, self.state->viewport.track_height, kindTint.r,
                kindTint.g, kindTint.b);
        }
        // Resolve-like patch/target cells and compact track controls.
        add(50.0, y + 7.0, 24.0, 20.0, 0.070f, 0.073f, 0.077f);
        add(50.0, y + 7.0, 2.0, 20.0, video ? 0.18f : 0.20f,
            video ? 0.50f : 0.57f, video ? 0.73f : 0.34f);
        add(81.0, y + 10.0, 16.0, 14.0, 0.13f, 0.135f, 0.14f);
        add(104.0, y + 10.0, 16.0, 14.0, 0.13f, 0.135f, 0.14f);
        add(127.0, y + 10.0, 16.0, 14.0, 0.13f, 0.135f, 0.14f);
        add(150.0, y + 10.0, 16.0, 14.0, 0.13f, 0.135f, 0.14f);
        if (self.state->targetedTrackIds.count(tracks[index]->id)) {
            add(50.0, y + 7.0, 24.0, 20.0, video ? 0.10f : 0.08f,
                video ? 0.31f : 0.34f, video ? 0.48f : 0.20f);
            add(54.0, y + 16.0, 16.0, 2.0, video ? 0.48f : 0.42f,
                video ? 0.82f : 0.88f, video ? 1.00f : 0.58f);
        }
        if (tracks[index]->sync_lock) {
            add(127.0, y + 10.0, 16.0, 14.0, 0.10f, 0.25f, 0.31f);
            // Two joined links distinguish sync lock from the hard padlock.
            add(130.0, y + 15.0, 5.0, 2.0, 0.38f, 0.78f, 0.90f);
            add(135.0, y + 18.0, 5.0, 2.0, 0.38f, 0.78f, 0.90f);
            add(134.0, y + 16.0, 2.0, 3.0, 0.38f, 0.78f, 0.90f);
        }
        if (video) {
            // Eye and lock symbols.
            add(85.0, y + 16.0, 8.0, 2.0, 0.55f, 0.57f, 0.60f);
            add(88.0, y + 13.0, 2.0, 8.0, 0.55f, 0.57f, 0.60f);
            add(108.0, y + 16.0, 8.0, 6.0, 0.48f, 0.50f, 0.53f);
            add(110.0, y + 13.0, 4.0, 4.0, 0.48f, 0.50f, 0.53f);
        } else {
            // Solo / mute glyphs as minimal geometry.
            add(85.0, y + 14.0, 8.0, 2.0, 0.58f, 0.60f, 0.62f);
            add(85.0, y + 18.0, 8.0, 2.0, 0.58f, 0.60f, 0.62f);
            add(108.0, y + 14.0, 2.0, 7.0, 0.58f, 0.60f, 0.62f);
            add(114.0, y + 14.0, 2.0, 7.0, 0.58f, 0.60f, 0.62f);
            add(110.0, y + 14.0, 4.0, 2.0, 0.58f, 0.60f, 0.62f);
        }
        if (tracks[index]->locked) {
            add(104.0, y + 10.0, 16.0, 14.0, 0.34f, 0.12f, 0.10f);
            add(108.0, y + 16.0, 8.0, 6.0, 0.92f, 0.46f, 0.34f);
            add(110.0, y + 13.0, 4.0, 4.0, 0.92f, 0.46f, 0.34f);
            add(self.state->viewport.header_width, y,
                width - self.state->viewport.header_width,
                self.state->viewport.track_height, 0.02f, 0.02f, 0.02f, 0.28f);
        }
        add(0.0, y + self.state->viewport.track_height - 1.0, width, 1.0, 0.13f,
            0.132f, 0.135f);
    }
    const double addTrackY = top + kTimelineRulerHeight +
                             tracks.size() * self.state->viewport.track_height;
    if (addTrackY < top + timelineHeight) {
        add(0.0, addTrackY, width, kAddTrackRowHeight, 0.058f, 0.064f, 0.073f);
        add(0.0, addTrackY, self.state->viewport.header_width,
            kAddTrackRowHeight, 0.09f, 0.10f, 0.115f);
        const double split = self.state->viewport.header_width * 0.5;
        add(split, addTrackY, 1.0, kAddTrackRowHeight, 0.16f, 0.18f, 0.20f);
        for (int half = 0; half < 2; ++half) {
            const double centerX = split * (half + 0.5);
            const float red = half == 0 ? 0.30f : 0.36f;
            const float green = half == 0 ? 0.68f : 0.78f;
            const float blue = half == 0 ? 0.90f : 0.48f;
            add(centerX - 6.0, addTrackY + 11.0, 12.0, 2.0, red, green, blue);
            add(centerX - 1.0, addTrackY + 6.0, 2.0, 12.0, red, green, blue);
        }
    }
    add(self.state->viewport.header_width - 1.0, top, 1.0, timelineHeight,
        0.26f, 0.26f, 0.28f);
    const std::vector<double> tickXs = self.state->viewport.TickXs(width);
    for (size_t tickIndex = 0; tickIndex < tickXs.size(); ++tickIndex) {
        const double tickX = tickXs[tickIndex];
        add(tickX, top, 1.0, kTimelineRulerHeight, 0.38f, 0.39f, 0.41f);
        add(tickX, top + kTimelineRulerHeight - 5.0, 1.0, 5.0, 0.65f, 0.66f,
            0.70f);
        add(tickX, top + kTimelineRulerHeight, 1.0,
            std::max(0.0, timelineHeight - kTimelineRulerHeight), 0.17f, 0.175f,
            0.18f, 0.38f);
        if (tickIndex + 1 < tickXs.size()) {
            const double interval = tickXs[tickIndex + 1] - tickX;
            for (int subdivision = 1; subdivision < 4; ++subdivision) {
                add(tickX + interval * subdivision / 4.0,
                    top + kTimelineRulerHeight - 4.0, 1.0, 4.0, 0.30f, 0.32f,
                    0.35f);
            }
        }
    }

    if (const auto& gap = self.state->interaction->SelectedGap()) {
        const auto track = std::find_if(tracks.begin(), tracks.end(),
                                        [&](const DocumentTrack* value) {
                                            return value->id == gap->track_id;
                                        });
        if (track != tracks.end()) {
            const double rawLeft = self.state->viewport.TimeToX(gap->start);
            const double rawRight =
                self.state->viewport.TimeToX(gap->start.add(gap->duration));
            const double left = std::max(self.state->viewport.header_width,
                                         std::min(rawLeft, rawRight));
            const double right = std::min(width, std::max(rawLeft, rawRight));
            const double y = top + kTimelineRulerHeight +
                             std::distance(tracks.begin(), track) *
                                 self.state->viewport.track_height;
            if (right > left && y < top + timelineHeight) {
                const double height = self.state->viewport.track_height;
                add(left, y, right - left, height, 0.10f, 0.34f, 0.48f, 0.42f);
                add(left, y, right - left, 2.0, ui::theme::kAccentBlue.r,
                    ui::theme::kAccentBlue.g, ui::theme::kAccentBlue.b);
                add(left, y + height - 2.0, right - left, 2.0,
                    ui::theme::kAccentBlue.r, ui::theme::kAccentBlue.g,
                    ui::theme::kAccentBlue.b);
                add(left, y, 2.0, height, ui::theme::kAccentBlue.r,
                    ui::theme::kAccentBlue.g, ui::theme::kAccentBlue.b);
                add(right - 2.0, y, 2.0, height, ui::theme::kAccentBlue.r,
                    ui::theme::kAccentBlue.g, ui::theme::kAccentBlue.b);
                for (double stripe = left + 8.0; stripe < right; stripe += 12.0)
                    add(stripe, y + 5.0, 1.0, std::max(0.0, height - 10.0),
                        0.20f, 0.58f, 0.72f, 0.55f);
            }
        }
    }

    const auto clips =
        VisibleTimelineClips(self.state->document, self.state->viewport, width,
                             self.state->interaction->SelectedClipIds(),
                             self.state->interaction->TrimPreview(),
                             self.state->interaction->MovePreview());
    const bool singleClipSelection =
        self.state->interaction->SelectedClipIds().size() == 1;
    for (const TimelineClipRect& clip : clips) {
        double left = std::min(clip.x, clip.x + clip.width);
        double right = std::max(clip.x, clip.x + clip.width);
        const bool headVisible =
            left >= self.state->viewport.header_width && left <= width;
        const bool tailVisible =
            right >= self.state->viewport.header_width && right <= width;
        left = std::max(left, self.state->viewport.header_width);
        right = std::min(right, width);
        if (right <= left) continue;
        const double y = top + clip.y;
        if (y >= top + timelineHeight) continue;
        add(left + 1.0, y + 2.0, std::max(0.0, right - left), clip.height,
            0.015f, 0.018f, 0.022f, 0.65f);
        if (clip.moving) {
            add(left - 2.0, y - 2.0, right - left + 4.0, clip.height + 4.0,
                ui::theme::kAccentRed.r, ui::theme::kAccentRed.g,
                ui::theme::kAccentRed.b, 0.82f);
        }
        if (clip.selected) {
            add(left - 2.0, y - 2.0, right - left + 4.0, clip.height + 4.0,
                clip.valid ? ui::theme::kAccentAmber.r : 1.0f,
                clip.valid ? ui::theme::kAccentAmber.g : 0.16f,
                clip.valid ? ui::theme::kAccentAmber.b : 0.12f);
        }
        const auto color = ClipColor(clip.source_id, clip.audio);
        if (!clip.valid)
            add(left, y, right - left, clip.height, 0.72f, 0.08f, 0.08f, 0.92f);
        else
            add(left, y, right - left, clip.height,
                std::min(1.0f, color[0] + (clip.preview ? 0.12f : 0.0f)),
                std::min(1.0f, color[1] + (clip.preview ? 0.12f : 0.0f)),
                std::min(1.0f, color[2] + (clip.preview ? 0.12f : 0.0f)));
        add(left, y, right - left, 3.0, std::min(1.0f, color[0] + 0.22f),
            std::min(1.0f, color[1] + 0.22f), std::min(1.0f, color[2] + 0.22f));
        if (clip.audio && clip.width > 0.0) {
            const auto waveform = self.state->waveforms.find(clip.source_id);
            const DocumentClip* documentClip =
                self.state->document.FindClip(clip.clip_id);
            if (waveform != self.state->waveforms.end() && documentClip &&
                documentClip->source_in.rate > 0 &&
                documentClip->duration.rate > 0) {
                const AudioWaveform& data = waveform->second;
                const long double sourceStart =
                    static_cast<long double>(documentClip->source_in.value) /
                    documentClip->source_in.rate;
                const long double sourceDuration =
                    static_cast<long double>(documentClip->duration.value) /
                    documentClip->duration.rate;
                const double center = y + clip.height * 0.53;
                const double availableHeight = std::max(2.0, clip.height - 9.0);
                for (double x = std::floor(left) + 1.0; x < right; x += 2.0) {
                    const long double firstFraction = std::clamp(
                        static_cast<long double>((x - clip.x) / clip.width),
                        0.0L, 1.0L);
                    const long double lastFraction = std::clamp(
                        static_cast<long double>(
                            (std::min(right, x + 2.0) - clip.x) / clip.width),
                        0.0L, 1.0L);
                    size_t first = static_cast<size_t>(std::max<long double>(
                        0.0L, (sourceStart + sourceDuration * firstFraction) *
                                  data.peaks_per_second));
                    size_t last = static_cast<size_t>(std::max<long double>(
                        0.0L, (sourceStart + sourceDuration * lastFraction) *
                                  data.peaks_per_second));
                    first = std::min(first, data.peaks.size());
                    last =
                        std::min(std::max(last, first + 1), data.peaks.size());
                    if (first >= last) continue;
                    float peak = 0.0f;
                    for (size_t index = first; index < last; ++index)
                        peak = std::max(peak, data.peaks[index]);
                    const double barHeight = std::max(
                        1.0, static_cast<double>(peak) * availableHeight);
                    add(x, center - barHeight * 0.5, std::min(1.25, right - x),
                        barHeight, 0.64f, 0.94f, 0.79f, 0.82f);
                }
            }
        }
        if (clip.audio && clip.sync_drift && clip.sync_drift->value != 0) {
            const std::string label =
                SyncDriftLabel(*clip.sync_drift, [self playheadFrameRate]);
            const double badgeWidth = label.size() * 6.0 + 4.0;
            if (right - left >= badgeWidth + 6.0) {
                const double badgeX = left + 5.0;
                const double badgeY = y + 7.0;
                add(badgeX, badgeY, badgeWidth, 11.0, 0.66f, 0.12f, 0.07f,
                    0.94f);
                addTinyText(badgeX + 2.0, badgeY + 2.0, label);
            } else {
                add(left + 3.0, y + 7.0, 3.0, std::min(11.0, clip.height - 9.0),
                    0.94f, 0.18f, 0.08f);
            }
        }
        // Every edit boundary must remain readable when adjacent clips share
        // the same source color. Fixed-point dark edges create a clear cut
        // seam without depending on zoom or inserting a fake timeline gap.
        const double outlineWidth = std::min(1.0, right - left);
        add(left, y, outlineWidth, clip.height, 0.025f, 0.029f, 0.035f, 0.96f);
        add(right - outlineWidth, y, outlineWidth, clip.height, 0.025f, 0.029f,
            0.035f, 0.96f);
        add(left, y + clip.height - 1.0, right - left, 1.0, 0.025f, 0.029f,
            0.035f, 0.90f);
        if (clip.selected && singleClipSelection) {
            // Resolve-style trim handles: fixed point sizes, independent of
            // zoom, with a grip bar and short top/bottom caps.
            const double span = right - left;
            const double gripWidth = std::min(3.0, span / 2.0);
            const double capWidth = std::min(7.0, span);
            if (headVisible) {
                add(left, y, std::min(6.0, span), clip.height, 0.08f, 0.08f,
                    0.09f, 0.42f);
                add(left, y + 2.0, gripWidth, std::max(0.0, clip.height - 4.0),
                    1.0f, 0.82f, 0.18f);
                add(left, y, capWidth, 2.0, 1.0f, 0.82f, 0.18f);
                add(left, y + clip.height - 2.0, capWidth, 2.0, 1.0f, 0.82f,
                    0.18f);
            }
            if (tailVisible) {
                add(std::max(left, right - 6.0), y, std::min(6.0, span),
                    clip.height, 0.08f, 0.08f, 0.09f, 0.42f);
                add(right - gripWidth, y + 2.0, gripWidth,
                    std::max(0.0, clip.height - 4.0), 1.0f, 0.82f, 0.18f);
                add(right - capWidth, y, capWidth, 2.0, 1.0f, 0.82f, 0.18f);
                add(right - capWidth, y + clip.height - 2.0, capWidth, 2.0,
                    1.0f, 0.82f, 0.18f);
            }
        }
    }

    for (const DocumentTransition& transition :
         self.state->document.sequence.transitions) {
        const DocumentTrack* track =
            self.state->document.FindTrack(transition.track_id);
        const DocumentClip* rightClip =
            self.state->document.FindClip(transition.right_clip_id);
        if (!track || !rightClip) continue;
        const auto displayTrack =
            std::find(tracks.begin(), tracks.end(), track);
        if (displayTrack == tracks.end()) continue;
        const MediaRate rate = self.state->document.sequence.frame_rate;
        const int64_t frames =
            transition.duration.to_frames(rate.num, rate.den);
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
        const RationalTime pre{preFrames * rate.den, rate.num};
        const RationalTime post{postFrames * rate.den, rate.num};
        double leftX =
            self.state->viewport.TimeToX(rightClip->timeline_in.sub(pre));
        double rightX =
            self.state->viewport.TimeToX(rightClip->timeline_in.add(post));
        leftX = std::max(leftX, self.state->viewport.header_width);
        rightX = std::min(rightX, width);
        if (rightX <= leftX) continue;
        const double y = top + kTimelineRulerHeight +
                         std::distance(tracks.begin(), displayTrack) *
                             self.state->viewport.track_height +
                         4.0;
        const double height =
            std::max(8.0, self.state->viewport.track_height - 8.0);
        add(leftX, y, rightX - leftX, height, 0.16f, 0.42f, 0.58f, 0.62f);
        add(leftX, y, rightX - leftX, 2.0, 0.35f, 0.88f, 1.0f);
        add(leftX, y + height - 2.0, rightX - leftX, 2.0, 0.35f, 0.88f, 1.0f);
        const double cutX =
            self.state->viewport.TimeToX(rightClip->timeline_in);
        add(cutX - 1.0, y, 2.0, height, 0.75f, 0.96f, 1.0f);
    }

    if (self.state->lassoDragging) {
        const double left = std::max(
            self.state->viewport.header_width,
            std::min(self.state->lassoStartX, self.state->lassoCurrentX));
        const double right = std::min(
            width,
            std::max(self.state->lassoStartX, self.state->lassoCurrentX));
        const double lassoTop = std::max(
            kTimelineRulerHeight,
            std::min(self.state->lassoStartY, self.state->lassoCurrentY));
        const double bottom =
            std::max(self.state->lassoStartY, self.state->lassoCurrentY);
        if (right > left && bottom > lassoTop) {
            add(left, top + lassoTop, right - left, bottom - lassoTop, 0.12f,
                0.52f, 0.78f, 0.18f);
            add(left, top + lassoTop, right - left, 1.0, 0.28f, 0.78f, 1.0f);
            add(left, top + bottom - 1.0, right - left, 1.0, 0.28f, 0.78f,
                1.0f);
            add(left, top + lassoTop, 1.0, bottom - lassoTop, 0.28f, 0.78f,
                1.0f);
            add(right - 1.0, top + lassoTop, 1.0, bottom - lassoTop, 0.28f,
                0.78f, 1.0f);
        }
    }

    if (self.state->interaction->SnapGuideTime()) {
        const double snapX = self.state->viewport.TimeToX(
            *self.state->interaction->SnapGuideTime());
        if (snapX >= self.state->viewport.header_width && snapX <= width)
            add(snapX, top + kTimelineRulerHeight, 1.0,
                timelineHeight - kTimelineRulerHeight, 0.15f, 0.88f, 1.0f);
    }
    if (self.state->cutPreviewX && self.state->cutPreviewY) {
        add(*self.state->cutPreviewX - 1.0, top + *self.state->cutPreviewY, 2.0,
            self.state->viewport.track_height, 1.0f, 0.16f, 0.12f);
    }

    const double playheadX =
        self.state->viewport.TimeToX(self.state->requestedPosition);
    if (playheadX >= self.state->viewport.header_width && playheadX <= width) {
        add(playheadX - 4.0, top, 8.0, 6.0, 1.0f, 0.20f, 0.14f);
        add(playheadX - 1.0, top, 2.0, timelineHeight, 1.0f, 0.22f, 0.16f);
    }
    return data;
}

- (void)presentNearestFrameAtDeadline:(BOOL)isDisplayDeadline {
    if (!self.state->frameCache || !self.state->renderer ||
        !self.state->sourceRenderer || !self.state->programRenderer) {
        return;
    }

    std::vector<AVFrame*> frames(self.state->requested.size(), nullptr);
    std::vector<RenderedSlot> candidates(self.state->requested.size());
    bool missing = false;
    for (size_t slot = 0; slot < self.state->requested.size(); ++slot) {
        const ResolvedSlot& requested = self.state->requested[slot];
        if (!requested.active) {
            continue;
        }
        int64_t cachedFrame = -1;
        frames[slot] = self.state->frameCache->GetNearest(
            requested.sourceId, requested.frame, cachedFrame);
        candidates[slot] = {true, requested.sourceId, cachedFrame,
                            requested.opacity, requested.clipId};
        if (!frames[slot] || cachedFrame != requested.frame) {
            missing = true;
        }
    }
    if (isDisplayDeadline && missing) {
        self.state->performanceMetrics->RecordDrop();
    }
    const bool programChanged = candidates != self.state->rendered;
    if (self.state->overlayDirty || programChanged) {
        TimelineRenderData programData;
        programData.color_management = self.state->document.color_management;
        programData.sequence_width = self.state->document.sequence.width;
        programData.sequence_height = self.state->document.sequence.height;
        programData.video_height = self.programMonitorView.bounds.size.height;
        programData.video_zoom = self.state->programMonitorZoom;
        programData.video_rotation_degrees.resize(candidates.size(), 0);
        programData.video_opacities.resize(candidates.size(), 1.0f);
        // F1.3: read-only render-time consumer of DocumentClip::effects.
        // Resolving here (rather than in Renderer.mm) keeps the renderer
        // generic over Document/ClipEffect -- it only ever sees the already
        // -resolved, already-float ResolvedColorGrade.
        programData.video_color_grades.resize(candidates.size());
        for (size_t slot = 0; slot < candidates.size(); ++slot) {
            if (!candidates[slot].active) continue;
            programData.video_opacities[slot] = candidates[slot].opacity;
            if (const DocumentClip* clip =
                    self.state->document.FindClip(candidates[slot].clipId)) {
                programData.video_color_grades[slot] =
                    ResolveColorGrade(clip->effects);
            }
            const auto media =
                self.state->mediaMetadata.find(candidates[slot].sourceId);
            if (media != self.state->mediaMetadata.end() &&
                media->second.metadata_complete)
                programData.video_rotation_degrees[slot] =
                    media->second.rotation_degrees;
        }
        if (self.state->programRenderer->RenderFrames(frames, programData)) {
            self.state->rendered = candidates;
        }
        TimelineRenderData timelineData = [self timelineRenderData];
        const std::vector<AVFrame*> noFrames;
        self.state->renderer->RenderFrames(noFrames, timelineData);
    }

    if (self.state->sourceMonitorVisible && self.state->sourceMonitor &&
        !self.state->sourceMonitorId.empty()) {
        const int64_t requestedFrame =
            self.state->sourceMonitorPosition.to_frames(
                self.state->sourceMonitorPosition.rate);
        int64_t cachedFrame = -1;
        AVFrame* sourceFrame = self.state->frameCache->GetNearest(
            self.state->sourceMonitorId, requestedFrame, cachedFrame);
        const RenderedSlot sourceCandidate{true, self.state->sourceMonitorId,
                                           cachedFrame};
        if (self.state->overlayDirty ||
            !(sourceCandidate == self.state->sourceRendered)) {
            TimelineRenderData sourceData;
            sourceData.color_management = self.state->document.color_management;
            sourceData.video_height = self.sourceMonitorView.bounds.size.height;
            sourceData.video_zoom = self.state->sourceMonitorZoom;
            sourceData.video_rotation_degrees = {0};
            const auto metadata =
                self.state->mediaMetadata.find(self.state->sourceMonitorId);
            if (metadata != self.state->mediaMetadata.end()) {
                sourceData.sequence_width = metadata->second.width;
                sourceData.sequence_height = metadata->second.height;
                if (metadata->second.metadata_complete) {
                    sourceData.video_rotation_degrees[0] =
                        metadata->second.rotation_degrees;
                    const int turns = static_cast<int>(
                        std::lround(metadata->second.rotation_degrees / 90.0));
                    if (std::abs(turns) % 2 == 1)
                        std::swap(sourceData.sequence_width,
                                  sourceData.sequence_height);
                }
            }
            const std::vector<AVFrame*> sourceFrames{sourceFrame};
            if (self.state->sourceRenderer->RenderFrames(sourceFrames,
                                                         sourceData))
                self.state->sourceRendered = sourceCandidate;
        }
        av_frame_free(&sourceFrame);
    } else if (self.state->sourceMonitorVisible && self.state->overlayDirty) {
        TimelineRenderData sourceData;
        sourceData.video_height = self.sourceMonitorView.bounds.size.height;
        const std::vector<AVFrame*> noFrames;
        self.state->sourceRenderer->RenderFrames(noFrames, sourceData);
    }
    self.state->overlayDirty = false;
    for (AVFrame*& frame : frames) av_frame_free(&frame);
}

- (void)windowDidResize:(NSNotification*)notification {
    (void)notification;
    if (self.state->renderer) {
        [self refreshTimelineChrome];
        self.state->overlayDirty = true;
    }
}

- (void)timelineMetalViewDidResize:(TimelineMetalView*)view {
    if (!self.state) return;
    if (view == self.metalView && self.state->renderer) {
        self.state->renderer->Resize(view.bounds);
        [self refreshTimelineChrome];
    } else if (view == self.sourceMonitorView && self.state->sourceRenderer) {
        self.state->sourceRenderer->Resize(view.bounds);
    } else if (view == self.programMonitorView && self.state->programRenderer) {
        self.state->programRenderer->Resize(view.bounds);
    }
    self.state->overlayDirty = true;
}

- (CGFloat)splitView:(NSSplitView*)splitView
    constrainMinCoordinate:(CGFloat)proposedMinimumPosition
               ofSubviewAt:(NSInteger)dividerIndex {
    (void)proposedMinimumPosition;
    (void)dividerIndex;
    if (splitView == self.workspaceSplitView) return 220.0;
    if (splitView == self.editorSplitView) return 180.0;
    if (splitView == self.monitorSplitView)
        return self.state && !self.state->sourceMonitorVisible ? 0.0 : 320.0;
    return proposedMinimumPosition;
}

- (CGFloat)splitView:(NSSplitView*)splitView
    constrainMaxCoordinate:(CGFloat)proposedMaximumPosition
               ofSubviewAt:(NSInteger)dividerIndex {
    (void)proposedMaximumPosition;
    (void)dividerIndex;
    if (splitView == self.workspaceSplitView)
        return std::max<CGFloat>(220.0, splitView.bounds.size.width - 647.0);
    if (splitView == self.editorSplitView)
        return std::max<CGFloat>(180.0, splitView.bounds.size.height - 200.0);
    if (splitView == self.monitorSplitView)
        return self.state && !self.state->sourceMonitorVisible
                   ? splitView.bounds.size.width
                   : std::max<CGFloat>(320.0,
                                       splitView.bounds.size.width - 320.0);
    return proposedMaximumPosition;
}

- (BOOL)splitView:(NSSplitView*)splitView canCollapseSubview:(NSView*)subview {
    return splitView == self.monitorSplitView &&
           subview == self.sourceMonitorPanel;
}

- (void)splitViewDidResizeSubviews:(NSNotification*)notification {
    if (!self.state) return;
    [self refreshTimelineChrome];
    self.state->overlayDirty = true;
}

- (void)applicationWillTerminate:(NSNotification*)notification {
    (void)notification;
    if (self.state) self.state->exportCancel.store(true);
    if (self.state && self.state->mediaTasks)
        self.state->mediaTasks->CancelAll();
    if (self.state->audioPlayback) self.state->audioPlayback->Stop();
    [self.displayTimer invalidate];
    self.displayTimer = nil;
    for (auto& worker : self.state->workers) {
        worker.second->Stop();
    }
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
    (void)sender;
    return YES;
}

- (void)dealloc {
    [_displayTimer invalidate];
    delete _state;
}

@end

int main(int argc, char* argv[]) {
    if (argc == 3 && std::string(argv[1]) == "--describe") {
        std::string output;
        const int result = DescribeCommand(argv[2], output);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    if (argc == 4 && std::string(argv[1]) == "--apply-op") {
        std::string output;
        const int result = ApplyOperationCommand(argv[2], argv[3], output);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    if (argc == 4 && std::string(argv[1]) == "--apply-project-op") {
        std::string output;
        const int result =
            ApplyProjectOperationCommand(argv[2], argv[3], output);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    if (argc == 3 && std::string(argv[1]) == "--undo-project-op") {
        std::string output;
        const int result = UndoProjectOperationCommand(argv[2], output);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    if (argc == 3 && std::string(argv[1]) == "--redo-project-op") {
        std::string output;
        const int result = RedoProjectOperationCommand(argv[2], output);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    if ((argc == 3 || argc == 5) && std::string(argv[1]) == "--mcp-serve") {
        int port = 0;
        if (argc == 5) {
            if (std::string(argv[3]) != "--port") {
                std::fprintf(stderr, "Unknown --mcp-serve option: %s\n",
                             argv[3]);
                return 2;
            }
            port = std::atoi(argv[4]);
        }
        // Purely local, loopback-only HTTP + JSON-RPC MCP server
        // (ROADMAP.md F1.1). McpProjectBackend reuses ApplyOperationCommand/
        // UndoOperationCommand/RedoOperationCommand/DescribeCommand -- the
        // exact functions --apply-op/--describe already call -- so every
        // tool call takes the same load/apply/commit path a human editing
        // through the app or the CLI does. No AppKit/Metal/media decoding is
        // initialized for this path.
        McpProjectBackend backend(argv[2]);
        McpServer server(backend);
        std::string startError;
        if (!server.Start(port, startError)) {
            std::fprintf(stderr, "mcp-serve failed to start: %s\n",
                         startError.c_str());
            return 1;
        }
        std::fprintf(stderr,
                     "MCP server listening on http://127.0.0.1:%d/mcp for "
                     "'%s' (Ctrl-C to stop)\n",
                     server.Port(), argv[2]);
        static std::atomic_bool stopRequested{false};
        std::signal(SIGINT, [](int) { stopRequested.store(true); });
        std::signal(SIGTERM, [](int) { stopRequested.store(true); });
        while (!stopRequested.load() && server.IsRunning())
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        server.Stop();
        return 0;
    }
    if ((argc == 4 || argc == 5) && std::string(argv[1]) == "--ingest" &&
        (argc == 4 || std::string(argv[4]) == "--recursive")) {
        std::string output;
        const int result = IngestCommand(argv[2], argv[3], argc == 5, output);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    if (argc >= 4 && argc <= 6 && std::string(argv[1]) == "--export") {
        ExportSettings settings = Exporter::HevcMain10Preset(argv[3]);
        for (int index = 4; index < argc; ++index) {
            const std::string option(argv[index]);
            if (option == "--software")
                settings.encoder = ExportEncoder::HevcSoftware;
            else if (option == "--overwrite")
                settings.overwrite = true;
            else {
                std::fprintf(stderr, "Unknown export option: %s\n",
                             argv[index]);
                return 2;
            }
        }
        std::string output;
        int lastPercent = -1;
        const int result = ExportCommand(
            argv[2], settings,
            [&](const ExportProgress& progress) {
                const int percent = static_cast<int>(progress.fraction * 100.0);
                if (percent != lastPercent) {
                    lastPercent = percent;
                    std::fprintf(
                        stderr, "Export HEVC: %d%% (%lld/%lld images)\r",
                        percent,
                        static_cast<long long>(progress.rendered_frames),
                        static_cast<long long>(progress.total_frames));
                    std::fflush(stderr);
                }
            },
            nullptr, output);
        if (lastPercent >= 0) std::fputc('\n', stderr);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    if (argc > 2 || (argc == 2 && argv[1][0] == '-')) {
        std::fprintf(
            stderr,
            "Usage: %s [/path/to/project.cutmachine.json]\n"
            "       %s --describe /path/to/project.cutmachine.json\n"
            "       %s --apply-op /path/to/project.cutmachine.json "
            "'<op.json>'\n"
            "       %s --apply-project-op /path/to/project.cutmachine.json "
            "'<op.json>'\n"
            "       %s --undo-project-op /path/to/project.cutmachine.json\n"
            "       %s --redo-project-op /path/to/project.cutmachine.json\n"
            "       %s --mcp-serve /path/to/project.cutmachine.json "
            "[--port N]\n"
            "       %s --ingest /path/to/project.cutmachine.json "
            "/path/to/media "
            "[--recursive]\n"
            "       %s --export /path/to/project.cutmachine.json output.mp4 "
            "[--software] [--overwrite]\n",
            argv[0], argv[0], argv[0], argv[0], argv[0], argv[0], argv[0],
            argv[0], argv[0]);
        return 2;
    }

    @autoreleasepool {
        [NSApplication sharedApplication];
        NSString* documentPath =
            argc == 2 ? [NSString stringWithUTF8String:argv[1]] : nil;
        AppDelegate* delegate =
            [[AppDelegate alloc] initWithDocumentPath:documentPath];
        NSApp.delegate = delegate;
        [NSApp run];
    }
    return 0;
}
