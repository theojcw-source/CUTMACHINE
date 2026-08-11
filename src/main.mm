#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

extern "C" {
#include <libavutil/frame.h>
}

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "AudioPlayback.h"
#include "Cli.h"
#include "DecodeWorker.h"
#include "Document.h"
#include "EditLog.h"
#include "Export.h"
#include "FrameCache.h"
#include "Ingest.h"
#include "PerformanceMetrics.h"
#include "Renderer.h"
#include "Timeline.h"
#include "TimelineView.h"

namespace {

constexpr size_t kGlobalCacheBudget = 2000000000ULL;  // 2.0 GB, all sources.
constexpr double kAddTrackRowHeight = 24.0;
NSPasteboardType const kCutmachineMediaPasteboardType =
    @"com.cutmachine.library-media";
NSPasteboardType const kCutmachineBinPasteboardType = @"com.cutmachine.bin";

enum class TimelineTool { Select, Hand, Zoom, Cut };

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
    }
}

struct ResolvedSlot {
    bool active = false;
    Ulid sourceId;
    int64_t frame = -1;
};

struct RenderedSlot {
    bool active = false;
    Ulid sourceId;
    int64_t frame = -1;

    bool operator==(const RenderedSlot& other) const {
        return active == other.active && sourceId == other.sourceId &&
               frame == other.frame;
    }
};

struct AppState {
    Document document;
    EditLog editLog;
    TimelineViewport viewport;
    std::unique_ptr<TimelineInteraction> interaction;
    std::unique_ptr<Timeline> timeline;
    std::unique_ptr<Renderer> renderer;
    std::unique_ptr<FrameCache> frameCache;
    std::unique_ptr<PerformanceMetrics> performanceMetrics;
    std::unique_ptr<AudioPlayback> audioPlayback;
    std::map<Ulid, std::unique_ptr<DecodeWorker>> workers;
    // Runtime probe cache. UI metadata never mutates the edit document.
    std::map<Ulid, LibraryMedia> mediaMetadata;
    std::vector<Ulid> videoTrackIds;
    std::vector<ResolvedSlot> requested;
    std::vector<RenderedSlot> rendered;
    RationalTime duration{0, 1};
    RationalTime requestedPosition{0, 1};
    PlayheadResolution playheadResolution = PlayheadResolution::Frame;
    bool overlayDirty = true;
    TimelineTool tool = TimelineTool::Select;
    bool spaceHand = false;
    bool navigationDragging = false;
    bool scrubDragging = false;
    bool editDragging = false;
    bool lassoCandidate = false;
    bool lassoDragging = false;
    bool linkedSelection = true;
    bool linkedSelectionGesture = true;
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
        return {0.14f + variation * 0.25f, 0.48f + variation,
                0.27f + variation * 0.45f};
    return {0.18f + variation * 0.35f, 0.31f + variation * 0.55f,
            0.62f + variation};
}

NSString* TimeString(const RationalTime& time) {
    return [NSString stringWithFormat:@"%lld/%d",
                                      static_cast<long long>(time.value),
                                      time.rate];
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

Operation CutOperationForClip(const Document& document,
                              const DocumentClip& clip,
                              const RationalTime& position,
                              bool linkedSelection) {
    if (linkedSelection && !clip.link_group_id.empty()) {
        std::vector<Ulid> members =
            ExpandLinkedClipSelection(document, {clip.id});
        members.erase(
            std::remove_if(members.begin(), members.end(), [&](const Ulid& id) {
                const DocumentClip* member = document.FindClip(id);
                return !member || position <= member->timeline_in ||
                       position >= member->timeline_in.add(member->duration);
            }),
            members.end());
        if (members.size() > 1)
            return SplitLinkedClipsOperation{clip.link_group_id, members,
                                             position, {}, {}, {}, {}};
    }
    return SplitClipOperation{clip.id, position, {}};
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
                      operation.linked_track_ids.end(), linkedTrack->id) !=
                operation.linked_track_ids.end())
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

@interface TimelineMetalView : NSView
@property(nonatomic, weak) id<TimelineEventTarget> eventTarget;
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
                [self selectRowIndexes:
                          [NSIndexSet indexSetWithIndex:candidate]
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
    if (row >= 0)
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
    if (indexPath) self.selectionIndexPaths = [NSSet setWithObject:indexPath];
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
                                   TimelineEventTarget,
                                   NSOutlineViewDataSource,
                                   NSOutlineViewDelegate,
                                   NSTableViewDataSource,
                                   NSTableViewDelegate,
                                   NSCollectionViewDataSource,
                                   NSCollectionViewDelegate,
                                   NSTextFieldDelegate>
@property(nonatomic, strong) NSWindow* window;
@property(nonatomic, strong) TimelineMetalView* metalView;
@property(nonatomic, strong) NSView* mediaPanel;
@property(nonatomic, strong) NSPopUpButton* binPopup;
@property(nonatomic, strong) NSPopUpButton* mediaPopup;
@property(nonatomic, strong) NSTextField* binSummaryLabel;
@property(nonatomic, strong) NSButton* assignMediaButton;
@property(nonatomic, strong) NSOutlineView* binOutline;
@property(nonatomic, strong) NSTableView* mediaTable;
@property(nonatomic, strong) NSSearchField* mediaSearchField;
@property(nonatomic, strong) NSMutableArray<NSString*>* visibleMediaIds;
@property(nonatomic, copy) NSString* selectedBinId;
@property(nonatomic, strong) NSCollectionView* mediaCollection;
@property(nonatomic, strong) NSScrollView* mediaListScroll;
@property(nonatomic, strong) NSScrollView* mediaIconScroll;
@property(nonatomic, strong) NSSegmentedControl* mediaViewToggle;
@property(nonatomic, strong) NSButton* sourceMonitorButton;
@property(nonatomic, strong) NSTextField* infoLabel;
@property(nonatomic, strong) NSButton* detachAudioButton;
@property(nonatomic, strong) NSButton* linkedSelectionButton;
@property(nonatomic, strong) NSTimer* displayTimer;
@property(nonatomic, copy) NSString* documentPath;
@property(nonatomic, assign) AppState* state;
- (BOOL)importMediaURLs:(NSArray<NSURL*>*)urls intoBin:(NSString*)binId;
- (void)beginEditingBin:(NSString*)binId;
@end

@implementation AppDelegate

- (NSMenuItem*)menuItem:(NSString*)title action:(SEL)action key:(NSString*)key {
    NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:title
                                                  action:action
                                           keyEquivalent:key ?: @""];
    item.target = self;
    return item;
}

- (void)installApplicationMenus {
    NSMenu* bar = [[NSMenu alloc] initWithTitle:@"Main"];
    NSMenuItem* appRoot = [[NSMenuItem alloc] initWithTitle:@"CUTMACHINE"
                                                     action:nil
                                              keyEquivalent:@""];
    NSMenu* app = [[NSMenu alloc] initWithTitle:@"CUTMACHINE"];
    [app addItemWithTitle:@"À propos de CUTMACHINE"
                   action:@selector(orderFrontStandardAboutPanel:)
            keyEquivalent:@""];
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
    [edit addItem:[self menuItem:@"Supprimer la sélection"
                          action:@selector(menuDeleteSelection:)
                             key:@"\b"]];
    NSMenuItem* rippleDelete =
        [self menuItem:@"Suppression ripple"
                action:@selector(menuRippleDeleteSelection:)
                   key:@"\b"];
    rippleDelete.keyEquivalentModifierMask = NSEventModifierFlagShift;
    [edit addItem:rippleDelete];
    editRoot.submenu = edit;
    [bar addItem:editRoot];

    NSMenuItem* clipRoot = [[NSMenuItem alloc] initWithTitle:@"Clip"
                                                      action:nil
                                               keyEquivalent:@""];
    NSMenu* clip = [[NSMenu alloc] initWithTitle:@"Clip"];
    [clip addItem:[self menuItem:@"Ouvrir dans le moniteur source"
                          action:@selector(openSelectedMediaInSourceMonitor:)
                             key:@""]];
    [clip addItem:[self menuItem:@"Séparer l’audio"
                          action:@selector(detachAudioButtonPressed:)
                             key:@"u"]];
    [clip addItem:[self menuItem:@"Couper au playhead"
                          action:@selector(menuCutSelectedAtPlayhead:)
                             key:@""]];
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
    [timeline addItem:[self menuItem:@"Outil Sélection"
                              action:@selector(menuSelectTool:)
                                 key:@"v"]];
    [timeline addItem:[self menuItem:@"Outil Lame"
                              action:@selector(menuCutTool:)
                                 key:@"c"]];
    [timeline addItem:NSMenuItem.separatorItem];
    [timeline addItem:[self menuItem:@"Magnétisme"
                              action:@selector(menuToggleSnapping:)
                                 key:@"n"]];
    [timeline addItem:[self menuItem:@"Sélection liée"
                              action:@selector(menuToggleLinkedSelection:)
                                 key:@""]];
    [timeline addItem:[self menuItem:@"Cadrer toute la timeline"
                              action:@selector(menuFitTimeline:)
                                 key:@"f"]];
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
    [playback addItem:[self menuItem:@"Lecture / Pause"
                              action:@selector(menuPlayPause:)
                                 key:@" "]];
    [playback addItem:[self menuItem:@"Lecture arrière"
                              action:@selector(menuPlayReverse:)
                                 key:@"j"]];
    [playback addItem:[self menuItem:@"Arrêt"
                              action:@selector(menuStop:)
                                 key:@"k"]];
    [playback addItem:[self menuItem:@"Lecture avant"
                              action:@selector(menuPlayForward:)
                                 key:@"l"]];
    playbackRoot.submenu = playback;
    [bar addItem:playbackRoot];
    NSApp.mainMenu = bar;
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
        NSPopUpButton* menu = [[NSPopUpButton alloc]
            initWithFrame:NSMakeRect(132, y, 316, 26)
                pullsDown:NO];
        [menu addItemsWithTitles:choices];
        [accessory addSubview:menu];
        return menu;
    };
    NSString* currentFormat = [NSString
        stringWithFormat:@"Actuel — %d × %d", current.width, current.height];
    NSPopUpButton* format = addPopup(
        @"Dimensions :",
        @[ currentFormat, @"HD — 1920 × 1080", @"UHD — 3840 × 2160",
           @"Vertical HD — 1080 × 1920", @"Carré — 1080 × 1080" ],
        44);
    NSString* currentRate = [NSString
        stringWithFormat:@"Actuelle — %d/%d i/s", current.frame_rate.num,
                         current.frame_rate.den];
    NSPopUpButton* cadence = addPopup(
        @"Cadence :",
        @[ currentRate, @"23,976 i/s", @"24 i/s", @"25 i/s",
           @"29,97 i/s", @"50 i/s", @"59,94 i/s" ],
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
        case 1: updated.width = 1920; updated.height = 1080; break;
        case 2: updated.width = 3840; updated.height = 2160; break;
        case 3: updated.width = 1080; updated.height = 1920; break;
        case 4: updated.width = 1080; updated.height = 1080; break;
        default: break;
    }
    switch (cadence.indexOfSelectedItem) {
        case 1: updated.frame_rate = {24000, 1001}; break;
        case 2: updated.frame_rate = {24, 1}; break;
        case 3: updated.frame_rate = {25, 1}; break;
        case 4: updated.frame_rate = {30000, 1001}; break;
        case 5: updated.frame_rate = {50, 1}; break;
        case 6: updated.frame_rate = {60000, 1001}; break;
        default: break;
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
        NSPopUpButton* menu = [[NSPopUpButton alloc]
            initWithFrame:NSMakeRect(144, y, 364, 26)
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
    NSPopUpButton* resolution = popup(
        @"Résolution :",
        @[ @"Selon le preset", @"Format de la séquence", @"1920 × 1080",
           @"3840 × 2160" ],
        76);
    NSPopUpButton* cadence = popup(
        @"Cadence :",
        @[ @"Selon le preset", @"Cadence de la séquence", @"23,976 i/s",
           @"24 i/s", @"25 i/s", @"29,97 i/s", @"50 i/s",
           @"59,94 i/s" ],
        44);
    NSPopUpButton* encoder = popup(
        @"Encodage :",
        @[ @"Selon le preset", @"VideoToolbox — rapide",
           @"x265 — qualité constante" ],
        12);
    settingsAlert.accessoryView = accessory;
    if ([settingsAlert runModal] != NSAlertFirstButtonReturn) return;

    const auto& presets = Exporter::Presets();
    const size_t presetIndex = std::min<size_t>(
        static_cast<size_t>(preset.indexOfSelectedItem), presets.size() - 1);
    ExportSettings selectedSettings = Exporter::SettingsForPreset(
        presets[presetIndex].id, "", sequenceWidth, sequenceHeight,
        sequenceRate);
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
        case 1: selectedSettings.frame_rate = sequenceRate; break;
        case 2: selectedSettings.frame_rate = {24000, 1001}; break;
        case 3: selectedSettings.frame_rate = {24, 1}; break;
        case 4: selectedSettings.frame_rate = {25, 1}; break;
        case 5: selectedSettings.frame_rate = {30000, 1001}; break;
        case 6: selectedSettings.frame_rate = {50, 1}; break;
        case 7: selectedSettings.frame_rate = {60000, 1001}; break;
        default: break;
    }
    if (encoder.indexOfSelectedItem == 1)
        selectedSettings.encoder = ExportEncoder::HevcVideoToolbox;
    else if (encoder.indexOfSelectedItem == 2)
        selectedSettings.encoder = ExportEncoder::HevcSoftware;

    const BOOL mov = container.indexOfSelectedItem == 1;
    NSSavePanel* save = [NSSavePanel savePanel];
    save.title = @"Destination de l’export";
    save.nameFieldStringValue = mov ? @"export.mov" : @"export.mp4";
    save.allowedContentTypes = @[
        [UTType typeWithFilenameExtension:(mov ? @"mov" : @"mp4")]
    ];
    [save beginSheetModalForWindow:self.window
                 completionHandler:^(NSModalResponse response) {
        if (response != NSModalResponseOK || !save.URL) return;

        ExportSettings settings = selectedSettings;
        settings.output_path = save.URL.fileSystemRepresentation ?: "";
        settings.overwrite = true;  // NSSavePanel already confirmed this.

        ExportPlan plan;
        std::string error;
        if (!Exporter::BuildPlan(self.state->document,
                                 self.documentPath.UTF8String ?: "", settings,
                                 plan, error)) {
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
        [progressAlert beginSheetModalForWindow:self.window
                              completionHandler:^(NSModalResponse) {
            if (self.state && self.state->exportRunning)
                self.state->exportCancel.store(true);
        }];

        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            std::string renderError;
            const bool ok = Exporter::Run(
                plan,
                [&](const ExportProgress& progress) {
                    dispatch_async(dispatch_get_main_queue(), ^{
                        indicator.doubleValue = progress.fraction * 100.0;
                        progressAlert.informativeText = [NSString
                            stringWithFormat:@"%lld / %lld images — %.2fx",
                            static_cast<long long>(progress.rendered_frames),
                            static_cast<long long>(progress.total_frames),
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

    NSView* accessory = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 500, 264)];
    NSButton* enabled = [NSButton checkboxWithTitle:@"Activer pour ce projet"
                                             target:nil
                                             action:nil];
    enabled.frame = NSMakeRect(0, 236, 500, 24);
    enabled.state = current.enabled ? NSControlStateValueOn
                                    : NSControlStateValueOff;
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
        NSPopUpButton* popup = [[NSPopUpButton alloc]
            initWithFrame:NSMakeRect(188, y, 300, 26)
                pullsDown:NO];
        for (const auto& choice : choices) {
            [popup addItemWithTitle:choice.first];
            popup.lastItem.representedObject = choice.second;
            if ([choice.second isEqualToString:
                    [NSString stringWithUTF8String:selected.c_str()]])
                [popup selectItem:popup.lastItem];
        }
        [accessory addSubview:popup];
        y -= 32.0;
        return popup;
    };
    NSPopUpButton* inputGamut = addPopup(
        @"Gamut d’entrée :", {{@"Sony S-Gamut3.Cine", @"sony_sgamut3_cine"},
                              {@"Sony S-Gamut3", @"sony_sgamut3"},
                              {@"Rec.709", @"rec709"},
                              {@"Rec.2020", @"rec2020"}},
        current.input_gamut);
    NSPopUpButton* inputTransfer = addPopup(
        @"Courbe d’entrée :", {{@"Sony S-Log3", @"sony_slog3"},
                               {@"Rec.709", @"rec709"},
                               {@"Linéaire", @"linear"}},
        current.input_transfer);
    NSPopUpButton* ycbcr = addPopup(
        @"Matrice YCbCr :", {{@"Auto (métadonnées)", @"auto"},
                             {@"BT.709", @"bt709"},
                             {@"BT.2020 non constante", @"bt2020_ncl"}},
        current.input_ycbcr_matrix);
    NSPopUpButton* inputRange = addPopup(
        @"Plage du signal :", {{@"Auto (métadonnées)", @"auto"},
                               {@"Full / Extended", @"full"},
                               {@"Legal / Limited", @"limited"}},
        current.input_range);
    NSPopUpButton* working = addPopup(
        @"Espace de travail :", {{@"ACEScct (AP1)", @"acescct"},
                                  {@"Rec.2020 linéaire", @"rec2020"},
                                  {@"Rec.709 linéaire", @"rec709"}},
        current.working_gamut);
    NSPopUpButton* outputGamut = addPopup(
        @"Gamut de sortie :", {{@"Rec.2020", @"rec2020"},
                               {@"Rec.709", @"rec709"}},
        current.output_gamut);
    NSPopUpButton* outputTransfer = addPopup(
        @"Courbe de sortie :", {{@"HLG", @"hlg"},
                                {@"Rec.709", @"rec709"}},
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

- (instancetype)initWithDocumentPath:(NSString*)documentPath {
    if ((self = [super init])) {
        _documentPath = [documentPath copy];
        _state = new AppState();
    }
    return self;
}

- (BOOL)loadDocumentAndSources {
    const std::string documentPath(self.documentPath.UTF8String ?: "");
    std::string error;
    if (!Document::Load(documentPath, self.state->document, error)) {
        std::fprintf(stderr, "Unable to load document: %s\n", error.c_str());
        return NO;
    }
    const std::string logPath = EditLogPathForDocument(documentPath);
    std::error_code logExistsError;
    EditError editError = EditError::None;
    if (std::filesystem::exists(logPath, logExistsError) &&
        !EditLog::Load(logPath, self.state->editLog, editError, error)) {
        std::fprintf(stderr, "Unable to load edit log: %s\n", error.c_str());
        return NO;
    }
    if (logExistsError) {
        std::fprintf(stderr, "Unable to inspect edit log: %s\n",
                     logExistsError.message().c_str());
        return NO;
    }
    self.state->viewport.view_start = {0, 1};
    self.state->viewport.pixels_per_second = 100.0;
    self.state->viewport.track_height = 44.0;
    self.state->viewport.header_width = 96.0;
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
        std::filesystem::path mediaPath(source.path);
        if (mediaPath.is_relative()) {
            mediaPath = baseDirectory / mediaPath;
        }
        auto worker =
            std::make_unique<DecodeWorker>(source.id, *self.state->frameCache,
                                           *self.state->performanceMetrics);
        if (!worker->Open(mediaPath.lexically_normal().string(), 5)) {
            std::fprintf(stderr, "Unable to open source %s at %s\n",
                         source.id.c_str(), mediaPath.string().c_str());
            return NO;
        }
        if (static_cast<int64_t>(worker->FrameRateNumerator()) *
                source.rate.den !=
            static_cast<int64_t>(source.rate.num) *
                worker->FrameRateDenominator()) {
            std::fprintf(
                stderr, "Source %s declares rate %d/%d but media is %d/%d\n",
                source.id.c_str(), source.rate.num, source.rate.den,
                worker->FrameRateNumerator(), worker->FrameRateDenominator());
            return NO;
        }
        const int64_t declaredFrames =
            source.duration.to_frames(source.rate.num, source.rate.den);
        if (declaredFrames > worker->FrameCount()) {
            std::fprintf(
                stderr,
                "Source %s declares %lld frames but media exposes %lld\n",
                source.id.c_str(), static_cast<long long>(declaredFrames),
                static_cast<long long>(worker->FrameCount()));
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
            for (const DocumentTrack& track : self.state->document.sequence.tracks)
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
    for (const DocumentTrack& videoTrack : self.state->document.sequence.tracks) {
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
    [self installApplicationMenus];

    if (![self loadDocumentAndSources]) {
        [NSApp terminate:nil];
        return;
    }

    const NSRect windowRect = NSMakeRect(0.0, 0.0, 1600.0, 960.0);
    self.window =
        [[NSWindow alloc] initWithContentRect:windowRect
                                    styleMask:(NSWindowStyleMaskTitled |
                                               NSWindowStyleMaskClosable |
                                               NSWindowStyleMaskResizable)
                                      backing:NSBackingStoreBuffered
                                        defer:NO];
    self.window.title = @"CUTMACHINE — timeline scrub";
    self.window.delegate = self;
    self.window.acceptsMouseMovedEvents = YES;

    NSView* content = [[NSView alloc] initWithFrame:windowRect];
    self.window.contentView = content;
    constexpr double mediaPanelWidth = 320.0;
    self.mediaPanel =
        [[NSView alloc] initWithFrame:NSMakeRect(0.0, 0.0, mediaPanelWidth,
                                                 windowRect.size.height)];
    self.mediaPanel.autoresizingMask = NSViewHeightSizable;
    self.mediaPanel.wantsLayer = YES;
    self.mediaPanel.layer.backgroundColor =
        [NSColor colorWithWhite:0.075 alpha:1.0].CGColor;
    [content addSubview:self.mediaPanel];

    NSTextField* libraryTitle =
        [NSTextField labelWithString:@"PROJET / CHUTIERS"];
    libraryTitle.frame = NSMakeRect(14.0, windowRect.size.height - 34.0,
                                    mediaPanelWidth - 28.0, 18.0);
    libraryTitle.autoresizingMask = NSViewMinYMargin;
    libraryTitle.font = [NSFont systemFontOfSize:11.0
                                          weight:NSFontWeightSemibold];
    libraryTitle.textColor = NSColor.secondaryLabelColor;
    [self.mediaPanel addSubview:libraryTitle];

    NSButton* addBinButton =
        [NSButton buttonWithTitle:@"+ Chutier"
                           target:self
                           action:@selector(createBinPressed:)];
    addBinButton.frame =
        NSMakeRect(12.0, windowRect.size.height - 70.0, 142.0, 28.0);
    addBinButton.autoresizingMask = NSViewMinYMargin;
    addBinButton.bezelStyle = NSBezelStyleRounded;
    [self.mediaPanel addSubview:addBinButton];

    NSButton* deleteBinButton =
        [NSButton buttonWithTitle:@"Supprimer"
                           target:self
                           action:@selector(deleteBinPressed:)];
    deleteBinButton.frame =
        NSMakeRect(166.0, windowRect.size.height - 70.0, 142.0, 28.0);
    deleteBinButton.autoresizingMask = NSViewMinYMargin;
    deleteBinButton.bezelStyle = NSBezelStyleRounded;
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
        initWithFrame:NSMakeRect(12.0, windowRect.size.height - 300.0,
                                 mediaPanelWidth - 24.0, 220.0)];
    binScroll.autoresizingMask = NSViewMinYMargin;
    binScroll.documentView = self.binOutline;
    binScroll.hasVerticalScroller = YES;
    binScroll.borderType = NSBezelBorder;
    [self.mediaPanel addSubview:binScroll];

    self.mediaSearchField = [[NSSearchField alloc]
        initWithFrame:NSMakeRect(12.0, windowRect.size.height - 338.0,
                                 mediaPanelWidth - 104.0, 26.0)];
    self.mediaSearchField.placeholderString = @"Rechercher nom, codec, format…";
    self.mediaSearchField.target = self;
    self.mediaSearchField.action = @selector(mediaSearchChanged:);
    self.mediaSearchField.continuous = YES;
    self.mediaSearchField.autoresizingMask = NSViewMinYMargin;
    [self.mediaPanel addSubview:self.mediaSearchField];

    self.mediaViewToggle = [[NSSegmentedControl alloc]
        initWithFrame:NSMakeRect(mediaPanelWidth - 84.0,
                                 windowRect.size.height - 338.0, 72.0, 26.0)];
    self.mediaViewToggle.segmentCount = 2;
    [self.mediaViewToggle setLabel:@"☷" forSegment:0];
    [self.mediaViewToggle setLabel:@"▦" forSegment:1];
    self.mediaViewToggle.selectedSegment = 0;
    self.mediaViewToggle.target = self;
    self.mediaViewToggle.action = @selector(mediaViewChanged:);
    self.mediaViewToggle.autoresizingMask = NSViewMinYMargin;
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
    self.mediaTable.usesAlternatingRowBackgroundColors = YES;
    self.mediaListScroll = [[NSScrollView alloc]
        initWithFrame:NSMakeRect(12.0, 112.0, mediaPanelWidth - 24.0,
                                 windowRect.size.height - 462.0)];
    self.mediaListScroll.autoresizingMask = NSViewHeightSizable;
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
    [self.mediaCollection setDraggingSourceOperationMask:NSDragOperationCopy
                                                forLocal:YES];
    [self.mediaCollection registerClass:MediaIconItem.class
                  forItemWithIdentifier:@"media-icon"];
    self.mediaIconScroll =
        [[NSScrollView alloc] initWithFrame:self.mediaListScroll.frame];
    self.mediaIconScroll.autoresizingMask = NSViewHeightSizable;
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
    [self.mediaPanel addSubview:self.assignMediaButton];

    self.sourceMonitorButton =
        [NSButton buttonWithTitle:@"Source"
                           target:self
                           action:@selector(openSelectedMediaInSourceMonitor:)];
    self.sourceMonitorButton.frame = NSMakeRect(202.0, 76.0, 106.0, 28.0);
    self.sourceMonitorButton.autoresizingMask = NSViewMaxYMargin;
    self.sourceMonitorButton.bezelStyle = NSBezelStyleRounded;
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
    self.mediaTable.menu = mediaContext;
    self.mediaCollection.menu = mediaContext;

    self.binSummaryLabel = [NSTextField labelWithString:@""];
    self.binSummaryLabel.frame =
        NSMakeRect(14.0, 42.0, mediaPanelWidth - 28.0, 34.0);
    self.binSummaryLabel.autoresizingMask = NSViewMaxYMargin;
    self.binSummaryLabel.font = [NSFont systemFontOfSize:11.0];
    self.binSummaryLabel.textColor = NSColor.secondaryLabelColor;
    self.binSummaryLabel.maximumNumberOfLines = 2;
    [self.mediaPanel addSubview:self.binSummaryLabel];

    self.metalView = [[TimelineMetalView alloc]
        initWithFrame:NSMakeRect(mediaPanelWidth, 42.0,
                                 windowRect.size.width - mediaPanelWidth,
                                 windowRect.size.height - 42.0)];
    self.metalView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    self.metalView.eventTarget = self;
    [content addSubview:self.metalView];

    self.infoLabel = [NSTextField labelWithString:@"Aucun clip sélectionné"];
    self.infoLabel.frame =
        NSMakeRect(mediaPanelWidth + 20.0, 12.0,
                   windowRect.size.width - mediaPanelWidth - 375.0, 18.0);
    self.infoLabel.autoresizingMask = NSViewWidthSizable;
    self.infoLabel.font =
        [NSFont monospacedDigitSystemFontOfSize:12.0
                                         weight:NSFontWeightRegular];
    self.infoLabel.textColor = NSColor.secondaryLabelColor;
    self.infoLabel.lineBreakMode = NSLineBreakByTruncatingTail;
    [content addSubview:self.infoLabel];

    self.detachAudioButton =
        [NSButton buttonWithTitle:@"Séparer audio  U"
                           target:self
                           action:@selector(detachAudioButtonPressed:)];
    self.detachAudioButton.frame =
        NSMakeRect(windowRect.size.width - 170.0, 7.0, 150.0, 28.0);
    self.detachAudioButton.autoresizingMask = NSViewMinXMargin;
    self.detachAudioButton.bezelStyle = NSBezelStyleRounded;
    self.detachAudioButton.controlSize = NSControlSizeSmall;
    self.detachAudioButton.enabled = NO;
    self.detachAudioButton.toolTip =
        @"Crée un clip audio indépendant via DetachAudio (U)";
    [content addSubview:self.detachAudioButton];

    self.linkedSelectionButton =
        [NSButton buttonWithTitle:@"Sélection liée : ON"
                           target:self
                           action:@selector(linkedSelectionPressed:)];
    self.linkedSelectionButton.frame =
        NSMakeRect(windowRect.size.width - 340.0, 7.0, 160.0, 28.0);
    self.linkedSelectionButton.autoresizingMask = NSViewMinXMargin;
    self.linkedSelectionButton.bezelStyle = NSBezelStyleRounded;
    self.linkedSelectionButton.controlSize = NSControlSizeSmall;
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
    self.state->viewport.FitDuration(self.state->duration,
                                     self.metalView.bounds.size.width);

    [self.window center];
    [self.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    for (auto& worker : self.state->workers) {
        worker.second->Start();
    }
    [self requestResolvedPosition:{0, 1}];
    self.displayTimer =
        [NSTimer scheduledTimerWithTimeInterval:(1.0 / 60.0)
                                         target:self
                                       selector:@selector(displayTick:)
                                       userInfo:nil
                                        repeats:YES];
}

- (void)requestResolvedPosition:(RationalTime)position {
    self.state->requestedPosition = position;
    self.state->requested.assign(self.state->videoTrackIds.size(), {});
    for (size_t slot = 0; slot < self.state->videoTrackIds.size(); ++slot) {
        const std::optional<ResolvedFrame> resolved =
            self.state->timeline->ResolveTrack(self.state->videoTrackIds[slot],
                                               position);
        if (!resolved) {
            continue;
        }
        self.state->requested[slot] =
            ResolvedSlot{true, resolved->source_id, resolved->source_frame};
        const auto worker = self.state->workers.find(resolved->source_id);
        if (worker != self.state->workers.end()) {
            worker->second->RequestFrame(resolved->source_frame);
        }
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
    const DocumentBin* cursor = self.state->document.FindBin(
        parentId.UTF8String ?: "");
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
    NSString* movingBin = [pasteboard stringForType:kCutmachineBinPasteboardType];
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
    NSArray<NSURL*>* urls = [pasteboard readObjectsForClasses:@[ NSURL.class ]
                                                     options:@{
                                                         NSPasteboardURLReadingFileURLsOnlyKey : @YES
                                                     }];
    if (urls.count > 0) return [self importMediaURLs:urls intoBin:target];

    NSString* media = [pasteboard stringForType:kCutmachineMediaPasteboardType];
    NSString* movingBin = [pasteboard stringForType:kCutmachineBinPasteboardType];
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
        self.binSummaryLabel.stringValue = [NSString
            stringWithFormat:@"Enregistrement impossible : %s",
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
    if (selected == "__all__" || selected == "__root__") {
        NSString* sequenceName = [NSString
            stringWithUTF8String:self.state->document.sequence.name.c_str()];
        if (query.length == 0 ||
            [sequenceName.lowercaseString rangeOfString:query].location !=
                NSNotFound) {
            [self.visibleMediaIds addObject:[NSString
                stringWithUTF8String:self.state->document.sequence.id.c_str()]];
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
    if (self.mediaViewToggle.selectedSegment == 1) {
        NSIndexPath* selected =
            self.mediaCollection.selectionIndexPaths.anyObject;
        if (!selected || selected.item >= self.visibleMediaIds.count)
            return nil;
        return self.visibleMediaIds[selected.item];
    }
    const NSInteger row = self.mediaTable.selectedRow;
    if (row < 0 || row >= (NSInteger)self.visibleMediaIds.count) return nil;
    return self.visibleMediaIds[row];
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
    if ([identifier isEqualToString:[NSString
            stringWithUTF8String:self.state->document.sequence.id.c_str()]]) {
        const DocumentSequence& sequence = self.state->document.sequence;
        item.textField.stringValue =
            [NSString stringWithUTF8String:sequence.name.c_str()];
        NSImage* symbol = [NSImage
            imageWithSystemSymbolName:@"timeline.selection"
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
    const LibraryMedia* media = self.state->document.FindLibraryMedia(
        identifier.UTF8String ?: "");
    if (!media) return item;
    item.textField.stringValue =
        [NSString stringWithUTF8String:media->filename.c_str()];
    NSImage* symbol = [NSImage imageWithSystemSymbolName:@"film"
                                accessibilityDescription:@"Média vidéo"];
    symbol.size = NSMakeSize(44.0, 44.0);
    item.imageView.image = symbol;
    item.view.toolTip = [NSString
        stringWithFormat:@"%s · %s · %dx%d · %@", media->filename.c_str(),
                         media->codec.c_str(), media->width, media->height,
                         TimeString(media->duration)];
    return item;
}

- (id<NSPasteboardWriting>)collectionView:(NSCollectionView*)collectionView
       pasteboardWriterForItemAtIndexPath:(NSIndexPath*)indexPath {
    (void)collectionView;
    if (indexPath.item >= self.visibleMediaIds.count) return nil;
    if ([self.visibleMediaIds[indexPath.item]
            isEqualToString:[NSString
                stringWithUTF8String:self.state->document.sequence.id.c_str()]])
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
    if ([self.visibleMediaIds[row]
            isEqualToString:[NSString
                stringWithUTF8String:self.state->document.sequence.id.c_str()]])
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
    if ([identifier isEqualToString:[NSString
            stringWithUTF8String:self.state->document.sequence.id.c_str()]]) {
        [self setPlaybackDirection:0];
        self.state->sourceMonitor = false;
        [self requestResolvedPosition:self.state->requestedPosition];
        self.infoLabel.stringValue = [NSString
            stringWithFormat:@"PROGRAMME    %s    %dx%d    %d/%d fps",
                             self.state->document.sequence.name.c_str(),
                             self.state->document.sequence.width,
                             self.state->document.sequence.height,
                             self.state->document.sequence.frame_rate.num,
                             self.state->document.sequence.frame_rate.den];
        return;
    }
    [self openMediaIdentifierInSourceMonitor:identifier];
}

- (BOOL)validateMenuItem:(NSMenuItem*)item {
    const SEL action = item.action;
    if (action == @selector(exportFinalVideo:))
        return self.state && !self.state->exportRunning;
    if (action == @selector(menuUndo:))
        return self.state && self.state->editLog.AppliedCount() > 0;
    if (action == @selector(menuRedo:))
        return self.state && self.state->editLog.UndoneCount() > 0;
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
    if (action == @selector(renameBinPressed:) || action == @selector
                                                      (deleteBinPressed:))
        return self.state &&
               self.state->document.FindBin(self.selectedBinId.UTF8String
                                                ?: "") != nullptr;
    if (action == @selector(assignMediaToBinPressed:))
        return self.state &&
               self.state->document.FindLibraryMedia(
                   [self selectedMediaId].UTF8String ?: "") != nullptr;
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
    if (action == @selector(detachAudioButtonPressed:)) {
        if (!self.state || !self.state->interaction) return NO;
        const DocumentClip* clip = self.state->document.FindClip(
            self.state->interaction->SelectedClipId());
        return clip && clip->include_audio;
    }
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
    if (!source || !media || worker == self.state->workers.end()) {
        self.binSummaryLabel.stringValue =
            @"Ce média doit être réingéré pour devenir une source montable.";
        return;
    }
    [self setPlaybackDirection:0];
    self.state->sourceMonitor = true;
    self.state->sourceMonitorId = sourceId;
    self.state->sourceMonitorPosition = {0, source->duration.rate};
    self.state->requested.assign(1, ResolvedSlot{true, sourceId, 0});
    worker->second->RequestFrame(0);
    self.state->rendered.clear();
    self.state->overlayDirty = true;
    self.infoLabel.stringValue = [NSString
        stringWithFormat:@"MONITEUR SOURCE    %s    durée %@    %d/%d fps",
                         media->filename.c_str(), TimeString(source->duration),
                         source->rate.num, source->rate.den];
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

- (NSInteger)numberOfRowsInTableView:(NSTableView*)tableView {
    return tableView == self.mediaTable ? self.visibleMediaIds.count : 0;
}

- (NSView*)tableView:(NSTableView*)tableView
    viewForTableColumn:(NSTableColumn*)tableColumn
                   row:(NSInteger)row {
    if (tableView != self.mediaTable || row < 0 ||
        row >= (NSInteger)self.visibleMediaIds.count)
        return nil;
    const LibraryMedia* media = self.state->document.FindLibraryMedia(
        self.visibleMediaIds[row].UTF8String ?: "");
    NSTextField* label = [NSTextField labelWithString:@""];
    label.lineBreakMode = NSLineBreakByTruncatingTail;
    if (!media) {
        const DocumentSequence& sequence = self.state->document.sequence;
        if ([tableColumn.identifier isEqualToString:@"name"])
            label.stringValue = [NSString
                stringWithFormat:@"◫ %s", sequence.name.c_str()];
        else if ([tableColumn.identifier isEqualToString:@"format"])
            label.stringValue = [NSString
                stringWithFormat:@"Séquence %dx%d", sequence.width,
                                 sequence.height];
        else
            label.stringValue = TimeString(self.state->duration);
        return label;
    }
    if ([tableColumn.identifier isEqualToString:@"name"])
        label.stringValue =
            [NSString stringWithUTF8String:media->filename.c_str()];
    else if ([tableColumn.identifier isEqualToString:@"format"])
        label.stringValue =
            media->metadata_complete
                ? [NSString stringWithFormat:@"%s %dx%d", media->codec.c_str(),
                                             media->width, media->height]
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
    NSString* target = self.state->document.FindBin(
                           self.selectedBinId.UTF8String ?: "")
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
                path, std::filesystem::directory_options::skip_permission_denied,
                error), end;
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

    const Document before = self.state->document;
    const std::filesystem::path documentPath = std::filesystem::absolute(
        std::filesystem::path(self.documentPath.UTF8String ?: ""));
    std::map<std::string, size_t> known;
    for (size_t index = 0; index < self.state->document.library.size(); ++index) {
        LibraryMedia& media = self.state->document.library[index];
        std::filesystem::path path(media.path);
        if (path.is_relative()) path = documentPath.parent_path() / path;
        std::error_code error;
        const auto canonical = std::filesystem::weakly_canonical(path, error);
        if (!error) known[canonical.string()] = index;
    }

    size_t added = 0;
    size_t moved = 0;
    size_t rejected = 0;
    std::vector<std::pair<Ulid, std::filesystem::path>> addedSources;
    for (const auto& candidate : candidates) {
        std::error_code error;
        const auto absolute = std::filesystem::weakly_canonical(candidate, error);
        if (error) {
            ++rejected;
            continue;
        }
        const auto existing = known.find(absolute.string());
        if (existing != known.end()) {
            LibraryMedia& media = self.state->document.library[existing->second];
            if (media.bin_id != targetBin) {
                media.bin_id = targetBin;
                ++moved;
            }
            continue;
        }
        LibraryMedia media;
        media.id = GenerateUlid();
        media.filename = absolute.filename().string();
        media.bin_id = targetBin;
        media.path = std::filesystem::relative(
                         absolute, documentPath.parent_path(), error)
                         .lexically_normal()
                         .string();
        if (error) media.path = absolute.string();
        std::string probeError;
        if (!ProbeMediaMetadata(absolute.string(), media, probeError)) {
            ++rejected;
            continue;
        }
        self.state->document.library.push_back(media);
        self.state->document.sources.push_back(
            {media.id, media.path, media.rate, media.duration});
        known[absolute.string()] = self.state->document.library.size() - 1;
        addedSources.push_back({media.id, absolute});
        ++added;
    }

    std::string message;
    if (added == 0 && moved == 0) {
        self.binSummaryLabel.stringValue = [NSString
            stringWithFormat:@"Aucun rush importé (%lu fichier%@ refusé%@).",
                             (unsigned long)rejected, rejected == 1 ? @"" : @"s",
                             rejected == 1 ? @"" : @"s"];
        return NO;
    }
    if (!self.state->document.Validate(message) || ![self persistEdits:message]) {
        self.state->document = before;
        self.binSummaryLabel.stringValue = [NSString
            stringWithFormat:@"Import impossible : %s", message.c_str()];
        return NO;
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
        }
    }
    if (!addedSources.empty()) {
        auto audio = std::make_unique<AudioPlayback>();
        std::string audioError;
        if (audio->Open(self.state->document,
                        documentPath.parent_path().string(), audioError))
            self.state->audioPlayback = std::move(audio);
    }
    NSString* selected = targetBin.empty() ? @"__root__" : binId;
    [self refreshBinControlsSelecting:selected];
    NSMutableArray<NSString*>* results = [NSMutableArray array];
    if (added)
        [results addObject:[NSString
            stringWithFormat:@"%lu rush%@ importé%@", (unsigned long)added,
                             added == 1 ? @"" : @"es",
                             added == 1 ? @"" : @"s"]];
    if (moved)
        [results addObject:[NSString
            stringWithFormat:@"%lu rush%@ déplacé%@", (unsigned long)moved,
                             moved == 1 ? @"" : @"es",
                             moved == 1 ? @"" : @"s"]];
    if (rejected)
        [results addObject:[NSString
            stringWithFormat:@"%lu refusé%@", (unsigned long)rejected,
                             rejected == 1 ? @"" : @"s"]];
    self.binSummaryLabel.stringValue = [results componentsJoinedByString:@" · "];
    return YES;
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
            Operation{AddBinOperation{binId, name, parent}},
            error, message)) {
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
    const DocumentBin* bin = self.state->document.FindBin(
        identifier.UTF8String ?: "");
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
                                         name.UTF8String ?: ""}}, error,
            message)) {
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
    NSString* media = [self selectedMediaId];
    const LibraryMedia* selected = self.state->document.FindLibraryMedia(
        media.UTF8String ?: "");
    if (!selected) return;

    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = @"Déplacer le rush";
    alert.informativeText = @"Choisissez son chutier de destination.";
    [alert addButtonWithTitle:@"Déplacer"];
    [alert addButtonWithTitle:@"Annuler"];
    NSPopUpButton* destinations =
        [[NSPopUpButton alloc] initWithFrame:NSMakeRect(0, 0, 320, 26)];
    [destinations addItemWithTitle:@"Sans chutier"];
    destinations.lastItem.representedObject = @"";
    std::vector<const DocumentBin*> bins;
    for (const DocumentBin& bin : self.state->document.bins) bins.push_back(&bin);
    std::stable_sort(bins.begin(), bins.end(),
                     [](const DocumentBin* left, const DocumentBin* right) {
                         return left->name < right->name;
                     });
    for (const DocumentBin* bin : bins) {
        std::string path = bin->name;
        const DocumentBin* parent = self.state->document.FindBin(bin->parent_id);
        while (parent) {
            path = parent->name + " / " + path;
            parent = self.state->document.FindBin(parent->parent_id);
        }
        [destinations addItemWithTitle:
                          [NSString stringWithUTF8String:path.c_str()]];
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
    if (!self.state->editLog.Apply(
            self.state->document,
            Operation{SetMediaBinOperation{media.UTF8String ?: "",
                                           bin.UTF8String ?: ""}},
            error, message)) {
        self.binSummaryLabel.stringValue = [NSString
            stringWithFormat:@"Classement refusé : %s", message.c_str()];
        return;
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
    const double contentHeight =
        kTimelineRulerHeight +
        self.state->document.sequence.tracks.size() * self.state->viewport.track_height +
        kAddTrackRowHeight;
    return std::min(contentHeight,
                    std::max(0.0, self.metalView.bounds.size.height - 160.0));
}

- (double)videoHeight {
    return self.metalView.bounds.size.height - [self timelineHeight];
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
    }
}

- (void)setTimelineTool:(TimelineTool)tool {
    self.state->tool = tool;
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

- (void)requestTimelinePosition:(RationalTime)position {
    if (self.state->sourceMonitor) {
        self.state->sourceMonitor = false;
        self.state->sourceMonitorId.clear();
        self.state->rendered.clear();
    }
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
    Operation operation = InsertClipOperation{track->id,
                                              sourceId,
                                              {0, source->duration.rate},
                                              source->duration,
                                              timelineIn,
                                              {},
                                              {}};
    EditError error = EditError::None;
    std::string message;
    const RationalTime playhead = self.state->requestedPosition;
    if (!self.state->editLog.Apply(self.state->document, std::move(operation),
                                   error, message)) {
        self.binSummaryLabel.stringValue =
            [NSString stringWithFormat:@"Insertion refusée (%s) : %s",
                                       EditErrorName(error), message.c_str()];
        return NO;
    }
    const auto& stored = std::get<InsertClipOperation>(
        self.state->editLog.AppliedEntries().back().op);
    self.state->interaction->SelectClip(stored.clip_id);
    [self refreshTimelineAfterEditFromPosition:playhead];
    if (![self persistEdits:message])
        std::fprintf(stderr, "Unable to persist media drop: %s\n",
                     message.c_str());
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
    self.detachAudioButton.enabled = NO;
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
                             media->pixel_format.c_str(),
                             media->color_transfer.c_str(),
                             media->color_range.c_str(),
                             media->color_space.c_str(),
                             media -> rotation_degrees, media -> rate.num,
                             media->rate.den,
                             media->has_audio ? @"oui" : @"non"];
    }
    NSString* roleText = @"clip";
    if (selectedTrack && selectedTrack->kind == "audio")
        roleText = @"audio séparé";
    else if (selectedTrack && selectedTrack->kind == "video") {
        roleText = clip->include_audio ? @"vidéo + audio lié — U séparer"
                                       : @"vidéo seule";
        self.detachAudioButton.enabled = clip->include_audio;
    }
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

- (void)detachAudioButtonPressed:(id)sender {
    (void)sender;
    [self.window makeFirstResponder:self.metalView];
    [self detachSelectedAudio];
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
    const NSPoint point = [self timelinePointForEvent:event];
    if (point.y < 0.0 || point.y > [self timelineHeight]) return;
    const double addTrackY =
        kTimelineRulerHeight +
        self.state->document.sequence.tracks.size() * self.state->viewport.track_height;
    if (point.y >= addTrackY && point.y < addTrackY + kAddTrackRowHeight &&
        point.x >= 0.0 && point.x < self.state->viewport.header_width) {
        [self addTrack:(point.x >= self.state->viewport.header_width * 0.5)];
        return;
    }
    if (point.y < kTimelineRulerHeight && point.x >= 0.0 &&
        point.x < self.state->viewport.header_width) {
        const int index = std::clamp(static_cast<int>(point.x / 24.0), 0, 3);
        [self setTimelineTool:static_cast<TimelineTool>(index)];
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
        Operation operation = CutOperationForClip(
            self.state->document, *clip,
            self.state->viewport.XToTime(point.x, clip->duration.rate),
            linkedCut);
        EditError error = EditError::None;
        std::string message;
        const RationalTime playhead = self.state->requestedPosition;
        if (self.state->editLog.Apply(self.state->document,
                                      std::move(operation), error, message)) {
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
    self.state->linkedSelectionGesture =
        self.state->linkedSelection &&
        (event.modifierFlags & NSEventModifierFlagOption) == 0;
    self.state->interaction->SetLinkedSelectionEnabled(
        self.state->linkedSelectionGesture);
    self.state->interaction->PointerDown(point.x, point.y,
                                         self.metalView.bounds.size.width,
                                         [self playheadInputRate]);
    if (self.state->linkedSelectionGesture && selectionHit) {
        self.state->interaction->SelectClips(ExpandLinkedClipSelection(
            self.state->document, {selectionHit->clip_id}));
    }
    self.state->editDragging = self.state->interaction->HasActiveDrag();
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
    self.state->overlayDirty = true;
}

- (void)timelineMouseMoved:(NSEvent*)event {
    self.state->cutPreviewX.reset();
    self.state->cutPreviewY.reset();
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
    const char* path = self.documentPath.UTF8String;
    return CommitDocumentAndEditLog(path ? path : "", self.state->document,
                                    self.state->editLog, message);
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
    Operation operation =
        AddTrackOperation{"", audio ? "audio" : "video", index};
    if (self.state->editLog.Apply(self.state->document, std::move(operation),
                                  error, message)) {
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

- (void)detachSelectedAudio {
    const DocumentClip* clip = self.state->document.FindClip(
        self.state->interaction->SelectedClipId());
    const DocumentTrack* sourceTrack =
        clip ? self.state->document.FindTrackForClip(clip->id) : nullptr;
    if (!clip || !sourceTrack || sourceTrack->kind != "video" ||
        !clip->include_audio) {
        self.infoLabel.stringValue =
            @"Sélectionnez un clip vidéo dont l’audio est encore lié";
        return;
    }
    const Ulid audioClipId = GenerateUlid();
    Ulid targetTrackId;
    for (const DocumentTrack* track :
         TimelineTracksInDisplayOrder(self.state->document)) {
        if (track->kind != "audio") continue;
        Document candidate = self.state->document;
        Operation probe =
            DetachAudioOperation{clip->id, track->id, audioClipId, {}};
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
        self.infoLabel.stringValue =
            @"Ajoutez une piste audio verte libre, puis appuyez sur U";
        return;
    }
    const RationalTime playhead = self.state->requestedPosition;
    EditError error = EditError::None;
    std::string message;
    Operation operation =
        DetachAudioOperation{clip->id, targetTrackId, audioClipId, {}};
    if (self.state->editLog.Apply(self.state->document, std::move(operation),
                                  error, message)) {
        self.state->interaction->SelectClip(audioClipId);
        [self refreshTimelineAfterEditFromPosition:playhead];
        if (![self persistEdits:message])
            std::fprintf(stderr, "Unable to persist audio detach: %s\n",
                         message.c_str());
        [self updateSelectionInfo];
        self.state->overlayDirty = true;
    } else {
        std::fprintf(stderr, "Audio detach rejected (%s): %s\n",
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
    [self requestResolvedPosition:quantized];
}

- (void)timelineMouseUp:(NSEvent*)event {
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
        self.state->interaction->SetLinkedSelectionEnabled(
            self.state->linkedSelection);
        return;
    }
    self.state->editDragging = false;
    const RationalTime playhead = self.state->requestedPosition;
    EditError error = EditError::None;
    std::string message;
    if (self.state->interaction->PointerUp(error, message)) {
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
    NSString* characters = event.charactersIgnoringModifiers.lowercaseString;
    if ((modifiers & NSEventModifierFlagCommand) != 0) {
        if ([characters isEqualToString:@"l"] &&
            (modifiers & NSEventModifierFlagShift) != 0) {
            self.linkedSelectionButton.state = self.state->linkedSelection
                                                   ? NSControlStateValueOff
                                                   : NSControlStateValueOn;
            [self linkedSelectionPressed:self.linkedSelectionButton];
            return YES;
        }
        if ([characters isEqualToString:@"t"] &&
            (modifiers & NSEventModifierFlagShift) != 0) {
            [self addTrack:(modifiers & NSEventModifierFlagOption) != 0];
            return YES;
        }
        if (![characters isEqualToString:@"z"]) return NO;
        EditError error = EditError::None;
        std::string message;
        const bool redo = (modifiers & NSEventModifierFlagShift) != 0;
        const RationalTime playhead = self.state->requestedPosition;
        const bool changed = redo ? self.state->editLog.Redo(
                                        self.state->document, error, message)
                                  : self.state->editLog.Undo(
                                        self.state->document, error, message);
        if (changed) {
            [self refreshTimelineAfterEditFromPosition:playhead];
            [self refreshBinControlsSelecting:nil];
            if (![self persistEdits:message])
                std::fprintf(stderr, "Unable to persist undo/redo: %s\n",
                             message.c_str());
            [self updateSelectionInfo];
            self.state->overlayDirty = true;
        } else if (error != EditError::EmptyUndo &&
                   error != EditError::EmptyRedo) {
            std::fprintf(stderr, "%s failed (%s): %s\n", redo ? "Redo" : "Undo",
                         EditErrorName(error), message.c_str());
        }
        return YES;
    }

    if ([characters isEqualToString:@"u"]) {
        [self detachSelectedAudio];
        return YES;
    }

    if (event.keyCode == 51 ||
        event.keyCode == 117) {  // Delete / Forward Delete
        const auto gap = self.state->interaction->SelectedGap();
        const Ulid selectedClipId = self.state->interaction->SelectedClipId();
        const std::vector<Ulid> selectedClipIds =
            self.state->interaction->SelectedClipIds();
        if (!gap && selectedClipIds.empty()) return NO;
        const RationalTime playhead = self.state->requestedPosition;
        EditError error = EditError::None;
        std::string message;
        const bool rippleDelete =
            (modifiers & NSEventModifierFlagShift) != 0;
        Operation operation =
            gap ? Operation{GapDeleteOperationForSelection(
                      self.state->document, *gap, self.state->linkedSelection)}
                : (rippleDelete
                       ? Operation{RemoveClipOperation{
                             selectedClipId.empty() ? selectedClipIds.front()
                                                    : selectedClipId,
                             {}}}
                       : Operation{ClearClipOperation{
                             selectedClipId.empty() ? selectedClipIds.front()
                                                    : selectedClipId,
                             {}}});
        if (!gap && self.state->linkedSelection && selectedClipIds.size() > 1) {
            const DocumentClip* first =
                self.state->document.FindClip(selectedClipIds.front());
            const bool oneGroup =
                first && !first->link_group_id.empty() &&
                std::all_of(selectedClipIds.begin(), selectedClipIds.end(),
                            [&](const Ulid& id) {
                                const DocumentClip* clip =
                                    self.state->document.FindClip(id);
                                return clip && clip->link_group_id ==
                                                   first->link_group_id;
                            });
            if (oneGroup) {
                operation = rippleDelete
                                ? Operation{RemoveLinkedClipsOperation{
                                      first->link_group_id, selectedClipIds,
                                      {}}}
                                : Operation{ClearLinkedClipsOperation{
                                      first->link_group_id, selectedClipIds,
                                      {}}};
            }
        }
        if (self.state->editLog.Apply(self.state->document,
                                      std::move(operation), error, message)) {
            if (!gap) self.state->interaction->SelectClip("");
            [self refreshTimelineAfterEditFromPosition:playhead];
            if (![self persistEdits:message])
                std::fprintf(stderr, "Unable to persist gap deletion: %s\n",
                             message.c_str());
            [self updateSelectionInfo];
            self.state->overlayDirty = true;
        } else {
            std::fprintf(stderr, "Delete rejected (%s): %s\n",
                         EditErrorName(error), message.c_str());
        }
        return YES;
    }

    if ([characters isEqualToString:@" "]) {
        if (!event.isARepeat) self.state->spaceUsedForPan = false;
        self.state->spaceHand = true;
        [self applyToolCursor];
        return YES;
    }
    if ([characters isEqualToString:@"v"]) {
        [self setTimelineTool:TimelineTool::Select];
        return YES;
    }
    if ([characters isEqualToString:@"h"]) {
        [self setTimelineTool:TimelineTool::Hand];
        return YES;
    }
    if ([characters isEqualToString:@"z"]) {
        [self setTimelineTool:TimelineTool::Zoom];
        return YES;
    }
    if ([characters isEqualToString:@"c"] ||
        [characters isEqualToString:@"b"]) {
        [self setTimelineTool:TimelineTool::Cut];
        return YES;
    }
    if ([characters isEqualToString:@"f"]) {
        self.state->viewport.FitDuration(self.state->duration,
                                         self.metalView.bounds.size.width);
        self.state->overlayDirty = true;
        return YES;
    }
    if ([characters isEqualToString:@"j"]) {
        [self setPlaybackDirection:-1];
        return YES;
    }
    if ([characters isEqualToString:@"k"]) {
        [self setPlaybackDirection:0];
        return YES;
    }
    if ([characters isEqualToString:@"l"]) {
        [self setPlaybackDirection:1];
        return YES;
    }
    if ([characters isEqualToString:@"n"]) {
        self.state->interaction->SetSnappingEnabled(
            !self.state->interaction->SnappingEnabled());
        [self updateSelectionInfo];
        self.state->overlayDirty = true;
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
        const RationalTime delta =
            self.state->playheadResolution == PlayheadResolution::Sample
                ? RationalTime{direction * amount, 48000}
                : RationalTime{
                      direction * amount * [self playheadFrameRate].den,
                      [self playheadFrameRate].num};
        [self requestTimelinePosition:self.state->requestedPosition.add(delta)];
        return YES;
    }
    if (event.keyCode == 53) {  // Escape
        self.state->interaction->CancelDrag();
        self.state->navigationDragging = false;
        self.state->scrubDragging = false;
        self.state->editDragging = false;
        self.state->lassoCandidate = false;
        self.state->lassoDragging = false;
        self.state->overlayDirty = true;
        return YES;
    }
    return NO;
}

- (void)timelineKeyUp:(NSEvent*)event {
    if ([event.charactersIgnoringModifiers isEqualToString:@" "]) {
        self.state->spaceHand = false;
        [self applyToolCursor];
        if (!self.state->spaceUsedForPan) {
            [self setPlaybackDirection:self.state->playbackDirection == 0 ? 1
                                                                          : 0];
        }
    }
}

- (void)menuUndo:(id)sender {
    (void)sender;
    EditError error = EditError::None;
    std::string message;
    const RationalTime playhead = self.state->requestedPosition;
    if (self.state->editLog.Undo(self.state->document, error, message)) {
        [self refreshTimelineAfterEditFromPosition:playhead];
        [self refreshBinControlsSelecting:nil];
        [self persistEdits:message];
        [self updateSelectionInfo];
    }
}

- (void)menuRedo:(id)sender {
    (void)sender;
    EditError error = EditError::None;
    std::string message;
    const RationalTime playhead = self.state->requestedPosition;
    if (self.state->editLog.Redo(self.state->document, error, message)) {
        [self refreshTimelineAfterEditFromPosition:playhead];
        [self refreshBinControlsSelecting:nil];
        [self persistEdits:message];
        [self updateSelectionInfo];
    }
}

- (BOOL)deleteCurrentTimelineSelectionRipple:(BOOL)ripple {
    const auto gap = self.state->interaction->SelectedGap();
    const std::vector<Ulid> ids = self.state->interaction->SelectedClipIds();
    if (!gap && ids.empty()) return NO;
    Operation operation =
        gap ? Operation{GapDeleteOperationForSelection(
                  self.state->document, *gap, self.state->linkedSelection)}
            : (ripple
                   ? Operation{RemoveClipOperation{ids.front(), {}}}
                   : Operation{ClearClipOperation{ids.front(), {}}});
    if (!gap && self.state->linkedSelection && ids.size() > 1) {
        const DocumentClip* first = self.state->document.FindClip(ids.front());
        if (first && !first->link_group_id.empty())
            operation =
                ripple
                    ? Operation{RemoveLinkedClipsOperation{
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

- (void)menuCutSelectedAtPlayhead:(id)sender {
    (void)sender;
    const Ulid clipId = self.state->interaction->SelectedClipId();
    if (clipId.empty()) return;
    self.state->contextClipId = clipId;
    self.state->contextTime = self.state->requestedPosition;
    [self contextCutClip:nil];
}

- (void)menuSelectTool:(id)sender {
    (void)sender;
    [self setTimelineTool:TimelineTool::Select];
}
- (void)menuCutTool:(id)sender {
    (void)sender;
    [self setTimelineTool:TimelineTool::Cut];
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
    if (!self.state->editLog.Apply(
            self.state->document,
            Operation{RemoveTrackOperation{track->id}}, error, message)) {
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
    self.infoLabel.stringValue = [NSString
        stringWithFormat:@"%lu piste%@ vide%@ supprimée%@",
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
        [menu addItem:[self menuItem:@"Séparer l’audio"
                              action:@selector(detachAudioButtonPressed:)
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
    Operation operation = CutOperationForClip(
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
    [self presentNearestFrameAtDeadline:YES];
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

    add(0.0, top - 2.0, width, 2.0, 0.04f, 0.045f, 0.052f);
    add(0.0, top, width, timelineHeight, 0.055f, 0.061f, 0.070f);
    add(0.0, top, width, kTimelineRulerHeight, 0.105f, 0.116f, 0.132f);
    for (int index = 0; index < 4; ++index) {
        const bool active = static_cast<int>([self effectiveTool]) == index;
        add(index * 24.0, top, 24.0, kTimelineRulerHeight,
            active ? 0.12f : 0.075f, active ? 0.42f : 0.086f,
            active ? 0.62f : 0.105f);
        add(index * 24.0 + 23.0, top, 1.0, kTimelineRulerHeight, 0.22f, 0.23f,
            0.25f);
    }
    // Tool glyphs: selection arrow, hand/pan and magnifier. They remain
    // geometry in the Metal pass, not AppKit controls.
    add(7.0, top + 5.0, 4.0, 13.0, 0.92f, 0.93f, 0.96f);
    add(10.0, top + 14.0, 7.0, 4.0, 0.92f, 0.93f, 0.96f);
    add(31.0, top + 9.0, 10.0, 10.0, 0.92f, 0.93f, 0.96f);
    add(33.0, top + 5.0, 2.0, 7.0, 0.92f, 0.93f, 0.96f);
    add(37.0, top + 5.0, 2.0, 7.0, 0.92f, 0.93f, 0.96f);
    add(54.0, top + 5.0, 10.0, 2.0, 0.92f, 0.93f, 0.96f);
    add(54.0, top + 15.0, 10.0, 2.0, 0.92f, 0.93f, 0.96f);
    add(54.0, top + 7.0, 2.0, 8.0, 0.92f, 0.93f, 0.96f);
    add(62.0, top + 7.0, 2.0, 8.0, 0.92f, 0.93f, 0.96f);
    add(63.0, top + 16.0, 5.0, 2.0, 0.92f, 0.93f, 0.96f);
    // Razor blade.
    add(78.0, top + 6.0, 11.0, 3.0, 0.92f, 0.93f, 0.96f);
    add(80.0, top + 9.0, 7.0, 8.0, 0.92f, 0.93f, 0.96f);
    add(77.0, top + 16.0, 13.0, 2.0, 0.92f, 0.93f, 0.96f);
    const auto tracks = TimelineTracksInDisplayOrder(self.state->document);
    for (size_t index = 0; index < tracks.size(); ++index) {
        const double y = top + kTimelineRulerHeight +
                         index * self.state->viewport.track_height;
        if (y >= top + timelineHeight) break;
        const float trackShade = index % 2 == 0 ? 0.075f : 0.088f;
        add(0.0, y, width, self.state->viewport.track_height, trackShade,
            trackShade + 0.006f, trackShade + 0.012f);
        add(0.0, y, self.state->viewport.header_width,
            self.state->viewport.track_height, 0.105f, 0.116f, 0.132f);
        const bool video = tracks[index]->kind == "video";
        add(0.0, y, 4.0, self.state->viewport.track_height,
            video ? 0.12f : 0.18f, video ? 0.48f : 0.62f,
            video ? 0.78f : 0.35f);
        add(10.0, y + 9.0, 27.0, 26.0, 0.15f, 0.17f, 0.20f);
        add(15.0, y + 15.0, 17.0, 3.0, video ? 0.20f : 0.30f,
            video ? 0.58f : 0.72f, video ? 0.86f : 0.42f);
        add(15.0, y + 23.0, 12.0, 3.0, 0.48f, 0.51f, 0.56f);
        add(self.state->viewport.header_width - 44.0, y + 14.0, 10.0, 10.0,
            0.19f, 0.21f, 0.24f);
        add(self.state->viewport.header_width - 25.0, y + 14.0, 10.0, 10.0,
            0.19f, 0.21f, 0.24f);
        add(0.0, y + self.state->viewport.track_height - 1.0, width, 1.0, 0.13f,
            0.145f, 0.165f);
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
        add(tickX, top, 1.0, kTimelineRulerHeight, 0.46f, 0.47f, 0.50f);
        add(tickX, top + kTimelineRulerHeight - 5.0, 1.0, 5.0, 0.65f, 0.66f,
            0.70f);
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
                add(left, y, right - left, 2.0, 0.24f, 0.82f, 1.0f);
                add(left, y + height - 2.0, right - left, 2.0, 0.24f, 0.82f,
                    1.0f);
                add(left, y, 2.0, height, 0.24f, 0.82f, 1.0f);
                add(right - 2.0, y, 2.0, height, 0.24f, 0.82f, 1.0f);
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
                0.86f, 0.16f, 0.12f, 0.82f);
        }
        if (clip.selected) {
            add(left - 2.0, y - 2.0, right - left + 4.0, clip.height + 4.0,
                clip.valid ? 0.95f : 1.0f, clip.valid ? 0.78f : 0.16f,
                clip.valid ? 0.18f : 0.12f);
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
    if (!self.state->frameCache || !self.state->renderer) {
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
        candidates[slot] = {true, requested.sourceId, cachedFrame};
        if (!frames[slot] || cachedFrame != requested.frame) {
            missing = true;
        }
    }
    if (isDisplayDeadline && missing) {
        self.state->performanceMetrics->RecordDrop();
    }
    if (self.state->overlayDirty || candidates != self.state->rendered) {
        TimelineRenderData timelineData = [self timelineRenderData];
        timelineData.video_rotation_degrees.resize(candidates.size(), 0);
        for (size_t slot = 0; slot < candidates.size(); ++slot) {
            if (!candidates[slot].active) continue;
            const auto media =
                self.state->mediaMetadata.find(candidates[slot].sourceId);
            if (media != self.state->mediaMetadata.end() &&
                media->second.metadata_complete)
                timelineData.video_rotation_degrees[slot] =
                    media->second.rotation_degrees;
        }
        if (self.state->renderer->RenderFrames(frames, timelineData)) {
            self.state->rendered = candidates;
            self.state->overlayDirty = false;
        }
    }
    for (AVFrame*& frame : frames) av_frame_free(&frame);
}

- (void)windowDidResize:(NSNotification*)notification {
    (void)notification;
    if (self.state->renderer) {
        self.state->renderer->Resize(self.metalView.bounds);
        self.state->overlayDirty = true;
    }
}

- (void)applicationWillTerminate:(NSNotification*)notification {
    (void)notification;
    if (self.state) self.state->exportCancel.store(true);
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
                std::fprintf(stderr, "Unknown export option: %s\n", argv[index]);
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
                    std::fprintf(stderr, "Export HEVC: %d%% (%lld/%lld images)\r",
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
    if (argc != 2 || (argc >= 2 && argv[1][0] == '-')) {
        std::fprintf(stderr,
                     "Usage: %s /path/to/timeline.json\n"
                     "       %s --describe /path/to/timeline.json\n"
                     "       %s --apply-op /path/to/timeline.json '<op.json>'\n"
                     "       %s --ingest /path/to/timeline.json /path/to/media "
                     "[--recursive]\n"
                     "       %s --export /path/to/timeline.json output.mp4 "
                     "[--software] [--overwrite]\n",
                     argv[0], argv[0], argv[0], argv[0], argv[0]);
        return 2;
    }

    @autoreleasepool {
        [NSApplication sharedApplication];
        NSString* documentPath = [NSString stringWithUTF8String:argv[1]];
        AppDelegate* delegate =
            [[AppDelegate alloc] initWithDocumentPath:documentPath];
        NSApp.delegate = delegate;
        [NSApp run];
    }
    return 0;
}
