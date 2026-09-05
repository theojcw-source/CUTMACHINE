#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>
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
#include "InspectorGradeControls.h"
#include "McpLiveBackend.h"
#include "McpProjectBackend.h"
#include "McpServer.h"
#include "MediaPanelModel.h"
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
// right-hand panel host that F2.2 (Inspector) and F2.4 (Chat) install their
// content into (see PanelHostView.h). ChatPanelView.h is F2.4's actual
// content view -- the chat transcript + input this file installs into the
// dock's Chat slot below. Objective-C++ declarations, so #import rather
// than #include, matching AppKit/Foundation above.
#import "ChatPanelView.h"
#import "InspectorView.h"
#import "PanelHostView.h"
#import "UiComponents.h"
#import "UiThemeAppKit.h"

namespace {

constexpr size_t kGlobalCacheBudget = 2000000000ULL;  // 2.0 GB, all sources.
constexpr double kAddTrackRowHeight = 22.0;
NSPasteboardType const kCutmachineMediaPasteboardType =
    @"com.cutmachine.library-media";
NSPasteboardType const kCutmachineBinPasteboardType = @"com.cutmachine.bin";
NSPasteboardType const kCutmachineTimelinePasteboardType =
    @"com.cutmachine.timeline";
// A media drag can copy into the timeline or move into a bin. AppKit
// intersects this source mask with the destination's requested operation;
// advertising Copy alone makes every bin drop resolve to None.
constexpr NSDragOperation kMediaLocalDragOperations =
    NSDragOperationCopy | NSDragOperationMove;
NSString* const kAutomaticProxyGenerationDefaultsKey =
    @"CUTMACHINEAutomaticProxyGeneration";
NSString* const kProgramVideoScopeDefaultsKey = @"CUTMACHINEProgramVideoScope";

#if defined(CUTMACHINE_UI_SMOKE_TEST)
bool gUiSmokeTesting = false;
int gUiSmokeFailures = 0;
bool gUiSmokeIconMouseDown = false;
bool gUiSmokeIconDragSession = false;
int gUiSmokeDecodeReloads = 0;
std::filesystem::path gUiSmokeRoot;
std::string gUiSmokeProjectPath;

void UiSmokeCheck(bool condition, const char* label) {
    if (condition) {
        std::fprintf(stdout, "PASS: %s\n", label);
    } else {
        ++gUiSmokeFailures;
        std::fprintf(stderr, "FAIL: %s\n", label);
    }
    std::fflush(condition ? stdout : stderr);
}

bool PrepareUiSmokeProject(std::string& error) {
    gUiSmokeRoot = std::filesystem::temp_directory_path() /
                   (GenerateUlid() + "-cutmachine-ui-smoke");
    std::filesystem::create_directories(gUiSmokeRoot);
    const std::filesystem::path mediaPath = gUiSmokeRoot / "fixture.mov";
    if (FILE* media = std::fopen(mediaPath.c_str(), "wb")) {
        const char bytes[] = "CUTMACHINE UI smoke fixture";
        std::fwrite(bytes, 1, sizeof(bytes), media);
        std::fclose(media);
    } else {
        error = "unable to create UI smoke media fixture";
        return false;
    }

    Document document;
    const Ulid parentBinId = GenerateUlid();
    const Ulid mediaBinId = GenerateUlid();
    LibraryMedia media;
    media.id = GenerateUlid();
    media.path = mediaPath.string();
    media.filename = "00-fixture.mov";
    media.rate = {25, 1};
    media.duration = {100, 25};
    media.metadata_complete = false;
    media.bin_id = mediaBinId;
    document.library = {media};
    document.bins = {{parentBinId, "Rush", ""},
                     {mediaBinId, "1_RUSHES", parentBinId}};
    document.sources = {
        {media.id, media.path, media.rate, media.duration},
    };
    // Multiple visual rows are deliberate: a one-row fixture cannot catch a
    // flipped NSCollectionView coordinate lookup selecting a lower tile.
    for (int index = 1; index < 7; ++index) {
        LibraryMedia extra = media;
        extra.id = GenerateUlid();
        extra.filename = "0" + std::to_string(index) + "-fixture.mov";
        document.library.push_back(extra);
        document.sources.push_back(
            {extra.id, extra.path, extra.rate, extra.duration});
    }
    DocumentClip clip;
    clip.source_id = media.id;
    clip.source_in = {0, 25};
    clip.duration = {50, 25};
    clip.timeline_in = {0, 25};
    clip.include_audio = false;
    DocumentTrack track;
    track.kind = "video";
    track.index = 0;
    track.clips = {clip};
    DocumentClip audioClip = clip;
    audioClip.id = GenerateUlid();
    audioClip.include_audio = true;
    DocumentTrack audioTrack;
    audioTrack.kind = "audio";
    audioTrack.index = 1;
    audioTrack.clips = {audioClip};
    document.sequence.name = "UI Smoke";
    document.sequence.tracks = {track, audioTrack};
    Project project = Project::FromDocument(std::move(document), "UI Smoke");
    const std::filesystem::path package =
        gUiSmokeRoot / "UI Smoke.cutmachine-project";
    return CreatePortableProject(package.string(), project, gUiSmokeProjectPath,
                                 error);
}
#endif

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
    bool automaticProxiesEnabled = false;
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
    // Whether the timeline has been scaled to a real, laid-out viewport
    // width yet. False until then, so the first resize that brings a usable
    // width does the fit the launch path could not.
    bool viewportFitted = false;
    TimelineTool tool = TimelineTool::Select;
    bool spaceHand = false;
    bool navigationDragging = false;
    std::optional<TimelineZoomBarDrag> zoomBarDrag;
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
    int hoveredTimelineTool = -1;
    Ulid hoveredTrackId;
    int hoveredTrackControl = -1;
    bool sourceMonitor = false;
    Ulid sourceMonitorId;
    RationalTime sourceMonitorPosition{0, 1};
    std::optional<RationalTime> sourceIn;
    std::optional<RationalTime> sourceOut;
    bool sourceMonitorActive = false;
    double sourceMonitorZoom = 0.0;
    double programMonitorZoom = 0.0;
    VideoScopeMode programVideoScope = VideoScopeMode::Off;
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
    const float variation = static_cast<float>(hash & 0xff) / 255.0f;
    const ui::theme::Color base =
        audio ? ui::theme::kTrackAudio : ui::theme::kTrackVideo;
    const float lift = (variation - 0.5f) * 0.06f;
    return {std::clamp(base.r + lift, 0.0f, 1.0f),
            std::clamp(base.g + lift, 0.0f, 1.0f),
            std::clamp(base.b + lift, 0.0f, 1.0f)};
}

std::string RushDisplayName(const Project& project, const LibraryMedia& media) {
    if (const ProjectBinMetadata* metadata = project.FindBinMetadata(media.id);
        metadata && !metadata->display_name.empty())
        return metadata->display_name;
    return media.filename;
}

std::string TimelineClipName(const Document& document, const Project& project,
                             const Ulid& sourceId) {
    std::string name;
    if (const LibraryMedia* media = document.FindLibraryMedia(sourceId))
        name = RushDisplayName(project, *media);
    if (name.empty()) {
        if (const DocumentSource* source = document.FindSource(sourceId))
            name = std::filesystem::path(source->path).filename().string();
    }
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0) name.resize(dot);
    return name;
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
// timeline toolbar format HH:MM:SS:FF identically
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
- (void)timelineMagnify:(NSEvent*)event;
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
    [CMThemeColor(ui::theme::kSurfaceBase) setFill];
    NSRectFill(rect);
    [CMThemeColor(ui::theme::kBorderStrong) setFill];
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

@implementation TimelineMetalView {
    NSTrackingArea* _trackingArea;
}
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
- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    if (_trackingArea) [self removeTrackingArea:_trackingArea];
    _trackingArea = [[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:NSTrackingInVisibleRect | NSTrackingMouseEnteredAndExited |
                     NSTrackingMouseMoved | NSTrackingActiveInActiveApp
               owner:self
            userInfo:nil];
    [self addTrackingArea:_trackingArea];
}
- (NSView*)hitTest:(NSPoint)point {
    // Monitor controls remain interactive while passive labels stay visual
    // overlays and editing gestures remain owned by the Metal surface.
    NSView* child = [super hitTest:point];
    // Modern AppKit controls can return a private content subview from
    // -hitTest:. Walk back to the public control instead of handing the click
    // to the Metal view, which otherwise swallows Insert/Overwrite actions.
    for (NSView* candidate = child; candidate && candidate != self;
         candidate = candidate.superview) {
        if ([candidate isKindOfClass:NSPopUpButton.class] ||
            [candidate isKindOfClass:NSButton.class])
            return candidate;
    }
    // `point` is expressed in the superview's coordinates. Comparing it to
    // `bounds` only happened to work for monitor views placed at (0, 0); the
    // timeline has a non-zero frame origin, so every click and drag was
    // rejected. Let NSView's hit test decide whether the point is inside and
    // redirect passive overlay labels back to the Metal interaction surface.
    return child ? self : nil;
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
- (void)mouseExited:(NSEvent*)event {
    [self.eventTarget timelineMouseMoved:event];
}
- (void)scrollWheel:(NSEvent*)event {
    [self.eventTarget timelineScroll:event];
}
- (void)magnifyWithEvent:(NSEvent*)event {
    [self.eventTarget timelineMagnify:event];
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

@interface VideoFullscreenWindow : NSWindow
@end

@implementation VideoFullscreenWindow
- (BOOL)canBecomeKeyWindow {
    return YES;
}
- (BOOL)canBecomeMainWindow {
    return YES;
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
- (void)mouseDown:(NSEvent*)event {
    NSView* content = self.window.contentView;
    NSView* hit = [content hitTest:[content convertPoint:event.locationInWindow
                                                fromView:nil]];
    for (NSView* candidate = hit; candidate && candidate != self;
         candidate = candidate.superview) {
        if (![candidate isKindOfClass:NSTextField.class]) continue;
        NSTextField* field = (NSTextField*)candidate;
        if (![field.identifier hasPrefix:@"browser:"] || !field.editable) break;
        const NSInteger row = [self rowForView:field];
        if (row >= 0)
            [self selectRowIndexes:[NSIndexSet indexSetWithIndex:row]
                byExtendingSelection:NO];
        [field selectText:field];
        return;
    }
    [super mouseDown:event];
}
@end

@interface CMIndustrialTableRowView : NSTableRowView
@end

@implementation CMIndustrialTableRowView {
    NSTrackingArea* _trackingArea;
    BOOL _hovered;
    CALayer* _hoverEdge;
}

- (instancetype)initWithFrame:(NSRect)frameRect {
    if ((self = [super initWithFrame:frameRect])) {
        self.wantsLayer = YES;
        _hoverEdge = [CALayer layer];
        _hoverEdge.backgroundColor = CMThemeColor(ui::theme::kAccent).CGColor;
        [self.layer addSublayer:_hoverEdge];
    }
    return self;
}

- (BOOL)wantsUpdateLayer {
    return YES;
}

- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    if (_trackingArea) [self removeTrackingArea:_trackingArea];
    _trackingArea = [[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:NSTrackingInVisibleRect | NSTrackingMouseEnteredAndExited |
                     NSTrackingActiveInActiveApp
               owner:self
            userInfo:nil];
    [self addTrackingArea:_trackingArea];
}

- (void)mouseEntered:(NSEvent*)event {
    (void)event;
    _hovered = YES;
    self.needsDisplay = YES;
}

- (void)mouseExited:(NSEvent*)event {
    (void)event;
    _hovered = NO;
    self.needsDisplay = YES;
}

- (void)setSelected:(BOOL)selected {
    [super setSelected:selected];
    self.needsDisplay = YES;
}

- (void)drawSelectionInRect:(NSRect)dirtyRect {
    (void)dirtyRect;
}

- (void)updateLayer {
    const ui::theme::Color background =
        self.selected ? ui::theme::kSurfaceControl
        : _hovered    ? ui::theme::Mix(ui::theme::kSurfacePanel,
                                       ui::theme::kTextPrimary, 0.05f)
                      : ui::theme::kSurfacePanel;
    self.layer.backgroundColor = CMThemeColor(background).CGColor;
    _hoverEdge.frame = CGRectMake(0.0, 0.0, 2.0, self.bounds.size.height);
    _hoverEdge.hidden = !_hovered || self.selected;
}

@end

@interface ContextCollectionView : NSCollectionView <NSDraggingSource>
@property(nonatomic, weak) id doubleClickTarget;
@property(nonatomic) SEL doubleClickAction;
@property(nonatomic, strong) NSIndexPath* forwardedMouseDownPath;
@end
@implementation ContextCollectionView
- (NSIndexPath*)iconIndexPathAtPoint:(NSPoint)point {
    // The flow layout and its flipped document view can disagree about the Y
    // axis. Resolve from actual visible item views: these are the rectangles
    // AppKit displayed and the user can physically click.
    for (NSCollectionViewItem* item in self.visibleItems) {
        NSIndexPath* path = [self indexPathForItem:item];
        if (!path) continue;
        const NSRect frame = [item.view convertRect:item.view.bounds
                                             toView:self];
        if (NSPointInRect(point, frame)) return path;
    }
    return nil;
}
- (BOOL)point:(NSPoint)point isInLabelAtIndexPath:(NSIndexPath*)indexPath {
    NSCollectionViewItem* item = [self itemAtIndexPath:indexPath];
    if (!item.textField) return NO;
    const NSRect labelRect = [item.textField convertRect:item.textField.bounds
                                                  toView:self];
    return NSPointInRect(point, labelRect);
}
- (NSMenu*)menuForEvent:(NSEvent*)event {
    NSIndexPath* indexPath =
        [self iconIndexPathAtPoint:[self convertPoint:event.locationInWindow
                                             fromView:nil]];
    if (indexPath && ![self.selectionIndexPaths containsObject:indexPath])
        self.selectionIndexPaths = [NSSet setWithObject:indexPath];
    return [super menuForEvent:event];
}
- (void)mouseDown:(NSEvent*)event {
#if defined(CUTMACHINE_UI_SMOKE_TEST)
    if (gUiSmokeTesting) gUiSmokeIconMouseDown = true;
#endif
    const NSPoint point = [self convertPoint:event.locationInWindow
                                    fromView:nil];
    // Always prefer current on-screen geometry. forwardedMouseDownPath is
    // only a fallback because NSCollectionView can reuse an item's view
    // across reloadData, leaving a previously forwarded path stale.
    NSIndexPath* path =
        [self iconIndexPathAtPoint:point] ?: self.forwardedMouseDownPath;
    self.forwardedMouseDownPath = nil;
    if (!path) {
        self.selectionIndexPaths = [NSSet set];
        return;
    }
    const BOOL renameClick =
        event.clickCount == 1 && [self point:point isInLabelAtIndexPath:path];
    NSSet<NSIndexPath*>* previous = self.selectionIndexPaths;
    NSSet<NSIndexPath*>* selection = [NSSet setWithObject:path];
    if ((event.modifierFlags & NSEventModifierFlagCommand) != 0 &&
        self.allowsMultipleSelection) {
        NSMutableSet<NSIndexPath*>* toggled = [previous mutableCopy];
        if ([toggled containsObject:path])
            [toggled removeObject:path];
        else
            [toggled addObject:path];
        selection = toggled;
    }
    self.selectionIndexPaths = selection;
    NSSet<NSIndexPath*>* added = [selection
        objectsPassingTest:^BOOL(NSIndexPath* candidate, BOOL* stop) {
          (void)stop;
          return ![previous containsObject:candidate];
        }];
    if (!renameClick && added.count > 0 &&
        [self.delegate respondsToSelector:@selector(collectionView:
                                              didSelectItemsAtIndexPaths:)])
        [self.delegate collectionView:self didSelectItemsAtIndexPaths:added];

    const NSEventMask trackingMask =
        NSEventMaskLeftMouseDragged | NSEventMaskLeftMouseUp;
    while (true) {
        NSEvent* next = [self.window nextEventMatchingMask:trackingMask];
        if (!next || next.type == NSEventTypeLeftMouseUp) {
            if (renameClick) {
                NSCollectionViewItem* item = [self itemAtIndexPath:path];
                if (item.textField.editable) [item.textField selectText:self];
            } else if (event.clickCount == 2 && self.doubleClickAction) {
                [NSApp sendAction:self.doubleClickAction
                               to:self.doubleClickTarget
                             from:self];
            }
            return;
        }
        const NSPoint current = [self convertPoint:next.locationInWindow
                                          fromView:nil];
        if (std::hypot(current.x - point.x, current.y - point.y) < 4.0)
            continue;
        if ([self.delegate
                respondsToSelector:@selector(collectionView:
                                       canDragItemsAtIndexPaths:withEvent:)] &&
            ![self.delegate collectionView:self
                  canDragItemsAtIndexPaths:selection
                                 withEvent:event])
            continue;
        if (![self.delegate
                respondsToSelector:@selector(collectionView:
                                       pasteboardWriterForItemAtIndexPath:)])
            continue;
        id<NSPasteboardWriting> writer = [self.delegate collectionView:self
                                    pasteboardWriterForItemAtIndexPath:path];
        if (!writer) continue;
        NSCollectionViewItem* collectionItem = [self itemAtIndexPath:path];
        if (!collectionItem) continue;
        NSDraggingItem* draggingItem =
            [[NSDraggingItem alloc] initWithPasteboardWriter:writer];
        NSPoint imageOffset = NSZeroPoint;
        NSImage* image = [self draggingImageForItemsAtIndexPaths:selection
                                                       withEvent:event
                                                          offset:&imageOffset];
        [draggingItem setDraggingFrame:collectionItem.view.frame
                              contents:image];
#if defined(CUTMACHINE_UI_SMOKE_TEST)
        if (gUiSmokeTesting) gUiSmokeIconDragSession = true;
#endif
        [self beginDraggingSessionWithItems:@[ draggingItem ]
                                      event:next
                                     source:self];
        return;
    }
}

- (NSDragOperation)draggingSession:(NSDraggingSession*)session
    sourceOperationMaskForDraggingContext:(NSDraggingContext)context {
    (void)session;
    return context == NSDraggingContextWithinApplication
               ? kMediaLocalDragOperations
               : NSDragOperationCopy;
}
@end

@interface BrowserRenameTextField : NSTextField
@end

@implementation BrowserRenameTextField
- (ContextCollectionView*)owningCollectionView {
    NSView* ancestor = self.superview;
    while (ancestor && ![ancestor isKindOfClass:ContextCollectionView.class])
        ancestor = ancestor.superview;
    return (ContextCollectionView*)ancestor;
}

- (NSTableView*)owningTableView {
    NSView* ancestor = self.superview;
    while (ancestor && ![ancestor isKindOfClass:NSTableView.class])
        ancestor = ancestor.superview;
    return (NSTableView*)ancestor;
}

- (void)selectBrowserObjectForEvent:(NSEvent*)event {
    if (ContextCollectionView* collection = [self owningCollectionView]) {
        NSIndexPath* path = [collection
            iconIndexPathAtPoint:[collection convertPoint:event.locationInWindow
                                                 fromView:nil]];
        if (path) collection.selectionIndexPaths = [NSSet setWithObject:path];
        return;
    }
    if (NSTableView* table = [self owningTableView]) {
        const NSInteger row = [table rowForView:self];
        if (row >= 0)
            [table selectRowIndexes:[NSIndexSet indexSetWithIndex:row]
                byExtendingSelection:NO];
    }
}

- (void)mouseDown:(NSEvent*)event {
    [self selectBrowserObjectForEvent:event];
    // A name is an explicit editing affordance. Enter the field editor on the
    // first click instead of waiting for NSTableView's delayed second click.
    if (event.type == NSEventTypeLeftMouseDown && event.clickCount == 1 &&
        self.editable) {
        [self selectText:self];
        return;
    }
    [super mouseDown:event];
}

- (NSMenu*)menuForEvent:(NSEvent*)event {
    [self selectBrowserObjectForEvent:event];
    if (ContextCollectionView* collection = [self owningCollectionView])
        return collection.menu;
    if (NSTableView* table = [self owningTableView]) return table.menu;
    return [super menuForEvent:event];
}
@end

@interface MediaIconView : NSView
@property(nonatomic, strong) NSIndexPath* indexPath;
@end

@implementation MediaIconView
- (NSView*)hitTest:(NSPoint)point {
    if (!NSPointInRect(point, self.bounds)) return nil;
    // One owner for the whole tile avoids a race between NSTextField's field
    // editor and NSCollectionView's selection/drag tracking. The collection
    // resolves the label rectangle after mouse-up and starts editing there.
    return self;
}
- (void)mouseDown:(NSEvent*)event {
    NSView* ancestor = self.superview;
    while (ancestor && ![ancestor isKindOfClass:ContextCollectionView.class])
        ancestor = ancestor.superview;
    if (ancestor) {
        ((ContextCollectionView*)ancestor).forwardedMouseDownPath =
            self.indexPath;
        [(ContextCollectionView*)ancestor mouseDown:event];
    } else
        [super mouseDown:event];
}
- (NSMenu*)menuForEvent:(NSEvent*)event {
    NSView* ancestor = self.superview;
    while (ancestor && ![ancestor isKindOfClass:ContextCollectionView.class])
        ancestor = ancestor.superview;
    return ancestor ? [(ContextCollectionView*)ancestor menuForEvent:event]
                    : [super menuForEvent:event];
}
@end

@interface MediaIconItem : NSCollectionViewItem
@end

@implementation MediaIconItem
- (void)loadView {
    self.view =
        [[MediaIconView alloc] initWithFrame:NSMakeRect(0, 0, 132, 112)];
    NSImageView* image =
        [[NSImageView alloc] initWithFrame:NSMakeRect(14, 27, 104, 76)];
    image.imageScaling = NSImageScaleProportionallyUpOrDown;
    image.wantsLayer = YES;
    image.layer.backgroundColor =
        CMThemeColor(ui::theme::kSurfaceControl).CGColor;
    image.layer.cornerRadius = 0.0;
    NSTextField* label = [[BrowserRenameTextField alloc]
        initWithFrame:NSMakeRect(4, 4, 124, 19)];
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
@property(nonatomic, strong) VideoFullscreenWindow* videoFullscreenWindow;
@property(nonatomic, assign) BOOL videoFullscreenActive;
@property(nonatomic, strong) NSView* mediaPanel;
@property(nonatomic, strong) NSSplitView* workspaceSplitView;
@property(nonatomic, strong) NSSplitView* editorSplitView;
@property(nonatomic, strong) NSSplitView* monitorSplitView;
// F2.1 design system: fixed right-hand dock hosting the Inspector (F2.2) and
// Chat (F2.4) tabs. See PanelHostView.h -- the set of tabs and their order
// are fixed at construction time, never user-rearrangeable.
@property(nonatomic, strong) CMPanelHostView* rightDockPanel;
// F2.2 -- Inspector content installed into rightDockPanel's Inspector slot
// (see -applicationDidFinishLaunching below).
@property(nonatomic, strong) CMInspectorView* inspectorView;
// F2.4 -- the Chat tab's actual content (ChatPanelView.h), installed into
// rightDockPanel's PanelSlot::Chat once at startup (see
// -applicationDidFinishLaunching:). `chatBackend` wraps
// `self.state->document`/`self.state->editLog`
// by reference (McpLiveBackend.h) -- allocated once, alongside the view,
// and never freed or reassigned for the life of the window, the same
// "assign, never explicitly torn down" convention `state` below already
// uses.
@property(nonatomic, strong) CMChatPanelView* chatPanelView;
@property(nonatomic, assign) McpLiveBackend* chatBackend;
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
@property(nonatomic, assign) BOOL updatingBinControls;
@property(nonatomic, strong) NSCollectionView* mediaCollection;
@property(nonatomic, strong)
    NSMutableDictionary<NSString*, NSImage*>* mediaThumbnails;
@property(nonatomic, strong) NSScrollView* mediaListScroll;
@property(nonatomic, strong) NSScrollView* mediaIconScroll;
@property(nonatomic, strong) NSSegmentedControl* mediaViewToggle;
@property(nonatomic, strong) NSButton* sourceMonitorButton;
// Legacy F2.3 builders remain below while their engine operations are being
// migrated, but the ATELIER interface installs only mediaTabContentMedia.
// Audio and caption controls are deliberately absent from the static dock.
@property(nonatomic, strong) CMTabStripView* mediaTabStrip;
@property(nonatomic, strong) NSView* mediaTabContentMedia;
@property(nonatomic, strong) NSView* mediaTabContentAudio;
@property(nonatomic, strong) NSView* mediaTabContentCaptions;
@property(nonatomic, strong) NSTableView* audioTable;
@property(nonatomic, strong) NSScrollView* audioScroll;
@property(nonatomic, strong) NSSearchField* audioSearchField;
@property(nonatomic, strong) NSMutableArray<NSString*>* visibleAudioIds;
@property(nonatomic, strong) NSTextField* audioSummaryLabel;
@property(nonatomic, strong) NSTableView* captionStyleTable;
@property(nonatomic, strong) NSScrollView* captionStyleScroll;
@property(nonatomic, strong) NSMutableArray<NSString*>* visibleCaptionStyleIds;
@property(nonatomic, strong) NSTextField* captionSelectionLabel;
@property(nonatomic, strong) NSButton* addCaptionStyleButton;
@property(nonatomic, strong) NSButton* removeCaptionStyleButton;
@property(nonatomic, strong) NSButton* applyCaptionStyleButton;
@property(nonatomic, strong) NSButton* clearCaptionButton;
@property(nonatomic, strong) NSTextField* infoLabel;
@property(nonatomic, strong) NSTextField* offlineMediaLabel;
@property(nonatomic, strong) NSTextField* sourceOfflineMediaLabel;
@property(nonatomic, strong) NSTextField* sourceMonitorTitleLabel;
@property(nonatomic, strong) NSTextField* sourceMonitorZoneLabel;
@property(nonatomic, strong) NSTextField* programMonitorTitleLabel;
@property(nonatomic, strong) NSPopUpButton* sourceMonitorZoomPopup;
@property(nonatomic, strong) NSPopUpButton* programMonitorZoomPopup;
@property(nonatomic, strong) NSPopUpButton* programVideoScopePopup;
@property(nonatomic, strong) NSButton* sourceMonitorToggleButton;
@property(nonatomic, strong) NSTextField* timelineTimecodeLabel;
@property(nonatomic, strong) NSMutableArray<NSTextField*>* trackHeaderLabels;
@property(nonatomic, strong) NSMutableArray<NSImageView*>* timelineToolIcons;
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
// Timeline shortcuts remain global editing gestures while an Inspector
// control owns first responder. AppKit otherwise sends plain keys such as C
// only to the slider, making a graded clip appear impossible to cut.
@property(nonatomic, strong) id timelineShortcutMonitor;
@property(nonatomic, copy) NSString* documentPath;
@property(nonatomic, copy) NSString* lastProjectLoadError;
@property(nonatomic, assign) AppState* state;
- (BOOL)importMediaURLs:(NSArray<NSURL*>*)urls intoBin:(NSString*)binId;
- (BOOL)chooseStartupProject;
- (BOOL)createStartupProject;
- (BOOL)openStartupProject;
- (NSString*)resolvedProjectPath:(NSString*)selection;
- (void)collectPortableProject:(id)sender;
- (NSString*)selectedBrowserObjectId;
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
- (BOOL)applyAndPersistTimelineOperation:(Operation)operation
                                   error:(EditError&)error
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
- (void)createTimelineFromSelectedMedia:(id)sender;
- (void)saveProjectPressed:(id)sender;
- (void)openIconBin:(NSClickGestureRecognizer*)recognizer;
- (void)openIconItem:(id)sender;
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
- (void)refreshAfterProjectRename;
- (void)enqueueProxyForMediaIdentifier:(NSString*)identifier;
- (void)loadOrEnqueueWaveformForMediaIdentifier:(NSString*)identifier;
- (void)enqueueWaveformForMediaIdentifier:(NSString*)identifier;
- (void)loadOrEnqueueThumbnailForMediaIdentifier:(NSString*)identifier;
- (void)enqueueThumbnailForMediaIdentifier:(NSString*)identifier;
#if defined(CUTMACHINE_UI_SMOKE_TEST)
- (void)runUiSmokeTests;
#endif
@end

#if defined(CUTMACHINE_UI_SMOKE_TEST)
static NSButton* FindButtonWithTitle(NSView* root, NSString* title) {
    if ([root isKindOfClass:NSButton.class] &&
        [((NSButton*)root).title isEqualToString:title])
        return (NSButton*)root;
    for (NSView* child in root.subviews) {
        if (NSButton* match = FindButtonWithTitle(child, title)) return match;
    }
    return nil;
}

static void CollectSliders(NSView* root, NSMutableArray<NSSlider*>* result) {
    if ([root isKindOfClass:NSSlider.class]) [result addObject:(NSSlider*)root];
    for (NSView* child in root.subviews) CollectSliders(child, result);
}

static void ClickControlThroughWindow(NSControl* control) {
    NSWindow* window = control.window;
    const NSPoint local =
        NSMakePoint(NSMidX(control.bounds), NSMidY(control.bounds));
    const NSPoint windowPoint = [control convertPoint:local toView:nil];
    const NSTimeInterval timestamp = NSProcessInfo.processInfo.systemUptime;
    NSEvent* down = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown
                                       location:windowPoint
                                  modifierFlags:0
                                      timestamp:timestamp
                                   windowNumber:window.windowNumber
                                        context:nil
                                    eventNumber:0
                                     clickCount:1
                                       pressure:1.0];
    NSEvent* up = [NSEvent mouseEventWithType:NSEventTypeLeftMouseUp
                                     location:windowPoint
                                modifierFlags:0
                                    timestamp:timestamp + 0.01
                                 windowNumber:window.windowNumber
                                      context:nil
                                  eventNumber:0
                                   clickCount:1
                                     pressure:0.0];
    // NSButton's mouseDown enters a tracking loop. Queue mouse-up first so
    // that loop consumes it exactly as it would for a physical click.
    [NSApp postEvent:up atStart:NO];
    [window sendEvent:down];
}

static void SendWindowClick(NSView* view, NSPoint point) {
    NSWindow* window = view.window;
    if (!window) return;
    const NSPoint windowPoint = [view convertPoint:point toView:nil];
    const NSTimeInterval timestamp = NSProcessInfo.processInfo.systemUptime;
    NSEvent* down = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown
                                       location:windowPoint
                                  modifierFlags:0
                                      timestamp:timestamp
                                   windowNumber:window.windowNumber
                                        context:nil
                                    eventNumber:0
                                     clickCount:1
                                       pressure:1.0];
    NSEvent* up = [NSEvent mouseEventWithType:NSEventTypeLeftMouseUp
                                     location:windowPoint
                                modifierFlags:0
                                    timestamp:timestamp + 0.01
                                 windowNumber:window.windowNumber
                                      context:nil
                                  eventNumber:0
                                   clickCount:1
                                     pressure:0.0];
    [NSApp postEvent:up atStart:NO];
    [window sendEvent:down];
}

static void SendTimelineMouseGesture(NSView* view, NSPoint start, NSPoint end) {
    NSWindow* window = view.window;
    const NSPoint windowStart = [view convertPoint:start toView:nil];
    const NSPoint windowEnd = [view convertPoint:end toView:nil];
    const NSTimeInterval timestamp = NSProcessInfo.processInfo.systemUptime;
    const auto event = [&](NSEventType type, NSPoint location,
                           NSTimeInterval offset) {
        return [NSEvent
            mouseEventWithType:type
                      location:location
                 modifierFlags:0
                     timestamp:timestamp + offset
                  windowNumber:window.windowNumber
                       context:nil
                   eventNumber:0
                    clickCount:1
                      pressure:type == NSEventTypeLeftMouseUp ? 0.0 : 1.0];
    };
    TimelineMetalView* timeline = (TimelineMetalView*)view;
    [timeline mouseDown:event(NSEventTypeLeftMouseDown, windowStart, 0.0)];
    if (!NSEqualPoints(start, end))
        [timeline
            mouseDragged:event(NSEventTypeLeftMouseDragged, windowEnd, 0.01)];
    [timeline mouseUp:event(NSEventTypeLeftMouseUp, windowEnd, 0.02)];
}

static void SendWindowDragGesture(NSView* sourceView, NSPoint sourcePoint,
                                  NSView* destinationView,
                                  NSPoint destinationPoint) {
    NSWindow* window = sourceView.window;
    if (!window || destinationView.window != window) return;
    const NSPoint windowStart = [sourceView convertPoint:sourcePoint
                                                  toView:nil];
    const NSPoint windowEnd = [destinationView convertPoint:destinationPoint
                                                     toView:nil];
    const NSTimeInterval timestamp = NSProcessInfo.processInfo.systemUptime;
    const auto event = [&](NSEventType type, NSPoint location,
                           NSTimeInterval offset) {
        return [NSEvent
            mouseEventWithType:type
                      location:location
                 modifierFlags:0
                     timestamp:timestamp + offset
                  windowNumber:window.windowNumber
                       context:nil
                   eventNumber:0
                    clickCount:1
                      pressure:type == NSEventTypeLeftMouseUp ? 0.0 : 1.0];
    };
    const NSPoint windowThreshold =
        NSMakePoint(windowStart.x + 12.0, windowStart.y);
    NSEvent* threshold =
        event(NSEventTypeLeftMouseDragged, windowThreshold, 0.03);
    NSEvent* dragged = event(NSEventTypeLeftMouseDragged, windowEnd, 0.07);
    NSEvent* up = event(NSEventTypeLeftMouseUp, windowEnd, 0.10);
    [NSApp postEvent:threshold atStart:NO];
    [NSApp postEvent:dragged atStart:NO];
    [NSApp postEvent:up atStart:NO];
    [window sendEvent:event(NSEventTypeLeftMouseDown, windowStart, 0.0)];
}

static void SendKeyThroughWindow(NSView* view, NSString* characters,
                                 unsigned short keyCode) {
    NSWindow* window = view.window;
    [window makeFirstResponder:view];
    NSEvent* event =
        [NSEvent keyEventWithType:NSEventTypeKeyDown
                               location:NSZeroPoint
                          modifierFlags:0
                              timestamp:NSProcessInfo.processInfo.systemUptime
                           windowNumber:window.windowNumber
                                context:nil
                             characters:characters
            charactersIgnoringModifiers:characters
                              isARepeat:NO
                                keyCode:keyCode];
    [window sendEvent:event];
}

static void SendKeyThroughApplication(NSView* view, NSString* characters,
                                      unsigned short keyCode) {
    NSWindow* window = view.window;
    [window makeFirstResponder:view];
    NSEvent* event =
        [NSEvent keyEventWithType:NSEventTypeKeyDown
                               location:NSZeroPoint
                          modifierFlags:0
                              timestamp:NSProcessInfo.processInfo.systemUptime
                           windowNumber:window.windowNumber
                                context:nil
                             characters:characters
            charactersIgnoringModifiers:characters
                              isARepeat:NO
                                keyCode:keyCode];
    [NSApp sendEvent:event];
}
#endif

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
            @"title" : @"Affichage · Vidéo plein écran",
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
    [file addItem:[self menuItem:@"Enregistrer le projet"
                          action:@selector(saveProjectPressed:)
                             key:@"s"]];
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
    [timeline addItem:[self menuItem:@"Générer automatiquement les proxies"
                              action:@selector(menuToggleAutomaticProxies:)
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
    [view addItem:[self shortcutMenuItem:@"Vidéo plein écran"
                                  action:@selector(toggleFullscreen:)
                                 command:@"view.fullscreen"]];
    viewRoot.submenu = view;
    [bar addItem:viewRoot];
    NSApp.mainMenu = bar;
    [self applyShortcutBindingsToMenus];
}

- (void)toggleFullscreen:(id)sender {
    (void)sender;
    if (self.videoFullscreenActive) {
        self.videoFullscreenActive = NO;
        [self.programMonitorView removeFromSuperview];
        [self.videoFullscreenWindow orderOut:nil];
        self.programMonitorView.frame = self.programMonitorPanel.bounds;
        [self.programMonitorPanel addSubview:self.programMonitorView];
        self.programMonitorTitleLabel.hidden = NO;
        self.programMonitorZoomPopup.hidden = NO;
        self.sourceMonitorToggleButton.hidden = NO;
        [self.window makeKeyAndOrderFront:nil];
        [self.window makeFirstResponder:self.programMonitorView];
    } else {
        NSScreen* screen = self.window.screen ?: NSScreen.mainScreen;
        if (!screen) return;
        [self.programMonitorView removeFromSuperview];
        if (!self.videoFullscreenWindow) {
            self.videoFullscreenWindow = [[VideoFullscreenWindow alloc]
                initWithContentRect:screen.frame
                          styleMask:NSWindowStyleMaskBorderless
                            backing:NSBackingStoreBuffered
                              defer:NO
                             screen:screen];
            self.videoFullscreenWindow.backgroundColor = NSColor.blackColor;
            self.videoFullscreenWindow.opaque = YES;
            self.videoFullscreenWindow.level = NSMainMenuWindowLevel + 1;
            self.videoFullscreenWindow.animationBehavior =
                NSWindowAnimationBehaviorNone;
            self.videoFullscreenWindow.releasedWhenClosed = NO;
            self.videoFullscreenWindow.collectionBehavior =
                NSWindowCollectionBehaviorFullScreenAuxiliary |
                NSWindowCollectionBehaviorStationary;
        } else {
            [self.videoFullscreenWindow setFrame:screen.frame display:NO];
        }
        self.videoFullscreenActive = YES;
        self.programMonitorView.frame =
            self.videoFullscreenWindow.contentView.bounds;
        [self.videoFullscreenWindow.contentView
            addSubview:self.programMonitorView];
        self.programMonitorTitleLabel.hidden = YES;
        self.programMonitorZoomPopup.hidden = YES;
        self.sourceMonitorToggleButton.hidden = YES;
        self.state->sourceMonitorActive = false;
        [self.videoFullscreenWindow makeKeyAndOrderFront:nil];
        [self.videoFullscreenWindow makeFirstResponder:self.programMonitorView];
    }
    self.state->overlayDirty = true;
}

- (void)saveProjectPressed:(id)sender {
    (void)sender;
    std::string message;
    if ([self persistEdits:message]) {
        self.infoLabel.stringValue = @"Projet enregistré";
    } else {
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"Échec de l’enregistrement : %s",
                                       message.c_str()];
        NSBeep();
    }
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
    if (![self applyAndPersistTimelineOperation:Operation{std::move(updated)}
                                          error:editError
                                        message:message]) {
        [self showExportError:message];
        return;
    }
    [self refreshTimelineAfterEditFromPosition:self.state->requestedPosition];
    [self rebuildMediaList];
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
        CMThemeColor(ui::theme::kSurfacePanel).CGColor;
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
        _state->automaticProxiesEnabled = [NSUserDefaults.standardUserDefaults
            boolForKey:kAutomaticProxyGenerationDefaultsKey];
        _state->programVideoScope = VideoScopeModeFromPreference(
            static_cast<int32_t>([NSUserDefaults.standardUserDefaults
                integerForKey:kProgramVideoScopeDefaultsKey]));
        _mediaThumbnails = [NSMutableDictionary dictionary];
    }
    return self;
}

- (BOOL)loadDocumentAndSources {
    if ([self loadDocumentAndSourcesHoldingLock]) return YES;
    // A load that fails must not keep the project locked. Several callers
    // react to NO with -[NSApp terminate:], which tears the process down
    // without unwinding C++ destructors, so ProjectSessionLock's own
    // destructor never runs and the .lock directory outlives the process.
    // Acquire() does reclaim a lock whose owning pid is gone, so this only
    // ever leaked litter rather than wedging the next open -- but the litter
    // is confusing next to a project that failed to open for another reason.
    if (self.state) self.state->projectLock.reset();
    return NO;
}

- (BOOL)loadDocumentAndSourcesHoldingLock {
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
    if (!RecoverTextArtifactTransaction(
            std::filesystem::path(documentPath).parent_path().string(),
            error)) {
        self.lastProjectLoadError =
            [NSString stringWithFormat:@"Transaction projet irrécupérable : %s",
                                       error.c_str()];
        return NO;
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
            std::map<std::string, EditLog> recoveredLogs;
            ProjectEditLog recoveredProjectLog;
            if (!ProjectRecovery::LoadAutosave(documentPath, recovered,
                                               recoveredLogs,
                                               recoveredProjectLog, error)) {
                std::fprintf(stderr, "Unable to recover project: %s\n",
                             error.c_str());
                return NO;
            }
            if (!CommitStoredProjectAndLogs(documentPath, recovered,
                                            recoveredLogs, recoveredProjectLog,
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
    // Recovery must run before package validation: an interrupted commit can
    // leave the main generation incomplete while its autosave is still the
    // only valid representation of the project.
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
    self.state->viewport.track_height = ui::theme::kTimelineTrackHeight;
    self.state->viewport.header_width = ui::theme::kTimelineTrackHeaderWidth;
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
    Document stagedDocument = self.state->document;
    EditLog stagedLog = self.state->editLog;
    std::vector<Ulid> videoClipIds;
    for (const DocumentTrack& track : stagedDocument.sequence.tracks) {
        if (track.kind != "video") continue;
        for (const DocumentClip& clip : track.clips) {
            if (!clip.include_audio) continue;
            const auto detected =
                self.state->mediaMetadata.find(clip.source_id);
            const LibraryMedia* media =
                detected == self.state->mediaMetadata.end()
                    ? stagedDocument.FindLibraryMedia(clip.source_id)
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
             TimelineTracksInDisplayOrder(stagedDocument)) {
            if (track->kind != "audio") continue;
            Document candidate = stagedDocument;
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
            for (const DocumentTrack& track : stagedDocument.sequence.tracks)
                index = std::max(index, track.index + 1);
            targetTrackId = GenerateUlid();
            if (!stagedLog.Apply(
                    stagedDocument,
                    Operation{AddTrackOperation{targetTrackId, "audio", index}},
                    error, message))
                return NO;
        }
        if (!stagedLog.Apply(stagedDocument,
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
    for (const DocumentTrack& videoTrack : stagedDocument.sequence.tracks) {
        if (videoTrack.kind != "video") continue;
        for (const DocumentClip& video : videoTrack.clips) {
            if (video.include_audio || !video.sync_anchor_clip_id.empty())
                continue;
            const DocumentClip* match = nullptr;
            for (const DocumentTrack& audioTrack :
                 stagedDocument.sequence.tracks) {
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
        if (!stagedLog.Apply(
                stagedDocument,
                Operation{SetClipLinkOperation{pair.video, pair.audio,
                                               pair.group, pair.group}},
                error, message))
            return NO;
        ++migratedLinks;
    }
    if (!videoClipIds.empty() || migratedLinks > 0) {
        if (![self persistStagedDocument:stagedDocument
                                 editLog:stagedLog
                                 message:message])
            return NO;
        self.state->document = std::move(stagedDocument);
        self.state->editLog = std::move(stagedLog);
        self.state->lastHistoryDomain = HistoryDomain::Timeline;
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
        self.state->automaticProxiesEnabled =
            [NSUserDefaults.standardUserDefaults
                boolForKey:kAutomaticProxyGenerationDefaultsKey];
        self.state->programVideoScope = VideoScopeModeFromPreference(
            static_cast<int32_t>([NSUserDefaults.standardUserDefaults
                integerForKey:kProgramVideoScopeDefaultsKey]));
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
    content.wantsLayer = YES;
    content.layer.backgroundColor =
        CMThemeColor(ui::theme::kSurfaceBase).CGColor;
    self.window.contentView = content;
    constexpr double mediaPanelWidth = 320.0;
    // F2.1 design system: fixed-width right dock (Inspector/Chat tabs).
    // Fixed, not user-resized-and-remembered, on purpose -- see
    // PanelHostView.h.
    constexpr double rightDockWidth = 300.0;
    // UI-2026-08 -- transport and zoom now belong to the timeline display
    // list. The only fixed bottom strip is the 42 pt status surface.
    const double workspaceHeight = windowRect.size.height - 42.0;
    self.workspaceSplitView = [[CutmachineSplitView alloc]
        initWithFrame:NSMakeRect(0.0, 42.0, windowRect.size.width,
                                 workspaceHeight)];
    self.workspaceSplitView.vertical = YES;
    self.workspaceSplitView.dividerStyle = NSSplitViewDividerStyleThin;
    self.workspaceSplitView.delegate = self;
    self.workspaceSplitView.autoresizingMask =
        NSViewWidthSizable | NSViewHeightSizable;
    // ".3pane" because this split view gained a third pane (the right dock)
    // in F2.1. Autosaved geometry is positional, so frames written by a
    // two-pane build restore onto the wrong panes -- and since -adjustSubviews
    // redistributes in proportion to current sizes, a restored zero width
    // stays zero forever, which bricks the window rather than looking odd.
    // A new name retires that geometry instead of trying to repair it.
    self.workspaceSplitView.autosaveName = @"CUTMACHINE.WorkspaceSplit.3pane";
    [content addSubview:self.workspaceSplitView];
    self.mediaPanel = [[NSView alloc]
        initWithFrame:NSMakeRect(0.0, 0.0, mediaPanelWidth, workspaceHeight)];
    self.mediaPanel.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    self.mediaPanel.wantsLayer = YES;
    // Same surface token as the new right dock (UiTheme.h kSurfacePanel) --
    // both are side panels of the same visual kind.
    self.mediaPanel.layer.backgroundColor = CMSurfacePanelColor().CGColor;
    [self.workspaceSplitView addArrangedSubview:self.mediaPanel];

    // UI-2026-08 -- the left dock is a static media browser. Audio and
    // caption editing remain engine/command capabilities; they no longer
    // masquerade as alternate contents of this physical dock.
    const double mediaContentHeight = workspaceHeight;
    constexpr double binSidebarX = 8.0;
    constexpr double binSidebarWidth = 112.0;
    constexpr double mediaBrowserX = 128.0;
    constexpr double mediaBrowserWidth = mediaPanelWidth - mediaBrowserX - 8.0;

    self.mediaTabContentMedia =
        [[NSView alloc] initWithFrame:NSMakeRect(0.0, 0.0, mediaPanelWidth,
                                                 mediaContentHeight)];
    self.mediaTabContentMedia.autoresizingMask =
        NSViewWidthSizable | NSViewHeightSizable;
    [self.mediaPanel addSubview:self.mediaTabContentMedia];

    NSTextField* libraryTitle =
        [NSTextField labelWithString:@"PROJET / CHUTIERS"];
    libraryTitle.frame = NSMakeRect(14.0, mediaContentHeight - 34.0,
                                    mediaPanelWidth - 28.0, 18.0);
    libraryTitle.autoresizingMask = NSViewMinYMargin | NSViewWidthSizable;
    libraryTitle.font = [NSFont systemFontOfSize:11.0
                                          weight:NSFontWeightSemibold];
    libraryTitle.textColor = NSColor.secondaryLabelColor;
    [self.mediaTabContentMedia addSubview:libraryTitle];

    self.binOutline = [[ContextOutlineView alloc]
        initWithFrame:NSMakeRect(0.0, 0.0, binSidebarWidth,
                                 mediaContentHeight - 154.0)];
    NSTableColumn* binColumn =
        [[NSTableColumn alloc] initWithIdentifier:@"bin"];
    [self.binOutline addTableColumn:binColumn];
    self.binOutline.outlineTableColumn = binColumn;
    self.binOutline.headerView = nil;
    self.binOutline.dataSource = self;
    self.binOutline.delegate = self;
    self.binOutline.rowHeight = 26.0;
    self.binOutline.indentationPerLevel = 12.0;
    self.binOutline.backgroundColor = CMSurfacePanelColor();
    binColumn.width = binSidebarWidth;
    [self.binOutline registerForDraggedTypes:@[
        kCutmachineMediaPasteboardType, kCutmachineBinPasteboardType,
        kCutmachineTimelinePasteboardType, NSPasteboardTypeFileURL
    ]];
    [self.binOutline setDraggingSourceOperationMask:NSDragOperationMove
                                           forLocal:YES];
    NSScrollView* binScroll = [[NSScrollView alloc]
        initWithFrame:NSMakeRect(binSidebarX, 112.0, binSidebarWidth,
                                 mediaContentHeight - 154.0)];
    binScroll.autoresizingMask = NSViewHeightSizable | NSViewMaxXMargin;
    binScroll.documentView = self.binOutline;
    binScroll.hasVerticalScroller = YES;
    binScroll.borderType = NSNoBorder;
    binScroll.drawsBackground = YES;
    binScroll.backgroundColor = CMSurfacePanelColor();
    [self.mediaTabContentMedia addSubview:binScroll];

    self.mediaSearchField = [[NSSearchField alloc]
        initWithFrame:NSMakeRect(mediaBrowserX, mediaContentHeight - 70.0,
                                 mediaBrowserWidth - 80.0, 26.0)];
    self.mediaSearchField.placeholderString = @"Rechercher nom, codec, format…";
    self.mediaSearchField.target = self;
    self.mediaSearchField.action = @selector(mediaSearchChanged:);
    self.mediaSearchField.continuous = YES;
    self.mediaSearchField.autoresizingMask =
        NSViewMinYMargin | NSViewWidthSizable;
    [self.mediaTabContentMedia addSubview:self.mediaSearchField];

    self.mediaViewToggle = [[NSSegmentedControl alloc]
        initWithFrame:NSMakeRect(mediaPanelWidth - 80.0,
                                 mediaContentHeight - 70.0, 72.0, 26.0)];
    self.mediaViewToggle.segmentCount = 2;
    [self.mediaViewToggle setImage:SystemSymbol(@"list.bullet", @"Vue liste")
                        forSegment:0];
    [self.mediaViewToggle
          setImage:SystemSymbol(@"square.grid.2x2", @"Vue grille")
        forSegment:1];
    self.mediaViewToggle.selectedSegment = 1;
    self.mediaViewToggle.target = self;
    self.mediaViewToggle.action = @selector(mediaViewChanged:);
    self.mediaViewToggle.autoresizingMask = NSViewMinYMargin;
    self.mediaViewToggle.autoresizingMask = NSViewMinYMargin | NSViewMinXMargin;
    [self.mediaTabContentMedia addSubview:self.mediaViewToggle];

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
    self.mediaTable.rowHeight = 44.0;
    self.mediaTable.usesAlternatingRowBackgroundColors = NO;
    self.mediaListScroll = [[NSScrollView alloc]
        initWithFrame:NSMakeRect(mediaBrowserX, 112.0, mediaBrowserWidth,
                                 mediaContentHeight - 190.0)];
    self.mediaListScroll.autoresizingMask =
        NSViewHeightSizable | NSViewWidthSizable;
    self.mediaListScroll.documentView = self.mediaTable;
    self.mediaListScroll.hasVerticalScroller = YES;
    self.mediaListScroll.hasHorizontalScroller = YES;
    self.mediaListScroll.borderType = NSBezelBorder;
    [self.mediaTabContentMedia addSubview:self.mediaListScroll];

    NSCollectionViewFlowLayout* iconLayout =
        [[NSCollectionViewFlowLayout alloc] init];
    iconLayout.itemSize = NSMakeSize(132.0, 112.0);
    iconLayout.minimumInteritemSpacing = 6.0;
    iconLayout.minimumLineSpacing = 8.0;
    iconLayout.sectionInset = NSEdgeInsetsMake(8, 8, 8, 8);
    self.mediaCollection = [[ContextCollectionView alloc]
        initWithFrame:NSMakeRect(0.0, 0.0, mediaBrowserWidth, 500.0)];
    self.mediaCollection.collectionViewLayout = iconLayout;
    self.mediaCollection.dataSource = self;
    self.mediaCollection.delegate = self;
    self.mediaCollection.selectable = YES;
    self.mediaCollection.allowsMultipleSelection = YES;
    [self.mediaCollection
        setDraggingSourceOperationMask:kMediaLocalDragOperations
                              forLocal:YES];
    [self.mediaCollection setDraggingSourceOperationMask:NSDragOperationCopy
                                                forLocal:NO];
    [self.mediaCollection registerForDraggedTypes:@[
        kCutmachineMediaPasteboardType, kCutmachineBinPasteboardType,
        kCutmachineTimelinePasteboardType, NSPasteboardTypeFileURL
    ]];
    [self.mediaCollection registerClass:MediaIconItem.class
                  forItemWithIdentifier:@"media-icon"];
    self.mediaIconScroll =
        [[NSScrollView alloc] initWithFrame:self.mediaListScroll.frame];
    self.mediaIconScroll.autoresizingMask =
        NSViewHeightSizable | NSViewWidthSizable;
    self.mediaIconScroll.documentView = self.mediaCollection;
    self.mediaIconScroll.hasVerticalScroller = YES;
    self.mediaIconScroll.borderType = NSBezelBorder;
    self.mediaListScroll.hidden = YES;
    self.mediaIconScroll.hidden = NO;
    [self.mediaTabContentMedia addSubview:self.mediaIconScroll];

    self.mediaTable.target = self;
    self.mediaTable.doubleAction = @selector(openSelectedMediaInSourceMonitor:);
    ContextCollectionView* iconCollection =
        (ContextCollectionView*)self.mediaCollection;
    iconCollection.doubleClickTarget = self;
    iconCollection.doubleClickAction = @selector(openIconItem:);
    [self.mediaTable setDraggingSourceOperationMask:kMediaLocalDragOperations
                                           forLocal:YES];

    self.assignMediaButton = CMMakeStyledButton(@"IMPORTER DES RUSHES…", self,
                                                @selector(importMediaPressed:));
    self.assignMediaButton.frame = NSMakeRect(12.0, 76.0, 184.0, 28.0);
    self.assignMediaButton.autoresizingMask = NSViewMaxYMargin;
    self.assignMediaButton.image =
        SystemSymbol(@"square.and.arrow.down", @"Importer des rushes");
    self.assignMediaButton.imagePosition = NSImageLeading;
    [self.mediaTabContentMedia addSubview:self.assignMediaButton];

    self.sourceMonitorButton = CMMakeStyledButton(
        @"SOURCE", self, @selector(openSelectedMediaInSourceMonitor:));
    self.sourceMonitorButton.frame = NSMakeRect(202.0, 76.0, 106.0, 28.0);
    self.sourceMonitorButton.autoresizingMask = NSViewMaxYMargin;
    self.sourceMonitorButton.autoresizingMask = NSViewMinXMargin;
    self.sourceMonitorButton.image =
        SystemSymbol(@"rectangle.on.rectangle", @"Moniteur Source");
    self.sourceMonitorButton.imagePosition = NSImageLeading;
    self.sourceMonitorButton.toolTip =
        @"Ouvre le média sélectionné dans le moniteur source";
    [self.mediaTabContentMedia addSubview:self.sourceMonitorButton];

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
    [mediaContext
        addItem:[self menuItem:@"Nouvelle timeline avec la sélection"
                        action:@selector(createTimelineFromSelectedMedia:)
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
    [self.mediaTabContentMedia addSubview:self.binSummaryLabel];
    self.mediaTaskLabel = [NSTextField labelWithString:@"Tâches média : prêt"];
    self.mediaTaskLabel.frame =
        NSMakeRect(14.0, 13.0, mediaPanelWidth - 152.0, 18.0);
    self.mediaTaskLabel.autoresizingMask =
        NSViewMaxYMargin | NSViewWidthSizable;
    self.mediaTaskLabel.font = [NSFont systemFontOfSize:10.0];
    self.mediaTaskLabel.textColor = NSColor.tertiaryLabelColor;
    self.mediaTaskLabel.lineBreakMode = NSLineBreakByTruncatingMiddle;
    [self.mediaTabContentMedia addSubview:self.mediaTaskLabel];
    self.mediaTaskProgress = [[NSProgressIndicator alloc]
        initWithFrame:NSMakeRect(mediaPanelWidth - 132.0, 16.0, 88.0, 10.0)];
    self.mediaTaskProgress.autoresizingMask =
        NSViewMaxYMargin | NSViewMinXMargin;
    self.mediaTaskProgress.minValue = 0.0;
    self.mediaTaskProgress.maxValue = 100.0;
    self.mediaTaskProgress.doubleValue = 0.0;
    self.mediaTaskProgress.indeterminate = NO;
    [self.mediaTabContentMedia addSubview:self.mediaTaskProgress];
    self.mediaTaskCancelButton =
        CMMakeStyledButton(@"×", self, @selector(cancelDisplayedMediaTask:));
    self.mediaTaskCancelButton.frame =
        NSMakeRect(mediaPanelWidth - 40.0, 7.0, 28.0, 26.0);
    self.mediaTaskCancelButton.autoresizingMask =
        NSViewMaxYMargin | NSViewMinXMargin;
    self.mediaTaskCancelButton.toolTip = @"Annuler la tâche média active";
    self.mediaTaskCancelButton.enabled = NO;
    [self.mediaTabContentMedia addSubview:self.mediaTaskCancelButton];

    const double editorWidth =
        windowRect.size.width - mediaPanelWidth - rightDockWidth - 2.0;
    self.editorSplitView = [[CutmachineSplitView alloc]
        initWithFrame:NSMakeRect(0.0, 0.0, editorWidth, workspaceHeight)];
    self.editorSplitView.vertical = NO;
    self.editorSplitView.arrangesAllSubviews = NO;
    self.editorSplitView.dividerStyle = NSSplitViewDividerStyleThin;
    self.editorSplitView.delegate = self;
    // v5 retires every earlier key: v4 and before were written while the panes
    // were inserted in the reverse of their visual order and their frames
    // rewritten afterwards, so the saved coordinates describe a composition
    // this build no longer produces.
    self.editorSplitView.autosaveName =
        @"CUTMACHINE.EditorSplit.MonitorsTop.v5";
    self.editorSplitView.autoresizingMask =
        NSViewWidthSizable | NSViewHeightSizable;

    // NSSplitView is flipped: arranged subview 0 is the *top* pane of a
    // horizontal split and -setPosition: measures from the top edge. The
    // monitors therefore go in first and the timeline second -- the order
    // they appear on screen -- and AppKit needs no help holding them there.
    const double editorDivider = self.editorSplitView.dividerThickness;
    const double monitorPaneHeight = workspaceHeight * 0.44;
    const double timelinePaneTop = monitorPaneHeight + editorDivider;
    self.metalView = [[TimelineMetalView alloc]
        initWithFrame:NSMakeRect(0.0, timelinePaneTop, editorWidth,
                                 workspaceHeight - timelinePaneTop)];
    self.metalView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    self.metalView.eventTarget = self;
    self.metalView.resizeTarget = self;
    self.monitorSplitView = [[CutmachineSplitView alloc]
        initWithFrame:NSMakeRect(0.0, 0.0, editorWidth, monitorPaneHeight)];
    self.monitorSplitView.vertical = YES;
    self.monitorSplitView.dividerStyle = NSSplitViewDividerStyleThin;
    self.monitorSplitView.delegate = self;
    // The two viewers are one comparison surface: while Source is visible,
    // both panes always receive exactly half of the available width.
    self.monitorSplitView.autoresizingMask =
        NSViewWidthSizable | NSViewHeightSizable;
    self.sourceMonitorPanel =
        [[NSView alloc] initWithFrame:NSMakeRect(0.0, 0.0, editorWidth * 0.5,
                                                 monitorPaneHeight)];
    self.programMonitorPanel = [[NSView alloc]
        initWithFrame:NSMakeRect(editorWidth * 0.5, 0.0, editorWidth * 0.5,
                                 monitorPaneHeight)];
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
    // -adjustSubviews before every -setPosition:ofDividerAtIndex: below, and
    // never the other way round. Freshly added arranged subviews all still
    // sit at x/y 0 -- the split view has not placed them relative to each
    // other yet -- and setting a divider position against overlapping frames
    // makes AppKit resolve the conflict by collapsing panes to zero width.
    // -adjustSubviews lays them out first, so each -setPosition: is then a
    // small nudge from a valid arrangement rather than a guess from garbage.
    [self.monitorSplitView adjustSubviews];
    [self.monitorSplitView setPosition:editorWidth * 0.5 ofDividerAtIndex:0];
    // Monitors above, timeline below -- see the flipped-coordinates note next
    // to editorSplitView's autosaveName. Divider 0 is then the boundary the
    // -constrain{Min,Max}Coordinate: pair below already describes: at least
    // 180 pt of monitors above it, at least 200 pt of timeline under it.
    [self.editorSplitView insertArrangedSubview:self.monitorSplitView
                                        atIndex:0];
    [self.editorSplitView insertArrangedSubview:self.metalView atIndex:1];
    [self.editorSplitView setHoldingPriority:NSLayoutPriorityDefaultLow
                           forSubviewAtIndex:0];
    [self.editorSplitView setHoldingPriority:NSLayoutPriorityDefaultLow
                           forSubviewAtIndex:1];
    [self.editorSplitView adjustSubviews];
    [self.editorSplitView setPosition:monitorPaneHeight ofDividerAtIndex:0];
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

    // F2.4 -- ROADMAP.md: chat panel, wired to the exact same in-memory
    // Document/EditLog `self.state->editLog.Apply(self.state->document, ...)`
    // already mutates everywhere else in this file, through
    // McpLiveBackend -> McpToolRegistry::Call -- the identical dispatcher
    // McpServer.cc's `tools/call` uses for an external MCP client. No
    // second edit path: see McpLiveBackend.h/ChatSession.h for why. `state`
    // is allocated once, above, and never reallocated after the window is
    // built (only its fields are mutated in place by ordinary edits and
    // project loads), so the reference this backend holds stays valid for
    // the window's lifetime.
    __weak AppDelegate* weakSelf = self;
    self.chatBackend = new McpLiveBackend(
        self.state->document, self.state->editLog, [weakSelf]() {
            AppDelegate* strongSelf = weakSelf;
            if (!strongSelf) return;
            const RationalTime playhead = strongSelf.state->requestedPosition;
            std::string persistMessage;
            if (![strongSelf persistEdits:persistMessage]) {
                // McpLiveBackend owns references to the active objects and
                // applies before invoking this callback. The project and the
                // per-timeline log still hold the last committed generation,
                // so restore both when the disk commit fails.
                strongSelf.state->document =
                    strongSelf.state->project.MakeDocument(
                        strongSelf.state->activeTimelineId);
                const auto committedLog =
                    strongSelf.state->timelineEditLogs.find(
                        strongSelf.state->activeTimelineId);
                strongSelf.state->editLog =
                    committedLog == strongSelf.state->timelineEditLogs.end()
                        ? EditLog{}
                        : committedLog->second;
                std::fprintf(stderr, "Unable to persist chat-driven edit: %s\n",
                             persistMessage.c_str());
                strongSelf.infoLabel.stringValue = [NSString
                    stringWithFormat:@"Modification du chat annulée : %s",
                                     persistMessage.c_str()];
            }
            [strongSelf refreshTimelineAfterEditFromPosition:playhead];
            [strongSelf rebuildMediaList];
        });
    self.chatPanelView = [[CMChatPanelView alloc]
        initWithFrame:NSMakeRect(0.0, 0.0, rightDockWidth, workspaceHeight)];
    [self.chatPanelView configureWithBackend:*self.chatBackend];
    [self.rightDockPanel setContentView:self.chatPanelView
                                forSlot:ui::PanelSlot::Chat];

    [self.workspaceSplitView setHoldingPriority:NSLayoutPriorityDefaultHigh
                              forSubviewAtIndex:0];
    [self.workspaceSplitView setHoldingPriority:NSLayoutPriorityDefaultHigh
                              forSubviewAtIndex:2];
    [self.workspaceSplitView adjustSubviews];
    [self.workspaceSplitView setPosition:mediaPanelWidth ofDividerAtIndex:0];
    [self.workspaceSplitView setPosition:mediaPanelWidth + editorWidth
                        ofDividerAtIndex:1];

    self.infoLabel = [NSTextField labelWithString:@"Aucun clip sélectionné"];
    self.infoLabel.frame =
        NSMakeRect(20.0, 12.0, windowRect.size.width - 40.0, 18.0);
    self.infoLabel.autoresizingMask = NSViewWidthSizable;
    self.infoLabel.font =
        [NSFont monospacedDigitSystemFontOfSize:11.0
                                         weight:NSFontWeightRegular];
    self.infoLabel.textColor = CMThemeColor(ui::theme::kTextSecondary);
    self.infoLabel.lineBreakMode = NSLineBreakByTruncatingTail;
    [content addSubview:self.infoLabel];

    [self refreshBinControlsSelecting:@"__root__"];

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
        TimelineMetalView* monitor = label == self.programMonitorTitleLabel
                                         ? self.programMonitorView
                                         : self.sourceMonitorView;
        label.font = CMFont(ui::theme::kFontSizeSmall, NSFontWeightBold);
        label.drawsBackground = NO;
        label.frame = NSMakeRect(
            8.0,
            monitor.bounds.size.height - ui::theme::kPanelHeaderHeight - 4.0,
            monitor.bounds.size.width - 16.0, ui::theme::kPanelHeaderHeight);
        label.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;
    }
    self.sourceMonitorTitleLabel.textColor =
        CMThemeColor(ui::theme::kTextPrimary);
    self.programMonitorTitleLabel.textColor = NSColor.whiteColor;
    [self.sourceMonitorView addSubview:self.sourceMonitorTitleLabel];
    [self.programMonitorView addSubview:self.programMonitorTitleLabel];
    self.sourceMonitorZoneLabel =
        [NSTextField labelWithString:@"IN —    OUT —"];
    self.sourceMonitorZoneLabel.font =
        [NSFont monospacedDigitSystemFontOfSize:10.0 weight:NSFontWeightMedium];
    self.sourceMonitorZoneLabel.textColor =
        CMThemeColor(ui::theme::kTextSecondary);
    self.sourceMonitorZoneLabel.frame = NSMakeRect(10.0, 29.0, 300.0, 16.0);
    self.sourceMonitorZoneLabel.autoresizingMask =
        NSViewWidthSizable | NSViewMaxYMargin;
    [self.sourceMonitorView addSubview:self.sourceMonitorZoneLabel];
    NSButton* sourceInsertButton =
        CMMakeStyledButton(@"INSÉRER", self, @selector(menuInsertSource:));
    NSButton* sourceOverwriteButton =
        CMMakeStyledButton(@"ÉCRASER", self, @selector(menuOverwriteSource:));
    sourceInsertButton.frame = NSMakeRect(10.0, 49.0, 78.0, 24.0);
    sourceOverwriteButton.frame = NSMakeRect(92.0, 49.0, 82.0, 24.0);
    sourceInsertButton.image =
        SystemSymbol(@"arrow.down.to.line", @"Insérer depuis Source", 10.0);
    sourceOverwriteButton.image =
        SystemSymbol(@"square.on.square", @"Écraser depuis Source", 10.0);
    for (NSButton* button in @[ sourceInsertButton, sourceOverwriteButton ]) {
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
    self.programVideoScopePopup =
        [[NSPopUpButton alloc] initWithFrame:NSMakeRect(8.0, 4.0, 132.0, 24.0)
                                   pullsDown:NO];
    [self.programVideoScopePopup addItemsWithTitles:@[
        @"Aucun scope", @"Waveform", @"Parade RGB", @"Vectorscope"
    ]];
    for (NSInteger index = 0; index < self.programVideoScopePopup.numberOfItems;
         ++index)
        [self.programVideoScopePopup itemAtIndex:index].tag = index;
    [self.programVideoScopePopup
        selectItemWithTag:static_cast<NSInteger>(
                              self.state->programVideoScope)];
    self.programVideoScopePopup.target = self;
    self.programVideoScopePopup.action = @selector(programVideoScopeChanged:);
    self.programVideoScopePopup.controlSize = NSControlSizeSmall;
    self.programVideoScopePopup.font =
        [NSFont monospacedDigitSystemFontOfSize:10.0 weight:NSFontWeightMedium];
    self.programVideoScopePopup.autoresizingMask =
        NSViewMaxXMargin | NSViewMaxYMargin;
    self.programVideoScopePopup.toolTip =
        @"Afficher un scope de la sortie Record après grading";
    [self.programMonitorView addSubview:self.programVideoScopePopup];
    self.sourceMonitorToggleButton =
        CMMakeStyledButton(@"SOURCE ON", self, @selector(toggleSourceMonitor:));
    self.sourceMonitorToggleButton.state = NSControlStateValueOn;
    self.sourceMonitorToggleButton.frame = NSMakeRect(
        self.programMonitorView.bounds.size.width - 198.0, 4.0, 98.0, 24.0);
    self.sourceMonitorToggleButton.autoresizingMask =
        NSViewMinXMargin | NSViewMaxYMargin;
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
    [self refreshTimelineChrome];
    [self fitTimelineToViewportWidth];

    __weak AppDelegate* weakShortcutTarget = self;
    self.timelineShortcutMonitor = [NSEvent
        addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                     handler:^NSEvent*(NSEvent* event) {
                                       AppDelegate* target = weakShortcutTarget;
                                       if (!target ||
                                           event.window != target.window)
                                           return event;
                                       NSResponder* responder =
                                           target.window.firstResponder;
                                       if (![responder
                                               isKindOfClass:NSView.class])
                                           return event;
                                       NSView* focusedView = (NSView*)responder;
                                       if (![focusedView
                                               isDescendantOf:
                                                   target.inspectorView] ||
                                           [focusedView
                                               isKindOfClass:NSTextView.class])
                                           return event;
                                       // Preserve native Inspector control
                                       // navigation and adjustment. The
                                       // remaining keys use the same
                                       // configurable timeline dispatcher as
                                       // the Metal view, so this does not
                                       // create a second shortcut policy.
                                       switch (event.keyCode) {
                                           case 48:   // Tab
                                           case 53:   // Escape
                                           case 115:  // Home
                                           case 119:  // End
                                           case 123:  // Left
                                           case 124:  // Right
                                           case 125:  // Down
                                           case 126:  // Up
                                               return event;
                                           default:
                                               break;
                                       }
                                       return [target timelineKeyDown:event]
                                                  ? nil
                                                  : event;
                                     }];

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
#if defined(CUTMACHINE_UI_SMOKE_TEST)
    if (gUiSmokeTesting)
        dispatch_async(dispatch_get_main_queue(), ^{
          [self runUiSmokeTests];
        });
#endif
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

    if (self.offlineMediaLabel)
        self.offlineMediaLabel.frame =
            NSMakeRect(0.0, 0.0, self.programMonitorView.bounds.size.width,
                       self.programMonitorView.bounds.size.height);
    // Labels now live in the ordered Metal atlas display list. Keeping the
    // former AppKit overlays hidden avoids two coordinate systems drifting
    // apart when the timeline is resized.
    self.timelineTimecodeLabel.hidden = YES;
}

- (void)refreshBinControlsSelecting:(NSString*)selectedBinId {
    if (!self.binOutline || !self.mediaTable) return;
    NSString* requested = selectedBinId ?: self.selectedBinId ?: @"__all__";
    self.selectedBinId = requested;
    self.updatingBinControls = YES;
    [self.binOutline reloadData];
    [self.binOutline expandItem:nil expandChildren:YES];
    const NSInteger row = [self.binOutline rowForItem:requested];
    if (row >= 0)
        [self.binOutline selectRowIndexes:[NSIndexSet indexSetWithIndex:row]
                     byExtendingSelection:NO];
    self.updatingBinControls = NO;
    [self rebuildMediaList];
    // Bin selection/membership is shared between the Media and Audio tabs --
    // both filter the same document.library (see MediaPanelModel.h) -- so
    // keep the Audio tab's list current from this one central place.
    if (self.audioTable) [self rebuildAudioList];
}

- (void)binSelectionChanged:(id)sender {
    (void)sender;
    if (self.updatingBinControls) return;
    const NSInteger row = self.binOutline.selectedRow;
    if (row < 0) return;
    NSString* selected = [self.binOutline itemAtRow:row];
    if ([selected isEqualToString:self.selectedBinId]) return;
    self.selectedBinId = selected;
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
    if (!item) return 2;
    NSString* identifier = item;
    if ([identifier isEqualToString:@"__all__"]) return 0;
    if ([identifier isEqualToString:@"__root__"])
        return [self childBinIds:@""].count;
    return [self childBinIds:identifier].count;
}

- (id)outlineView:(NSOutlineView*)outlineView
            child:(NSInteger)index
           ofItem:(id)item {
    (void)outlineView;
    if (!item) {
        if (index == 0) return @"__all__";
        return @"__root__";
    }
    if ([item isEqualToString:@"__root__"])
        return [self childBinIds:@""][index];
    return [self childBinIds:item][index];
}

- (BOOL)outlineView:(NSOutlineView*)outlineView isItemExpandable:(id)item {
    (void)outlineView;
    NSString* identifier = item;
    if ([identifier isEqualToString:@"__all__"]) return NO;
    if ([identifier isEqualToString:@"__root__"])
        return [self childBinIds:@""].count > 0;
    return [self childBinIds:identifier].count > 0;
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
    if ([pasteboard stringForType:kCutmachineTimelinePasteboardType])
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
    NSString* timeline =
        [pasteboard stringForType:kCutmachineTimelinePasteboardType];
    NSString* movingBin =
        [pasteboard stringForType:kCutmachineBinPasteboardType];
    if (timeline) {
        Project candidate = self.state->project;
        ProjectEditLog projectLog = self.state->projectEditLog;
        EditError error = EditError::None;
        std::string message;
        if (!projectLog.Apply(
                candidate,
                ProjectOperation{SetProjectTimelineBinOperation{
                    timeline.UTF8String ?: "", target.UTF8String ?: ""}},
                error, message)) {
            self.binSummaryLabel.stringValue = [NSString
                stringWithFormat:@"Déplacement refusé (%s) : %s",
                                 EditErrorName(error), message.c_str()];
            return NO;
        }
        if (![self commitProjectCandidate:candidate
                                  editLog:self.state->editLog
                               projectLog:projectLog
                                  message:message]) {
            self.binSummaryLabel.stringValue =
                [NSString stringWithFormat:@"Enregistrement impossible : %s",
                                           message.c_str()];
            return NO;
        }
        self.state->project = std::move(candidate);
        self.state->projectEditLog = std::move(projectLog);
        self.state->lastHistoryDomain = HistoryDomain::Project;
        NSString* selected = target.length == 0 ? @"__root__" : target;
        [self refreshBinControlsSelecting:selected];
        return YES;
    }
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
    if (![self applyAndPersistTimelineOperation:std::move(operation)
                                          error:error
                                        message:message]) {
        self.binSummaryLabel.stringValue =
            [NSString stringWithFormat:@"Déplacement impossible (%s) : %s",
                                       EditErrorName(error), message.c_str()];
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
    NSTableCellView* cell = [[NSTableCellView alloc]
        initWithFrame:NSMakeRect(0.0, 0.0, tableColumn.width, 26.0)];
    NSImageView* icon =
        [[NSImageView alloc] initWithFrame:NSMakeRect(2.0, 6.0, 14.0, 14.0)];
    icon.imageScaling = NSImageScaleProportionallyUpOrDown;
    NSTextField* label = [NSTextField labelWithString:@""];
    label.frame = NSMakeRect(
        21.0, 4.0, std::max<CGFloat>(20.0, tableColumn.width - 23.0), 18.0);
    label.autoresizingMask = NSViewWidthSizable;
    label.font = CMFont(ui::theme::kFontSizeSmall, NSFontWeightMedium);
    label.textColor = CMTextSecondaryColor();
    NSString* identifier = item;
    if ([identifier isEqualToString:@"__all__"]) {
        label.stringValue = @"Tous";
        icon.image = SystemSymbol(@"square.grid.2x2", @"Tous les objets", 12.0);
        icon.contentTintColor = CMThemeColor(ui::theme::kTextTertiary);
    } else if ([identifier isEqualToString:@"__root__"]) {
        label.stringValue = @"Projet";
        icon.image = SystemSymbol(@"folder", @"Racine du projet", 12.0);
        icon.contentTintColor = CMAccentColor();
    } else {
        const DocumentBin* bin =
            self.state->document.FindBin(identifier.UTF8String ?: "");
        label.stringValue =
            bin ? [NSString stringWithUTF8String:bin->name.c_str()]
                : @"Chutier manquant";
        icon.image = SystemSymbol(@"folder", @"Chutier", 12.0);
        icon.contentTintColor = CMAccentColor();
        if (bin) {
            label.editable = YES;
            label.selectable = YES;
            label.delegate = self;
            label.identifier = [@"bin:" stringByAppendingString:identifier];
        }
    }
    label.lineBreakMode = NSLineBreakByTruncatingTail;
    cell.imageView = icon;
    cell.textField = label;
    [cell addSubview:icon];
    [cell addSubview:label];
    return cell;
}

- (void)outlineViewSelectionDidChange:(NSNotification*)notification {
    if (notification.object == self.binOutline)
        [self binSelectionChanged:self.binOutline];
}

- (void)rebuildMediaList {
    self.visibleMediaIds = [NSMutableArray array];
    NSString* query = self.mediaSearchField.stringValue.lowercaseString ?: @"";
    const std::string selected(self.selectedBinId.UTF8String ?: "__all__");
    if (selected != "__all__") {
        const Ulid parent = selected == "__root__" ? Ulid{} : selected;
        const auto childBins = ui::media_panel::DirectChildBins(
            self.state->document.bins, parent, query.UTF8String ?: "");
        for (const DocumentBin* bin : childBins)
            [self.visibleMediaIds
                addObject:[NSString stringWithUTF8String:bin->id.c_str()]];
    }
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
        const std::string displayName =
            RushDisplayName(self.state->project, media);
        NSString* searchable = [NSString
            stringWithFormat:@"%s %s %s %s %s %dx%d", displayName.c_str(),
                             media.filename.c_str(), media.path.c_str(),
                             media.codec.c_str(), media.orientation.c_str(),
                             media.width, media.height];
        if (query.length > 0 &&
            [searchable.lowercaseString rangeOfString:query].location ==
                NSNotFound)
            continue;
        mediaItems.push_back(&media);
    }
    const Project* project = &self.state->project;
    std::stable_sort(
        mediaItems.begin(), mediaItems.end(),
        [project](const LibraryMedia* left, const LibraryMedia* right) {
            return RushDisplayName(*project, *left) <
                   RushDisplayName(*project, *right);
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

- (NSString*)selectedBrowserObjectId {
    if (self.mediaViewToggle.selectedSegment == 1) {
        NSIndexPath* path = self.mediaCollection.selectionIndexPaths.anyObject;
        return path && path.item < self.visibleMediaIds.count
                   ? self.visibleMediaIds[path.item]
                   : nil;
    }
    const NSInteger row = self.mediaTable.selectedRow;
    return row >= 0 && row < (NSInteger)self.visibleMediaIds.count
               ? self.visibleMediaIds[row]
               : nil;
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
        for (NSIndexPath* path in paths) {
            if (path.item >= self.visibleMediaIds.count) continue;
            NSString* identifier = self.visibleMediaIds[path.item];
            if (!self.state->document.FindBin(identifier.UTF8String ?: ""))
                [identifiers addObject:identifier];
        }
    } else {
        [self.mediaTable.selectedRowIndexes
            enumerateIndexesUsingBlock:^(NSUInteger row, BOOL* stop) {
              (void)stop;
              if (row >= self.visibleMediaIds.count) return;
              NSString* identifier = self.visibleMediaIds[row];
              if (!self.state->document.FindBin(identifier.UTF8String ?: ""))
                  [identifiers addObject:identifier];
            }];
    }
    return identifiers;
}

// ---- F2.3 Media panel: Média/Audio/Légendes tab switching -----------------
// See MediaPanelModel.h -- ui::media_panel::Tab is the pure-C++ source of
// truth for which tab exists and in what order; this method only toggles
// AppKit visibility and lazily rebuilds whichever tab's content just became
// visible (nothing here mutates the Document).

- (void)mediaTabChanged:(CMTabStripView*)sender {
    const NSInteger index = sender.selectedIndex;
    const auto& tabs = ui::media_panel::AllTabs();
    if (index < 0 || index >= static_cast<NSInteger>(tabs.size())) return;
    const ui::media_panel::Tab tab = tabs[index];
    self.mediaTabContentMedia.hidden = tab != ui::media_panel::Tab::Media;
    self.mediaTabContentAudio.hidden = tab != ui::media_panel::Tab::Audio;
    self.mediaTabContentCaptions.hidden = tab != ui::media_panel::Tab::Captions;
    [NSUserDefaults.standardUserDefaults setInteger:index
                                             forKey:@"ui.mediaPanel."
                                                    @"lastActiveTab"];
    switch (tab) {
        case ui::media_panel::Tab::Media:
            [self rebuildMediaList];
            break;
        case ui::media_panel::Tab::Audio:
            [self rebuildAudioList];
            break;
        case ui::media_panel::Tab::Captions:
            [self rebuildCaptionStylesList];
            break;
    }
}

// ---- F2.3 Media panel: Audio tab -------------------------------------------
// A restyled, filtered view onto the same document.library the Media tab
// already browses -- see MediaPanelModel.h's file comment on why
// "audio-capable" (LibraryMedia::has_audio) stands in for "audio-only
// source" here. No ingest logic is duplicated: this tab reads the library
// Ingest.cc already populated and mutates it only through the same
// SetMediaBinOperation the Media tab already applies via EditLog::Apply.

- (void)buildAudioTabContentWithWidth:(double)width height:(double)height {
    self.mediaTabContentAudio =
        [[NSView alloc] initWithFrame:NSMakeRect(0.0, 0.0, width, height)];
    self.mediaTabContentAudio.autoresizingMask =
        NSViewWidthSizable | NSViewHeightSizable;
    [self.mediaPanel addSubview:self.mediaTabContentAudio];

    NSTextField* audioTitle = CMMakeSectionHeader(@"Sources audio");
    audioTitle.frame = NSMakeRect(14.0, height - 30.0, width - 28.0, 18.0);
    audioTitle.autoresizingMask = NSViewMinYMargin | NSViewWidthSizable;
    [self.mediaTabContentAudio addSubview:audioTitle];

    self.audioSearchField = [[NSSearchField alloc]
        initWithFrame:NSMakeRect(12.0, height - 66.0, width - 24.0, 26.0)];
    self.audioSearchField.placeholderString = @"Rechercher nom, codec…";
    self.audioSearchField.target = self;
    self.audioSearchField.action = @selector(audioSearchChanged:);
    self.audioSearchField.continuous = YES;
    self.audioSearchField.autoresizingMask =
        NSViewMinYMargin | NSViewWidthSizable;
    [self.mediaTabContentAudio addSubview:self.audioSearchField];

    self.audioTable = [[ContextTableView alloc]
        initWithFrame:NSMakeRect(0.0, 0.0, width - 24.0, 500.0)];
    for (NSArray<NSString*>* definition in @[
             @[ @"name", @"Nom", @"140" ], @[ @"format", @"Format", @"90" ],
             @[ @"duration", @"Durée", @"60" ]
         ]) {
        NSTableColumn* column =
            [[NSTableColumn alloc] initWithIdentifier:definition[0]];
        column.title = definition[1];
        column.width = definition[2].doubleValue;
        [self.audioTable addTableColumn:column];
    }
    self.audioTable.dataSource = self;
    self.audioTable.delegate = self;
    self.audioTable.allowsMultipleSelection = YES;
    self.audioTable.usesAlternatingRowBackgroundColors = YES;
    self.audioTable.target = self;
    self.audioTable.doubleAction = @selector(openSelectedAudioInSourceMonitor:);
    self.audioScroll = [[NSScrollView alloc]
        initWithFrame:NSMakeRect(12.0, 78.0, width - 24.0, height - 174.0)];
    self.audioScroll.autoresizingMask =
        NSViewHeightSizable | NSViewWidthSizable;
    self.audioScroll.documentView = self.audioTable;
    self.audioScroll.hasVerticalScroller = YES;
    self.audioScroll.hasHorizontalScroller = YES;
    self.audioScroll.borderType = NSBezelBorder;
    [self.mediaTabContentAudio addSubview:self.audioScroll];

    NSButton* openAudioButton =
        [NSButton buttonWithTitle:@"Source"
                           target:self
                           action:@selector(openSelectedAudioInSourceMonitor:)];
    openAudioButton.frame = NSMakeRect(12.0, 44.0, 140.0, 28.0);
    openAudioButton.autoresizingMask = NSViewMaxYMargin;
    openAudioButton.bezelStyle = NSBezelStyleRounded;
    openAudioButton.image =
        SystemSymbol(@"rectangle.on.rectangle", @"Moniteur Source");
    openAudioButton.imagePosition = NSImageLeading;
    [self.mediaTabContentAudio addSubview:openAudioButton];

    NSButton* assignAudioButton =
        [NSButton buttonWithTitle:@"Déplacer…"
                           target:self
                           action:@selector(assignSelectedAudioToBinPressed:)];
    assignAudioButton.frame = NSMakeRect(160.0, 44.0, 148.0, 28.0);
    assignAudioButton.autoresizingMask = NSViewMaxYMargin | NSViewMinXMargin;
    assignAudioButton.bezelStyle = NSBezelStyleRounded;
    assignAudioButton.image =
        SystemSymbol(@"folder", @"Déplacer vers un chutier");
    assignAudioButton.imagePosition = NSImageLeading;
    [self.mediaTabContentAudio addSubview:assignAudioButton];

    self.audioSummaryLabel = [NSTextField labelWithString:@""];
    self.audioSummaryLabel.frame = NSMakeRect(14.0, 12.0, width - 28.0, 24.0);
    self.audioSummaryLabel.autoresizingMask =
        NSViewMaxYMargin | NSViewWidthSizable;
    self.audioSummaryLabel.font = [NSFont systemFontOfSize:11.0];
    self.audioSummaryLabel.textColor = NSColor.secondaryLabelColor;
    self.audioSummaryLabel.maximumNumberOfLines = 2;
    [self.mediaTabContentAudio addSubview:self.audioSummaryLabel];
}

- (void)audioSearchChanged:(id)sender {
    (void)sender;
    [self rebuildAudioList];
}

- (void)rebuildAudioList {
    if (!self.state) return;
    const std::string selected(self.selectedBinId.UTF8String ?: "__all__");
    const bool anyBin = selected == "__all__";
    const bool wantRoot = selected == "__root__";
    const Ulid wantBinId = anyBin || wantRoot ? Ulid{} : selected;
    const std::string search(self.audioSearchField.stringValue.UTF8String
                                 ?: "");
    const auto matches = ui::media_panel::FilterAudioSources(
        self.state->document.library, anyBin, wantRoot, wantBinId, search);
    self.visibleAudioIds = [NSMutableArray array];
    for (const LibraryMedia* media : matches)
        [self.visibleAudioIds
            addObject:[NSString stringWithUTF8String:media->id.c_str()]];
    [self.audioTable reloadData];
    const NSUInteger count = self.visibleAudioIds.count;
    self.audioSummaryLabel.stringValue =
        [NSString stringWithFormat:@"%lu source%@ audio", (unsigned long)count,
                                   count == 1 ? @"" : @"s"];
}

- (NSString*)selectedAudioId {
    return [self selectedAudioIds].firstObject;
}

- (NSArray<NSString*>*)selectedAudioIds {
    NSMutableArray<NSString*>* identifiers = [NSMutableArray array];
    [self.audioTable.selectedRowIndexes
        enumerateIndexesUsingBlock:^(NSUInteger row, BOOL* stop) {
          (void)stop;
          if (row < self.visibleAudioIds.count)
              [identifiers addObject:self.visibleAudioIds[row]];
        }];
    return identifiers;
}

- (void)openSelectedAudioInSourceMonitor:(id)sender {
    (void)sender;
    NSString* identifier = [self selectedAudioId];
    if (!identifier) return;
    [self openMediaIdentifierInSourceMonitor:identifier];
}

- (void)assignSelectedAudioToBinPressed:(id)sender {
    (void)sender;
    [self moveMediaIdsToBin:[self selectedAudioIds]];
}

// ---- F2.3 Media panel: Captions tab ----------------------------------------
// Lists sequence.caption_styles (F0.2) and lets the user apply one to every
// clip currently selected on the timeline, or clear the selection's
// caption. Every mutation goes through the existing
// AddCaptionStyleOperation/RemoveCaptionStyleOperation/SetClipCaptionOperation
// via EditLog::Apply -- see MediaPanelModel.h for the pure logic
// (SummarizeCaptionStyles/JoinClipToCaptionStyle/ClearClipCaption) this
// method calls into.

- (void)buildCaptionsTabContentWithWidth:(double)width height:(double)height {
    self.mediaTabContentCaptions =
        [[NSView alloc] initWithFrame:NSMakeRect(0.0, 0.0, width, height)];
    self.mediaTabContentCaptions.autoresizingMask =
        NSViewWidthSizable | NSViewHeightSizable;
    [self.mediaPanel addSubview:self.mediaTabContentCaptions];

    NSTextField* stylesTitle = CMMakeSectionHeader(@"Styles de légende");
    stylesTitle.frame = NSMakeRect(14.0, height - 30.0, width - 28.0, 18.0);
    stylesTitle.autoresizingMask = NSViewMinYMargin | NSViewWidthSizable;
    [self.mediaTabContentCaptions addSubview:stylesTitle];

    self.captionStyleTable = [[ContextTableView alloc]
        initWithFrame:NSMakeRect(0.0, 0.0, width - 24.0, 220.0)];
    for (NSArray<NSString*>* definition in
         @[ @[ @"style", @"Style", @"200" ], @[ @"clips", @"Clips", @"56" ] ]) {
        NSTableColumn* column =
            [[NSTableColumn alloc] initWithIdentifier:definition[0]];
        column.title = definition[1];
        column.width = definition[2].doubleValue;
        [self.captionStyleTable addTableColumn:column];
    }
    self.captionStyleTable.dataSource = self;
    self.captionStyleTable.delegate = self;
    self.captionStyleTable.usesAlternatingRowBackgroundColors = YES;
    self.captionStyleScroll = [[NSScrollView alloc]
        initWithFrame:NSMakeRect(12.0, height - 260.0, width - 24.0, 220.0)];
    self.captionStyleScroll.autoresizingMask =
        NSViewMinYMargin | NSViewWidthSizable;
    self.captionStyleScroll.documentView = self.captionStyleTable;
    self.captionStyleScroll.hasVerticalScroller = YES;
    self.captionStyleScroll.borderType = NSBezelBorder;
    [self.mediaTabContentCaptions addSubview:self.captionStyleScroll];

    self.addCaptionStyleButton =
        [NSButton buttonWithTitle:@"+ Style"
                           target:self
                           action:@selector(createCaptionStylePressed:)];
    self.addCaptionStyleButton.frame =
        NSMakeRect(12.0, height - 292.0, 140.0, 28.0);
    self.addCaptionStyleButton.autoresizingMask = NSViewMinYMargin;
    self.addCaptionStyleButton.bezelStyle = NSBezelStyleRounded;
    self.addCaptionStyleButton.image =
        SystemSymbol(@"plus.circle", @"Nouveau style de légende");
    self.addCaptionStyleButton.imagePosition = NSImageLeading;
    [self.mediaTabContentCaptions addSubview:self.addCaptionStyleButton];

    self.removeCaptionStyleButton =
        [NSButton buttonWithTitle:@"− Style"
                           target:self
                           action:@selector(deleteCaptionStylePressed:)];
    self.removeCaptionStyleButton.frame =
        NSMakeRect(160.0, height - 292.0, 148.0, 28.0);
    self.removeCaptionStyleButton.autoresizingMask =
        NSViewMinYMargin | NSViewMinXMargin;
    self.removeCaptionStyleButton.bezelStyle = NSBezelStyleRounded;
    self.removeCaptionStyleButton.image =
        SystemSymbol(@"minus.circle", @"Supprimer le style");
    self.removeCaptionStyleButton.imagePosition = NSImageLeading;
    [self.mediaTabContentCaptions addSubview:self.removeCaptionStyleButton];

    NSTextField* selectionTitle = CMMakeSectionHeader(@"Sélection timeline");
    selectionTitle.frame = NSMakeRect(14.0, height - 320.0, width - 28.0, 18.0);
    selectionTitle.autoresizingMask = NSViewMinYMargin | NSViewWidthSizable;
    [self.mediaTabContentCaptions addSubview:selectionTitle];

    self.captionSelectionLabel = [NSTextField
        labelWithString:@"Aucun clip sélectionné sur la timeline."];
    self.captionSelectionLabel.frame =
        NSMakeRect(14.0, height - 368.0, width - 28.0, 44.0);
    self.captionSelectionLabel.autoresizingMask =
        NSViewMinYMargin | NSViewWidthSizable;
    self.captionSelectionLabel.font = [NSFont systemFontOfSize:11.0];
    self.captionSelectionLabel.textColor = NSColor.secondaryLabelColor;
    self.captionSelectionLabel.maximumNumberOfLines = 3;
    [self.mediaTabContentCaptions addSubview:self.captionSelectionLabel];

    self.applyCaptionStyleButton =
        [NSButton buttonWithTitle:@"Appliquer le style"
                           target:self
                           action:@selector(applyCaptionStylePressed:)];
    self.applyCaptionStyleButton.frame =
        NSMakeRect(12.0, height - 404.0, 140.0, 28.0);
    self.applyCaptionStyleButton.autoresizingMask = NSViewMinYMargin;
    self.applyCaptionStyleButton.bezelStyle = NSBezelStyleRounded;
    self.applyCaptionStyleButton.image =
        SystemSymbol(@"text.bubble", @"Appliquer le style de légende");
    self.applyCaptionStyleButton.imagePosition = NSImageLeading;
    [self.mediaTabContentCaptions addSubview:self.applyCaptionStyleButton];

    self.clearCaptionButton =
        [NSButton buttonWithTitle:@"Retirer"
                           target:self
                           action:@selector(clearCaptionPressed:)];
    self.clearCaptionButton.frame =
        NSMakeRect(160.0, height - 404.0, 148.0, 28.0);
    self.clearCaptionButton.autoresizingMask =
        NSViewMinYMargin | NSViewMinXMargin;
    self.clearCaptionButton.bezelStyle = NSBezelStyleRounded;
    self.clearCaptionButton.image =
        SystemSymbol(@"text.badge.xmark", @"Retirer la légende");
    self.clearCaptionButton.imagePosition = NSImageLeading;
    [self.mediaTabContentCaptions addSubview:self.clearCaptionButton];
}

- (void)rebuildCaptionStylesList {
    if (!self.state) return;
    const auto summaries =
        ui::media_panel::SummarizeCaptionStyles(self.state->document.sequence);
    self.visibleCaptionStyleIds = [NSMutableArray array];
    for (const auto& summary : summaries)
        [self.visibleCaptionStyleIds
            addObject:[NSString stringWithUTF8String:summary.style_id.c_str()]];
    [self.captionStyleTable reloadData];
    [self updateCaptionSelectionLabel];
}

- (void)updateCaptionSelectionLabel {
    if (!self.state || !self.state->interaction) {
        self.captionSelectionLabel.stringValue = @"Aucune timeline chargée.";
        return;
    }
    const std::vector<Ulid>& selected =
        self.state->interaction->SelectedClipIds();
    if (selected.empty()) {
        self.captionSelectionLabel.stringValue =
            @"Aucun clip sélectionné sur la timeline.";
        return;
    }
    size_t withCaption = 0;
    for (const Ulid& clipId : selected) {
        const DocumentClip* clip = self.state->document.FindClip(clipId);
        if (clip && !clip->caption_group_id.empty()) ++withCaption;
    }
    self.captionSelectionLabel.stringValue = [NSString
        stringWithFormat:@"%lu clip%@ sélectionné%@ · %lu avec légende",
                         (unsigned long)selected.size(),
                         selected.size() == 1 ? @"" : @"s",
                         selected.size() == 1 ? @"" : @"s",
                         (unsigned long)withCaption];
}

- (Ulid)selectedCaptionStyleId {
    const NSInteger row = self.captionStyleTable.selectedRow;
    if (row < 0 ||
        row >= static_cast<NSInteger>(self.visibleCaptionStyleIds.count))
        return {};
    return Ulid(self.visibleCaptionStyleIds[row].UTF8String ?: "");
}

- (void)createCaptionStylePressed:(id)sender {
    (void)sender;
    if (!self.state) return;
    EditError error = EditError::None;
    std::string message;
    if (![self applyAndPersistTimelineOperation:Operation {
            AddCaptionStyleOperation { CaptionStyle{}, -1 }
        }
                                          error:error
                                        message:message]) {
        self.captionSelectionLabel.stringValue = [NSString
            stringWithFormat:@"Création refusée : %s", message.c_str()];
        return;
    }
    [self rebuildCaptionStylesList];
}

- (void)deleteCaptionStylePressed:(id)sender {
    (void)sender;
    if (!self.state) return;
    const Ulid styleId = [self selectedCaptionStyleId];
    if (styleId.empty()) return;
    EditError error = EditError::None;
    std::string message;
    if (![self applyAndPersistTimelineOperation:Operation {
            RemoveCaptionStyleOperation { styleId }
        }
                                          error:error
                                        message:message]) {
        self.captionSelectionLabel.stringValue = [NSString
            stringWithFormat:@"Suppression refusée : %s", message.c_str()];
        return;
    }
    [self rebuildCaptionStylesList];
}

- (void)applyCaptionStylePressed:(id)sender {
    (void)sender;
    if (!self.state || !self.state->interaction) return;
    const Ulid styleId = [self selectedCaptionStyleId];
    if (styleId.empty()) return;
    const std::vector<Ulid> selected =
        self.state->interaction->SelectedClipIds();
    if (selected.empty()) return;
    Document stagedDocument = self.state->document;
    EditLog stagedLog = self.state->editLog;
    EditError error = EditError::None;
    std::string message;
    const RationalTime playhead = self.state->requestedPosition;
    for (const Ulid& clipId : selected) {
        const DocumentClip* clip = stagedDocument.FindClip(clipId);
        const std::string existingText =
            (clip && clip->caption_group_id == styleId) ? clip->caption_text
                                                        : std::string{};
        if (!stagedLog.Apply(stagedDocument,
                             Operation{ui::media_panel::JoinClipToCaptionStyle(
                                 clipId, styleId, existingText)},
                             error, message)) {
            self.captionSelectionLabel.stringValue = [NSString
                stringWithFormat:@"Application refusée : %s", message.c_str()];
            return;
        }
    }
    if (![self persistStagedDocument:stagedDocument
                             editLog:stagedLog
                             message:message]) {
        self.captionSelectionLabel.stringValue =
            [NSString stringWithFormat:@"Enregistrement impossible : %s",
                                       message.c_str()];
        return;
    }
    self.state->document = std::move(stagedDocument);
    self.state->editLog = std::move(stagedLog);
    self.state->lastHistoryDomain = HistoryDomain::Timeline;
    [self refreshTimelineAfterEditFromPosition:playhead];
    self.state->overlayDirty = true;
    [self rebuildCaptionStylesList];
}

- (void)clearCaptionPressed:(id)sender {
    (void)sender;
    if (!self.state || !self.state->interaction) return;
    const std::vector<Ulid> selected =
        self.state->interaction->SelectedClipIds();
    if (selected.empty()) return;
    Document stagedDocument = self.state->document;
    EditLog stagedLog = self.state->editLog;
    EditError error = EditError::None;
    std::string message;
    const RationalTime playhead = self.state->requestedPosition;
    for (const Ulid& clipId : selected) {
        if (!stagedLog.Apply(
                stagedDocument,
                Operation{ui::media_panel::ClearClipCaption(clipId)}, error,
                message)) {
            self.captionSelectionLabel.stringValue = [NSString
                stringWithFormat:@"Retrait refusé : %s", message.c_str()];
            return;
        }
    }
    if (![self persistStagedDocument:stagedDocument
                             editLog:stagedLog
                             message:message]) {
        self.captionSelectionLabel.stringValue =
            [NSString stringWithFormat:@"Enregistrement impossible : %s",
                                       message.c_str()];
        return;
    }
    self.state->document = std::move(stagedDocument);
    self.state->editLog = std::move(stagedLog);
    self.state->lastHistoryDomain = HistoryDomain::Timeline;
    [self refreshTimelineAfterEditFromPosition:playhead];
    self.state->overlayDirty = true;
    [self rebuildCaptionStylesList];
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
    item.representedObject = identifier;
    ((MediaIconView*)item.view).indexPath = indexPath;
    item.textField.editable = YES;
    item.textField.selectable = YES;
    item.textField.delegate = self;
    item.textField.identifier =
        [@"browser:" stringByAppendingString:identifier];
    const DocumentBin* bin =
        self.state->document.FindBin(identifier.UTF8String ?: "");
    if (bin) {
        item.textField.stringValue =
            [NSString stringWithUTF8String:bin->name.c_str()];
        item.imageView.image = SystemSymbol(@"folder.fill", @"Chutier", 38.0);
        item.imageView.contentTintColor = CMThemeColor(ui::theme::kAccent);
        item.imageView.layer.backgroundColor = NSColor.clearColor.CGColor;
        item.view.toolTip = @"Double-cliquer pour ouvrir ce chutier";
        return item;
    }
    const DocumentSequence* timeline =
        self.state->project.FindTimeline(identifier.UTF8String ?: "");
    if (timeline) {
        const DocumentSequence& sequence = *timeline;
        item.textField.stringValue =
            [NSString stringWithUTF8String:sequence.name.c_str()];
        item.imageView.image =
            SystemSymbol(@"rectangle.stack.fill", @"Séquence", 34.0);
        item.imageView.contentTintColor = CMAccentColor();
        item.imageView.layer.backgroundColor = NSColor.clearColor.CGColor;
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
        stringWithUTF8String:RushDisplayName(self.state->project, *media)
                                 .c_str()];
    const bool offline = self.state->offlineSourceIds.count(media->id) != 0;
    NSImage* thumbnail = offline ? nil : self.mediaThumbnails[identifier];
    if (thumbnail) {
        item.imageView.contentTintColor = nil;
        item.imageView.image = thumbnail;
    } else {
        NSImage* symbol = [NSImage
            imageWithSystemSymbolName:(offline ? @"exclamationmark.triangle"
                                               : @"film")
             accessibilityDescription:(offline ? @"Média offline"
                                               : @"Média vidéo")];
        symbol.size = NSMakeSize(44.0, 44.0);
        item.imageView.image = symbol;
        item.imageView.contentTintColor =
            offline ? CMThemeColor(ui::theme::kError) : CMTextSecondaryColor();
    }
    item.imageView.layer.backgroundColor = CMSurfaceRaisedColor().CGColor;
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
    if (self.state->document.FindBin(
            self.visibleMediaIds[indexPath.item].UTF8String ?: ""))
        return nil;
    NSPasteboardItem* item = [[NSPasteboardItem alloc] init];
    NSString* identifier = self.visibleMediaIds[indexPath.item];
    const NSPasteboardType type =
        self.state->project.FindTimeline(identifier.UTF8String ?: "")
            ? kCutmachineTimelinePasteboardType
            : kCutmachineMediaPasteboardType;
    [item setString:identifier forType:type];
    return item;
}

- (BOOL)collectionView:(NSCollectionView*)collectionView
    canDragItemsAtIndexPaths:(NSSet<NSIndexPath*>*)indexPaths
                   withEvent:(NSEvent*)event {
    (void)collectionView;
    (void)event;
    for (NSIndexPath* path in indexPaths) {
        if (path.item >= self.visibleMediaIds.count) continue;
        const std::string identifier(self.visibleMediaIds[path.item].UTF8String
                                         ?: "");
        if (self.state->document.FindLibraryMedia(identifier) ||
            self.state->project.FindTimeline(identifier))
            return YES;
    }
    return NO;
}

- (NSString*)iconGridDropTargetAtIndexPath:(NSIndexPath*)indexPath
                             dropOperation:
                                 (NSCollectionViewDropOperation)dropOperation {
    if (dropOperation == NSCollectionViewDropOn && indexPath &&
        indexPath.item < self.visibleMediaIds.count) {
        NSString* identifier = self.visibleMediaIds[indexPath.item];
        if (self.state->document.FindBin(identifier.UTF8String ?: ""))
            return identifier;
    }
    if (self.state->document.FindBin(self.selectedBinId.UTF8String ?: ""))
        return self.selectedBinId;
    return @"";
}

- (NSDragOperation)collectionView:(NSCollectionView*)collectionView
                     validateDrop:(id<NSDraggingInfo>)draggingInfo
                proposedIndexPath:(NSIndexPath**)proposedDropIndexPath
                    dropOperation:
                        (NSCollectionViewDropOperation*)proposedDropOperation {
    if (collectionView != self.mediaCollection || !proposedDropIndexPath ||
        !*proposedDropIndexPath)
        return NSDragOperationNone;
    NSString* target =
        [self iconGridDropTargetAtIndexPath:*proposedDropIndexPath
                              dropOperation:*proposedDropOperation];

    NSPasteboard* pasteboard = draggingInfo.draggingPasteboard;
    NSString* movingMedia =
        [pasteboard stringForType:kCutmachineMediaPasteboardType];
    if (movingMedia) {
        const LibraryMedia* media =
            self.state->document.FindLibraryMedia(movingMedia.UTF8String ?: "");
        if (!media || media->bin_id == (target.UTF8String ?: ""))
            return NSDragOperationNone;
        return NSDragOperationMove;
    }
    NSString* movingTimeline =
        [pasteboard stringForType:kCutmachineTimelinePasteboardType];
    if (movingTimeline) {
        const Ulid timelineId(movingTimeline.UTF8String ?: "");
        if (!self.state->project.FindTimeline(timelineId))
            return NSDragOperationNone;
        const auto placement =
            self.state->project.timeline_bin_ids.find(timelineId);
        const Ulid current =
            placement == self.state->project.timeline_bin_ids.end()
                ? Ulid{}
                : placement->second;
        return current == (target.UTF8String ?: "") ? NSDragOperationNone
                                                    : NSDragOperationMove;
    }
    NSString* movingBin =
        [pasteboard stringForType:kCutmachineBinPasteboardType];
    if (movingBin) {
        if (![self bin:movingBin canMoveInto:target])
            return NSDragOperationNone;
        return NSDragOperationMove;
    }
    if ([pasteboard availableTypeFromArray:@[ NSPasteboardTypeFileURL ]])
        return NSDragOperationCopy;
    return NSDragOperationNone;
}

- (BOOL)collectionView:(NSCollectionView*)collectionView
            acceptDrop:(id<NSDraggingInfo>)draggingInfo
             indexPath:(NSIndexPath*)indexPath
         dropOperation:(NSCollectionViewDropOperation)dropOperation {
    if (collectionView != self.mediaCollection) return NO;
    NSString* target = [self iconGridDropTargetAtIndexPath:indexPath
                                             dropOperation:dropOperation];
    // Reuse the sidebar's operation-backed drop path: media classification,
    // bin nesting, Finder ingest, persistence, and refresh stay identical for
    // both representations of the same project architecture.
    return [self outlineView:self.binOutline
                  acceptDrop:draggingInfo
                        item:target.length > 0 ? target : @"__root__"
                  childIndex:NSOutlineViewDropOnItemIndex];
}

- (BOOL)tableView:(NSTableView*)tableView
    writeRowsWithIndexes:(NSIndexSet*)rowIndexes
            toPasteboard:(NSPasteboard*)pasteboard {
    if (tableView != self.mediaTable || rowIndexes.count != 1) return NO;
    const NSUInteger row = rowIndexes.firstIndex;
    if (row >= self.visibleMediaIds.count) return NO;
    if (self.state->document.FindBin(self.visibleMediaIds[row].UTF8String
                                         ?: ""))
        return NO;
    NSString* identifier = self.visibleMediaIds[row];
    const NSPasteboardType type =
        self.state->project.FindTimeline(identifier.UTF8String ?: "")
            ? kCutmachineTimelinePasteboardType
            : kCutmachineMediaPasteboardType;
    [pasteboard declareTypes:@[ type ] owner:nil];
    return [pasteboard setString:identifier forType:type];
}

- (void)openIconBin:(NSClickGestureRecognizer*)recognizer {
    if (recognizer.state != NSGestureRecognizerStateEnded) return;
    const NSPoint point = [recognizer locationInView:self.mediaCollection];
    NSIndexPath* path = [self.mediaCollection indexPathForItemAtPoint:point];
    if (!path || path.item >= self.visibleMediaIds.count) return;
    NSString* identifier = self.visibleMediaIds[path.item];
    if (!self.state->document.FindBin(identifier.UTF8String ?: "")) return;
    self.mediaCollection.selectionIndexPaths = [NSSet setWithObject:path];
    self.selectedBinId = identifier;
    const NSInteger row = [self.binOutline rowForItem:identifier];
    if (row >= 0) {
        self.updatingBinControls = YES;
        [self.binOutline selectRowIndexes:[NSIndexSet indexSetWithIndex:row]
                     byExtendingSelection:NO];
        self.updatingBinControls = NO;
    }
    [self rebuildMediaList];
}

- (void)openIconItem:(id)sender {
    NSIndexPath* path = nil;
    if ([sender isKindOfClass:NSGestureRecognizer.class]) {
        NSGestureRecognizer* recognizer = sender;
        if (recognizer.state != NSGestureRecognizerStateEnded) return;
        const NSPoint point = [recognizer locationInView:self.mediaCollection];
        path = [self.mediaCollection indexPathForItemAtPoint:point];
    } else {
        path = self.mediaCollection.selectionIndexPaths.anyObject;
    }
    if (!path || path.item >= self.visibleMediaIds.count) return;
    NSString* identifier = self.visibleMediaIds[path.item];
    if (self.state->document.FindBin(identifier.UTF8String ?: "")) return;
    self.mediaCollection.selectionIndexPaths = [NSSet setWithObject:path];
    [self openSelectedMediaInSourceMonitor:sender];
}

- (void)openSelectedMediaInSourceMonitor:(id)sender {
    (void)sender;
    NSString* browserObject = [self selectedBrowserObjectId];
    if (browserObject &&
        self.state->document.FindBin(browserObject.UTF8String ?: "")) {
        [self refreshBinControlsSelecting:browserObject];
        return;
    }
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

- (void)createTimelineFromSelectedMedia:(id)sender {
    (void)sender;
    std::vector<Ulid> mediaIds;
    for (NSString* identifier in [self selectedMediaIds]) {
        const Ulid mediaId(identifier.UTF8String ?: "");
        if (self.state->document.FindLibraryMedia(mediaId))
            mediaIds.push_back(mediaId);
    }
    if (mediaIds.empty()) {
        self.binSummaryLabel.stringValue =
            @"Sélectionnez au moins un rush pour créer une timeline.";
        NSBeep();
        return;
    }

    NSAlert* alert = [NSAlert new];
    alert.messageText = @"Nouvelle timeline avec la sélection";
    alert.informativeText =
        [NSString stringWithFormat:@"Les %lu rushes seront placés bout à bout.",
                                   (unsigned long)mediaIds.size()];
    [alert addButtonWithTitle:@"Créer"];
    [alert addButtonWithTitle:@"Annuler"];
    NSTextField* nameField =
        [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 320, 24)];
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

    const DocumentSequence& current = self.state->document.sequence;
    Project finalProject = self.state->project;
    AddProjectTimelineOperation enriched{trimmed.UTF8String ?: "New Timeline",
                                         current.width, current.height,
                                         current.frame_rate};
    enriched.timeline_id = GenerateUlid();
    enriched.video_track_id = GenerateUlid();
    enriched.audio_track_id = GenerateUlid();
    DocumentSequence createdTimeline;
    createdTimeline.id = enriched.timeline_id;
    createdTimeline.name = enriched.name;
    createdTimeline.width = enriched.width;
    createdTimeline.height = enriched.height;
    createdTimeline.frame_rate = enriched.frame_rate;
    createdTimeline.tracks = {{enriched.video_track_id, "video", 0, {}},
                              {enriched.audio_track_id, "audio", 1, {}}};
    finalProject.timelines.push_back(std::move(createdTimeline));
    EditError editError = EditError::None;
    std::string message;
    DocumentSequence* timeline =
        finalProject.FindTimeline(enriched.timeline_id);
    if (!timeline) return;
    DocumentTrack* videoTrack = nullptr;
    DocumentTrack* audioTrack = nullptr;
    for (DocumentTrack& track : timeline->tracks) {
        if (track.id == enriched.video_track_id) videoTrack = &track;
        if (track.id == enriched.audio_track_id) audioTrack = &track;
    }
    if (!videoTrack || !audioTrack) return;

    RationalTime position{0, 1};
    for (const Ulid& mediaId : mediaIds) {
        const auto foundMedia = std::find_if(
            finalProject.rushes.begin(), finalProject.rushes.end(),
            [&](const LibraryMedia& value) { return value.id == mediaId; });
        const LibraryMedia* media =
            foundMedia == finalProject.rushes.end() ? nullptr : &*foundMedia;
        const DocumentSource* source = self.state->document.FindSource(mediaId);
        if (!media || !source) continue;
        const Ulid linkGroup = media->has_audio ? GenerateUlid() : Ulid{};
        DocumentClip video;
        video.source_id = mediaId;
        video.source_in = {0, source->duration.rate};
        video.duration = source->duration;
        video.timeline_in = position;
        video.include_audio = false;
        video.link_group_id = linkGroup;
        video.sync_anchor_clip_id = video.id;
        videoTrack->clips.push_back(video);
        if (media->has_audio) {
            DocumentClip audio;
            audio.source_id = mediaId;
            audio.source_in = video.source_in;
            audio.duration = video.duration;
            audio.timeline_in = video.timeline_in;
            audio.include_audio = true;
            audio.link_group_id = linkGroup;
            audio.sync_anchor_clip_id = video.id;
            audioTrack->clips.push_back(std::move(audio));
        }
        position = position.add(source->duration);
    }
    if (!finalProject.Validate(message)) {
        self.binSummaryLabel.stringValue = [NSString
            stringWithFormat:@"Timeline invalide : %s", message.c_str()];
        return;
    }

    enriched.exact_project_result =
        ExactProjectState{finalProject.SaveToString()};
    Project candidate = self.state->project;
    ProjectEditLog projectLog = self.state->projectEditLog;
    if (!projectLog.Apply(candidate, ProjectOperation{std::move(enriched)},
                          editError, message) ||
        ![self commitProjectCandidate:candidate
                              editLog:self.state->editLog
                           projectLog:projectLog
                              message:message]) {
        self.binSummaryLabel.stringValue = [NSString
            stringWithFormat:@"Création impossible : %s", message.c_str()];
        return;
    }
    self.state->project = std::move(candidate);
    self.state->projectEditLog = std::move(projectLog);
    self.state->lastHistoryDomain = HistoryDomain::Project;
    [self rebuildMediaList];
    [self activateTimelineIdentifier:[NSString
                                         stringWithUTF8String:timeline->id
                                                                  .c_str()]];
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
    if (action == @selector(menuToggleAutomaticProxies:)) {
        if (!self.state) return NO;
        item.state = self.state->automaticProxiesEnabled
                         ? NSControlStateValueOn
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

- (void)programVideoScopeChanged:(NSPopUpButton*)sender {
    self.state->programVideoScope = VideoScopeModeFromPreference(
        static_cast<int32_t>(sender.selectedItem.tag));
    [NSUserDefaults.standardUserDefaults
        setInteger:static_cast<NSInteger>(self.state->programVideoScope)
            forKey:kProgramVideoScopeDefaultsKey];
    self.state->overlayDirty = true;
}

- (void)toggleSourceMonitor:(NSButton*)sender {
    // Removing the pane from the split is deterministic. Merely hiding an
    // arranged subview leaves NSSplitView free to restore its autosaved
    // divider on the next layout pass, which made SOURCE OFF appear inert.
    const bool makeVisible = !self.state->sourceMonitorVisible;
    if (!makeVisible) {
        self.state->sourceMonitorPanelWidth =
            self.sourceMonitorPanel.frame.size.width;
        self.state->sourceMonitorVisible = false;
        self.state->sourceMonitorActive = false;
        [self.monitorSplitView removeArrangedSubview:self.sourceMonitorPanel];
        [self.sourceMonitorPanel removeFromSuperview];
        sender.state = NSControlStateValueOff;
        sender.title = @"SOURCE OFF";
        sender.image =
            SystemSymbol(@"rectangle", @"Moniteur Record seul", 11.0);
    } else {
        self.state->sourceMonitorVisible = true;
        [self.monitorSplitView insertArrangedSubview:self.sourceMonitorPanel
                                             atIndex:0];
        [self.monitorSplitView adjustSubviews];
        const double available = self.monitorSplitView.bounds.size.width -
                                 self.monitorSplitView.dividerThickness;
        [self.monitorSplitView setPosition:available * 0.5 ofDividerAtIndex:0];
        self.state->sourceMonitorActive = self.state->sourceMonitor;
        sender.state = NSControlStateValueOn;
        sender.title = @"SOURCE ON";
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
    if (tableView == self.mediaTable) return self.visibleMediaIds.count;
    if (tableView == self.audioTable) return self.visibleAudioIds.count;
    if (tableView == self.captionStyleTable)
        return self.visibleCaptionStyleIds.count;
    return 0;
}

- (NSTableRowView*)tableView:(NSTableView*)tableView
               rowViewForRow:(NSInteger)row {
    (void)tableView;
    (void)row;
    return [[CMIndustrialTableRowView alloc] initWithFrame:NSZeroRect];
}

- (NSView*)tableView:(NSTableView*)tableView
    viewForTableColumn:(NSTableColumn*)tableColumn
                   row:(NSInteger)row {
    if (tableView == self.audioTable) {
        if (row < 0 || row >= (NSInteger)self.visibleAudioIds.count) return nil;
        const LibraryMedia* media = self.state->document.FindLibraryMedia(
            self.visibleAudioIds[row].UTF8String ?: "");
        NSTextField* label = [NSTextField labelWithString:@""];
        label.lineBreakMode = NSLineBreakByTruncatingTail;
        if (!media) return label;
        if ([tableColumn.identifier isEqualToString:@"name"])
            label.stringValue =
                [NSString stringWithUTF8String:media->filename.c_str()];
        else if ([tableColumn.identifier isEqualToString:@"format"])
            label.stringValue =
                [NSString stringWithFormat:@"%s · %d ch", media->codec.c_str(),
                                           media->audio_channels];
        else
            label.stringValue = TimeString(media->duration);
        return label;
    }
    if (tableView == self.captionStyleTable) {
        if (row < 0 || row >= (NSInteger)self.visibleCaptionStyleIds.count)
            return nil;
        const CaptionStyle* style = self.state->document.FindCaptionStyle(
            self.visibleCaptionStyleIds[row].UTF8String ?: "");
        NSTextField* label = [NSTextField labelWithString:@""];
        label.lineBreakMode = NSLineBreakByTruncatingTail;
        if (!style) return label;
        if ([tableColumn.identifier isEqualToString:@"style"]) {
            label.stringValue = [NSString
                stringWithUTF8String:ui::media_panel::DescribeCaptionStyle(
                                         *style)
                                         .c_str()];
        } else {
            const auto summaries = ui::media_panel::SummarizeCaptionStyles(
                self.state->document.sequence);
            int32_t count = 0;
            for (const auto& summary : summaries)
                if (summary.style_id == style->id) count = summary.clip_count;
            label.stringValue = [NSString stringWithFormat:@"%d", count];
        }
        return label;
    }
    if (tableView != self.mediaTable || row < 0 ||
        row >= (NSInteger)self.visibleMediaIds.count)
        return nil;
    NSString* identifier = self.visibleMediaIds[row];
    const LibraryMedia* media =
        self.state->document.FindLibraryMedia(identifier.UTF8String ?: "");
    const DocumentBin* bin =
        self.state->document.FindBin(identifier.UTF8String ?: "");
    if ([tableColumn.identifier isEqualToString:@"name"]) {
        NSTableCellView* cell = [[NSTableCellView alloc]
            initWithFrame:NSMakeRect(0, 0, tableColumn.width, 44.0)];
        NSImageView* image =
            [[NSImageView alloc] initWithFrame:NSMakeRect(4, 3, 60, 38)];
        image.imageScaling = NSImageScaleProportionallyUpOrDown;
        image.wantsLayer = YES;
        image.layer.cornerRadius = 0.0;
        image.layer.masksToBounds = YES;
        NSTextField* title = [[BrowserRenameTextField alloc]
            initWithFrame:NSMakeRect(72, 12,
                                     std::max(40.0, tableColumn.width - 76),
                                     20)];
        title.bezeled = NO;
        title.drawsBackground = NO;
        title.autoresizingMask = NSViewWidthSizable;
        title.lineBreakMode = NSLineBreakByTruncatingTail;
        title.editable = YES;
        title.selectable = YES;
        title.delegate = self;
        title.identifier = [@"browser:" stringByAppendingString:identifier];
        cell.imageView = image;
        cell.textField = title;
        [cell addSubview:image];
        [cell addSubview:title];
        if (bin) {
            title.stringValue =
                [NSString stringWithUTF8String:bin->name.c_str()];
            image.image = SystemSymbol(@"folder.fill", @"Chutier", 24.0);
            image.contentTintColor = CMThemeColor(ui::theme::kAccent);
        } else if (media) {
            title.stringValue =
                [NSString stringWithUTF8String:RushDisplayName(
                                                   self.state->project, *media)
                                                   .c_str()];
            const bool offline =
                self.state->offlineSourceIds.count(media->id) != 0;
            image.image = offline ? nil : self.mediaThumbnails[identifier];
            if (image.image) {
                image.contentTintColor = nil;
            } else {
                image.image = SystemSymbol(
                    offline ? @"exclamationmark.triangle" : @"film",
                    offline ? @"Média offline" : @"Média vidéo");
                image.contentTintColor = offline
                                             ? CMThemeColor(ui::theme::kError)
                                             : CMTextSecondaryColor();
            }
        } else if (const DocumentSequence* sequence =
                       self.state->project.FindTimeline(identifier.UTF8String
                                                            ?: "")) {
            title.stringValue =
                [NSString stringWithUTF8String:sequence->name.c_str()];
            image.image =
                SystemSymbol(@"rectangle.stack.fill", @"Séquence", 22.0);
            image.contentTintColor = CMAccentColor();
        }
        return cell;
    }
    NSTextField* label = [NSTextField labelWithString:@""];
    label.lineBreakMode = NSLineBreakByTruncatingTail;
    if (bin) {
        if ([tableColumn.identifier isEqualToString:@"format"])
            label.stringValue = @"Chutier";
        else
            label.stringValue = @"—";
        return label;
    }
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
    NSIndexPath* path = indexPaths.anyObject;
    if (path && path.item < self.visibleMediaIds.count) {
        NSString* identifier = self.visibleMediaIds[path.item];
        if (self.state->document.FindBin(identifier.UTF8String ?: "")) {
            self.selectedBinId = identifier;
            const NSInteger row = [self.binOutline rowForItem:identifier];
            if (row >= 0) {
                self.updatingBinControls = YES;
                [self.binOutline
                        selectRowIndexes:[NSIndexSet indexSetWithIndex:row]
                    byExtendingSelection:NO];
                self.updatingBinControls = NO;
            }
            [self rebuildMediaList];
            return;
        }
    }
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

- (void)menuToggleAutomaticProxies:(id)sender {
    (void)sender;
    self.state->automaticProxiesEnabled = !self.state->automaticProxiesEnabled;
    [NSUserDefaults.standardUserDefaults
        setBool:self.state->automaticProxiesEnabled
         forKey:kAutomaticProxyGenerationDefaultsKey];
    self.infoLabel.stringValue =
        self.state->automaticProxiesEnabled
            ? @"Génération automatique des proxies activée"
            : @"Génération automatique des proxies désactivée";
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
    if (![self applyAndPersistTimelineOperation:Operation {
            AddBinOperation { binId, name, parent }
        }
                                          error:error
                                        message:message]) {
        self.binSummaryLabel.stringValue = [NSString
            stringWithFormat:@"Création refusée : %s", message.c_str()];
        return;
    }
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
    if (![self applyAndPersistTimelineOperation:Operation {
            RemoveBinOperation { binId, "", "" }
        }
                                          error:error
                                        message:message]) {
        self.binSummaryLabel.stringValue = [NSString
            stringWithFormat:@"Suppression refusée : %s", message.c_str()];
        return;
    }
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
    if (![field isKindOfClass:NSTextField.class]) return;
    const BOOL sidebarRename = [field.identifier hasPrefix:@"bin:"];
    const BOOL browserRename = [field.identifier hasPrefix:@"browser:"];
    if (!sidebarRename && !browserRename) return;
    const NSUInteger prefixLength = sidebarRename ? 4 : 8;
    NSString* identifier = [field.identifier substringFromIndex:prefixLength];
    const NSInteger movement =
        [notification.userInfo[NSTextMovementUserInfoKey] integerValue];
    if (movement == NSCancelTextMovement) {
        [self refreshBinControlsSelecting:self.selectedBinId ?: @"__all__"];
        return;
    }
    const DocumentBin* bin =
        self.state->document.FindBin(identifier.UTF8String ?: "");
    NSString* name = [field.stringValue
        stringByTrimmingCharactersInSet:NSCharacterSet
                                            .whitespaceAndNewlineCharacterSet];
    const DocumentSequence* timeline =
        self.state->project.FindTimeline(identifier.UTF8String ?: "");
    const LibraryMedia* media =
        self.state->document.FindLibraryMedia(identifier.UTF8String ?: "");
    const std::string currentName =
        bin        ? bin->name
        : timeline ? timeline->name
        : media    ? RushDisplayName(self.state->project, *media)
                   : std::string{};
    if (name.length == 0 || currentName.empty() ||
        currentName == (name.UTF8String ?: "")) {
        [self refreshBinControlsSelecting:self.selectedBinId ?: @"__all__"];
        return;
    }
    EditError error = EditError::None;
    std::string message;
    if (bin) {
        if (![self applyAndPersistTimelineOperation:Operation {
                RenameBinOperation {
                    identifier.UTF8String ?: "", name.UTF8String ?: ""
                }
            }
                                              error:error
                                            message:message]) {
            self.binSummaryLabel.stringValue = [NSString
                stringWithFormat:@"Renommage refusé : %s", message.c_str()];
            return;
        }
    } else {
        Project candidate = self.state->project;
        ProjectEditLog projectLog = self.state->projectEditLog;
        if (!projectLog.Apply(
                candidate,
                ProjectOperation{RenameProjectItemOperation{
                    identifier.UTF8String ?: "", name.UTF8String ?: ""}},
                error, message) ||
            ![self commitProjectCandidate:candidate
                                  editLog:self.state->editLog
                               projectLog:projectLog
                                  message:message]) {
            self.binSummaryLabel.stringValue = [NSString
                stringWithFormat:@"Renommage refusé : %s", message.c_str()];
            return;
        }
        self.state->project = std::move(candidate);
        self.state->projectEditLog = std::move(projectLog);
        self.state->lastHistoryDomain = HistoryDomain::Project;
        if (identifier.UTF8String &&
            self.state->activeTimelineId == identifier.UTF8String) {
            self.state->document =
                self.state->project.MakeDocument(self.state->activeTimelineId);
            self.programMonitorTitleLabel.stringValue = [NSString
                stringWithFormat:@"RECORD — %s",
                                 self.state->document.sequence.name.c_str()];
            self.window.title = [NSString
                stringWithFormat:@"CUTMACHINE — %s — %s",
                                 self.state->project.name.c_str(),
                                 self.state->document.sequence.name.c_str()];
        }
        self.state->overlayDirty = true;
    }
    [self refreshBinControlsSelecting:self.selectedBinId ?: @"__all__"];
}

- (void)assignMediaToBinPressed:(id)sender {
    (void)sender;
    [self moveMediaIdsToBin:[self selectedMediaIds]];
}

// Shared "choose a destination bin, then apply SetMediaBinOperation to every
// id" flow. Factored out of -assignMediaToBinPressed so F2.3's Audio tab
// (ROADMAP.md) -- which lists a different filtered slice of the same
// document.library, see MediaPanelModel.h -- can offer the identical
// "Déplacer vers un chutier" gesture without a second alert/menu
// implementation.
- (void)moveMediaIdsToBin:(NSArray<NSString*>*)mediaIdentifiers {
    NSMutableArray<NSString*>* mediaIds = [NSMutableArray array];
    for (NSString* identifier in mediaIdentifiers)
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
    Document stagedDocument = self.state->document;
    EditLog stagedLog = self.state->editLog;
    EditError error = EditError::None;
    std::string message;
    for (NSString* media in mediaIds) {
        if (!stagedLog.Apply(stagedDocument,
                             Operation{SetMediaBinOperation{
                                 media.UTF8String ?: "", bin.UTF8String ?: ""}},
                             error, message)) {
            self.binSummaryLabel.stringValue = [NSString
                stringWithFormat:@"Classement refusé : %s", message.c_str()];
            return;
        }
    }
    if (![self persistStagedDocument:stagedDocument
                             editLog:stagedLog
                             message:message]) {
        self.binSummaryLabel.stringValue =
            [NSString stringWithFormat:@"Enregistrement impossible : %s",
                                       message.c_str()];
        return;
    }
    self.state->document = std::move(stagedDocument);
    self.state->editLog = std::move(stagedLog);
    self.state->lastHistoryDomain = HistoryDomain::Timeline;
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
            index == static_cast<NSInteger>(tool) ? CMAccentColor()
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
    if (self.state->playbackDirection > 0)
        return [NSString
            stringWithFormat:@"Lecture ▶ ×%d", self.state->playbackDirection];
    if (self.state->playbackDirection < 0)
        return [NSString
            stringWithFormat:@"Lecture ◀ ×%d", -self.state->playbackDirection];
    return @"Pause";
}

- (NSString*)playheadResolutionStatus {
    return self.state->playheadResolution == PlayheadResolution::Frame
               ? @"Image (M)"
               : @"Échantillon 48 kHz (M)";
}

- (void)setPlaybackDirection:(int)direction {
    self.state->playbackDirection = std::clamp(direction, -4, 4);
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
    // F2.2 -- this is the one hook already fired after every selection
    // change and every edit that could touch the selected clip (see this
    // method's call sites), so it doubles as the Inspector's refresh
    // trigger.
    //
    // PrimarySelectedClipId(), not SelectedClipId(): the latter is empty
    // whenever more than one clip is selected, and clicking a clip that is
    // A/V-linked -- which every clip imported from a video with sound is --
    // selects its partner too. Keying the Inspector off it left the panel
    // blank for exactly the footage people actually edit.
    [self.inspectorView
        reloadWithDocument:self.state->document
            selectedClipId:self.state->interaction->PrimarySelectedClipId()];
    // F2.3 -- keep the Captions tab's "current timeline selection" summary
    // live even when that tab is not the one on screen; it is cheap (one
    // label update) and avoids a stale summary the moment the user switches
    // to it. See MediaPanelModel.h/-updateCaptionSelectionLabel.
    if (self.captionSelectionLabel) [self updateCaptionSelectionLabel];
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
    if (![self applyAndPersistTimelineOperation:Operation{std::move(operation)}
                                          error:error
                                        message:message]) {
        std::fprintf(stderr, "Grading edit rejected (%s): %s\n",
                     EditErrorName(error), message.c_str());
        return;
    }
    self.state->overlayDirty = true;
}

- (void)setLinkedSelectionEnabled:(BOOL)enabled {
    self.state->linkedSelection = enabled;
    self.state->interaction->SetLinkedSelectionEnabled(
        self.state->linkedSelection);
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
    self.state->zoomBarDrag.reset();
    const NSPoint point = [self timelinePointForEvent:event];
    const double timelineHeight = [self timelineHeight];
    if (point.y < 0.0 || point.y > timelineHeight) return;
    // The integrated strip shares its pure geometry with Metal rendering, so
    // the visible handles and the draggable hit areas cannot drift apart.
    if (point.y >= timelineHeight - kTimelineZoomBarHeight) {
        const TimelineZoomBarGeometry geometry =
            CalculateTimelineZoomBarGeometry(self.state->viewport,
                                             self.state->duration,
                                             self.metalView.bounds.size.width);
        const TimelineZoomBarPart part = geometry.HitTest(point.x);
        if (part != TimelineZoomBarPart::None)
            self.state->zoomBarDrag = TimelineZoomBarDrag{
                part, point.x, self.state->viewport, geometry};
        return;
    }
    const double addTrackY =
        kTimelineRulerHeight + self.state->document.sequence.tracks.size() *
                                   self.state->viewport.track_height;
    if (point.y >= kTimelineRulerHeight && point.y < addTrackY &&
        point.x >= 40.0 && point.x < 92.0) {
        const auto tracks = TimelineTracksInDisplayOrder(self.state->document);
        const size_t index =
            static_cast<size_t>((point.y - kTimelineRulerHeight) /
                                self.state->viewport.track_height);
        if (index < tracks.size()) {
            const DocumentTrack& track = *tracks[index];
            const int control = point.x < 56.0 ? 0 : point.x < 74.0 ? 1 : 2;
            EditError error = EditError::None;
            std::string message;
            const RationalTime playhead = self.state->requestedPosition;
            bool applies = true;
            Operation operation = SetTrackLockOperation{};
            if (track.kind == "video" && control == 0) {
                operation = SetTrackOutputOperation{track.id, !track.visible,
                                                    track.muted, track.solo};
            } else if (track.kind == "audio" && control == 0) {
                operation = SetTrackOutputOperation{track.id, track.visible,
                                                    track.muted, !track.solo};
            } else if (track.kind == "audio" && control == 1) {
                operation = SetTrackOutputOperation{track.id, track.visible,
                                                    !track.muted, track.solo};
            } else if ((track.kind == "video" && control == 1) ||
                       (track.kind == "audio" && control == 2)) {
                operation = SetTrackLockOperation{track.id, !track.locked};
            } else {
                applies = false;
            }
            if (applies &&
                [self applyAndPersistTimelineOperation:std::move(operation)
                                                 error:error
                                               message:message]) {
                [self refreshTimelineAfterEditFromPosition:playhead];
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
    if (point.y < kTimelineToolbarHeight && point.x >= 0.0) {
        constexpr double toolStart = 4.0;
        constexpr double toolWidth = 24.0;
        if (point.x >= toolStart && point.x < toolStart + toolWidth * 5.0) {
            const int index =
                static_cast<int>((point.x - toolStart) / toolWidth);
            [self setTimelineTool:static_cast<TimelineTool>(index)];
        } else if (point.x >= 128.0 && point.x < 166.0) {
            [self menuToggleSnapping:nil];
        } else if (point.x >= 170.0 && point.x < 216.0) {
            [self menuToggleLinkedSelection:nil];
        } else if (point.x >= 222.0 && point.x < 294.0) {
            const int index = static_cast<int>((point.x - 222.0) / 24.0);
            if (index == 0)
                [self menuPlayReverse:nil];
            else if (index == 1)
                [self menuStop:nil];
            else
                [self menuPlayForward:nil];
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
        if ([self applyAndPersistTimelineOperation:std::move(operation)
                                             error:error
                                           message:message]) {
            if (linkedCut)
                self.state->interaction->SelectClips(ExpandLinkedClipSelection(
                    self.state->document, {hit->clip_id}));
            else
                self.state->interaction->SelectClip(hit->clip_id);
            [self refreshTimelineAfterEditFromPosition:playhead];
            [self updateSelectionInfo];
            self.state->overlayDirty = true;
        } else if (error != EditError::InvalidTimelineIn) {
            std::fprintf(stderr, "Cut rejected (%s): %s\n",
                         EditErrorName(error), message.c_str());
            self.infoLabel.stringValue = [NSString
                stringWithFormat:@"Coupe refusée (%s) : %s",
                                 EditErrorName(error), message.c_str()];
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
    const bool commandGesture =
        (event.modifierFlags & NSEventModifierFlagCommand) != 0;
    const bool duplicateGesture =
        tool == TimelineTool::Select && selectionHit &&
        selectionHit->edge == TimelineHitEdge::None && optionGesture;
    self.state->linkedSelectionGesture =
        self.state->linkedSelection && (!optionGesture || duplicateGesture);
    self.state->interaction->SetLinkedSelectionEnabled(
        self.state->linkedSelectionGesture);
    self.state->interaction->SetPlayheadSnapTarget(
        commandGesture
            ? std::optional<RationalTime>(self.state->requestedPosition)
            : std::nullopt);
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
    // PointerDown owns linked-selection expansion because it also knows which
    // linked members are protected by a locked track. Re-expanding here would
    // undo that safety decision before the first drag event.
    self.state->editDragging = self.state->interaction->HasActiveDrag();
    self.state->duplicateDragging =
        duplicateGesture && self.state->editDragging;
    if (self.state->duplicateDragging)
        self.state->duplicateDragClipboard = CopyTimelineClips(
            self.state->document, self.state->interaction->SelectedClipIds());
    self.state->scrubDragging =
        self.state->interaction->RequestedPlayhead().has_value();
    if (self.state->interaction->RequestedPlayhead()) {
        RationalTime requested = *self.state->interaction->RequestedPlayhead();
        if (commandGesture) {
            const auto snapped = SnapTimelineTimeToEdit(
                self.state->document, self.state->viewport, requested);
            if (snapped) requested = *snapped;
        }
        [self requestTimelinePosition:requested];
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
    if (self.state->zoomBarDrag) {
        UpdateTimelineZoomBarDrag(
            self.state->viewport, *self.state->zoomBarDrag, point.x,
            self.state->duration, self.metalView.bounds.size.width,
            self.state->duration.rate);
        self.state->overlayDirty = true;
        return;
    }
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
        RationalTime requested =
            self.state->viewport.XToTime(point.x, [self playheadInputRate]);
        if ((event.modifierFlags & NSEventModifierFlagCommand) != 0) {
            const auto snapped = SnapTimelineTimeToEdit(
                self.state->document, self.state->viewport, requested);
            if (snapped) requested = *snapped;
        }
        [self requestTimelinePosition:requested];
        return;
    }
    self.state->interaction->SetPlayheadSnapTarget(
        (event.modifierFlags & NSEventModifierFlagCommand) != 0
            ? std::optional<RationalTime>(self.state->requestedPosition)
            : std::nullopt);
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
    const NSPoint point = [self timelinePointForEvent:event];
    const int previousHoveredTool = self.state->hoveredTimelineTool;
    const Ulid previousHoveredTrack = self.state->hoveredTrackId;
    const int previousHoveredControl = self.state->hoveredTrackControl;
    self.state->hoveredTimelineTool = -1;
    self.state->hoveredTrackId.clear();
    self.state->hoveredTrackControl = -1;

    if (point.y >= 0.0 && point.y < kTimelineToolbarHeight && point.x >= 4.0 &&
        point.x < 124.0)
        self.state->hoveredTimelineTool =
            static_cast<int>((point.x - 4.0) / 24.0);
    if (point.y >= kTimelineRulerHeight) {
        const auto tracks = TimelineTracksInDisplayOrder(self.state->document);
        const size_t trackIndex =
            static_cast<size_t>((point.y - kTimelineRulerHeight) /
                                self.state->viewport.track_height);
        if (trackIndex < tracks.size() && point.x >= 40.0 && point.x < 92.0) {
            self.state->hoveredTrackId = tracks[trackIndex]->id;
            self.state->hoveredTrackControl = point.x < 56.0   ? 0
                                              : point.x < 74.0 ? 1
                                                               : 2;
        }
    }
    const bool chromeHoverChanged =
        previousHoveredTool != self.state->hoveredTimelineTool ||
        previousHoveredTrack != self.state->hoveredTrackId ||
        previousHoveredControl != self.state->hoveredTrackControl;
    if ([self effectiveTool] == TimelineTool::Select) {
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
        if (chromeHoverChanged) self.state->overlayDirty = true;
        return;
    }
    if ([self effectiveTool] == TimelineTool::Slip) {
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

- (BOOL)applyAndPersistTimelineOperation:(Operation)operation
                                   error:(EditError&)error
                                 message:(std::string&)message {
    Document stagedDocument = self.state->document;
    EditLog stagedLog = self.state->editLog;
    if (!stagedLog.Apply(stagedDocument, std::move(operation), error, message))
        return NO;
    if (![self persistStagedDocument:stagedDocument
                             editLog:stagedLog
                             message:message])
        return NO;
    self.state->document = std::move(stagedDocument);
    self.state->editLog = std::move(stagedLog);
    self.state->lastHistoryDomain = HistoryDomain::Timeline;
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
    std::map<std::string, EditLog> timelineLogs = self.state->timelineEditLogs;
    timelineLogs[self.state->activeTimelineId] = editLog;
    if (!ProjectRecovery::WriteAutosave(path ? path : "", project, timelineLogs,
                                        projectLog, message))
        return NO;
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
        if (track.kind == "video" && track.visible) tracks.push_back(&track);
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
    if ([self applyAndPersistTimelineOperation:std::move(operation)
                                         error:error
                                       message:message]) {
        self.state->targetedTrackIds.insert(trackId);
        self.state->timelineTargetTracks[self.state->activeTimelineId] =
            self.state->targetedTrackIds;
        [self refreshTimelineAfterEditFromPosition:playhead];
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
    if (self.state->zoomBarDrag) {
        self.state->zoomBarDrag.reset();
        self.state->overlayDirty = true;
        return;
    }
    self.state->interaction->SetPlayheadSnapTarget(std::nullopt);
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
    const Document documentBeforeEdit = self.state->document;
    const EditLog logBeforeEdit = self.state->editLog;
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
        if (![self persistEdits:message]) {
            self.state->document = documentBeforeEdit;
            self.state->editLog = logBeforeEdit;
            std::fprintf(stderr, "Unable to persist edit: %s\n",
                         message.c_str());
            self.infoLabel.stringValue =
                [NSString stringWithFormat:@"Modification non enregistrée : %s",
                                           message.c_str()];
        }
        [self refreshTimelineAfterEditFromPosition:playhead];
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
    const TimelineScrollIntent intent = ResolveTimelineScrollIntent(
        event.scrollingDeltaX, event.scrollingDeltaY,
        event.hasPreciseScrollingDeltas,
        (event.modifierFlags & NSEventModifierFlagShift) != 0);
    if (std::abs(intent.delta) <= 0.0001) return;
    try {
        if (intent.action == TimelineScrollAction::Zoom) {
            const double exponent = std::clamp(-intent.delta * 0.12, -1.2, 1.2);
            self.state->viewport.ZoomAroundX(
                std::max(point.x, self.state->viewport.header_width),
                std::exp(exponent), self.state->duration.rate);
        } else {
            self.state->viewport.ScrollByPixels(intent.delta,
                                                self.state->duration.rate);
        }
        self.state->overlayDirty = true;
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "Viewport update rejected: %s\n",
                     exception.what());
    }
}

- (void)timelineMagnify:(NSEvent*)event {
    if (self.window.firstResponder == self.sourceMonitorView) return;
    const NSPoint point = [self timelinePointForEvent:event];
    if (point.y < 0.0 || point.y > [self timelineHeight]) return;
    try {
        self.state->viewport.ZoomAroundX(
            std::max(point.x, self.state->viewport.header_width),
            std::exp(std::clamp(static_cast<double>(event.magnification), -1.0,
                                1.0)),
            self.state->duration.rate);
        self.state->overlayDirty = true;
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "Viewport magnification rejected: %s\n",
                     exception.what());
    }
}

- (BOOL)timelineKeyDown:(NSEvent*)event {
    const NSEventModifierFlags modifiers =
        event.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask;
    if (self.videoFullscreenActive && event.keyCode == 53) {
        [self toggleFullscreen:nil];
        return YES;
    }
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
        [self setPlaybackDirection:NextShuttleSpeed(
                                       self.state->playbackDirection, -1)];
        return YES;
    }
    if ([self event:event matchesShortcut:@"play.stop"]) {
        [self menuStop:nil];
        return YES;
    }
    if ([self event:event matchesShortcut:@"play.forward"]) {
        [self setPlaybackDirection:NextShuttleSpeed(
                                       self.state->playbackDirection, 1)];
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
    if (event.keyCode == 126 || event.keyCode == 125) {  // Up / down
        [self setPlaybackDirection:0];
        const bool forward = event.keyCode == 125;
        const auto edit = AdjacentTimelineEdit(
            self.state->document, self.state->requestedPosition, forward);
        if (edit) [self requestTimelinePosition:*edit];
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
    Document stagedDocument = self.state->document;
    EditLog stagedLog = self.state->editLog;
    EditError error = EditError::None;
    std::string message;
    const RationalTime playhead = self.state->requestedPosition;
    if (!stagedLog.Apply(stagedDocument, Operation{std::move(operation)}, error,
                         message)) {
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"%@ refusé (%s) : %s", label,
                                       EditErrorName(error), message.c_str()];
        return NO;
    }
    const auto& stored =
        std::get<PasteClipsOperation>(stagedLog.AppliedEntries().back().op);
    std::vector<Ulid> pastedIds;
    pastedIds.reserve(stored.clips.size());
    for (const PastedClip& clip : stored.clips)
        pastedIds.push_back(clip.clip_id);
    if (![self persistStagedDocument:stagedDocument
                             editLog:stagedLog
                             message:message]) {
        std::fprintf(stderr, "Unable to persist paste: %s\n", message.c_str());
        self.infoLabel.stringValue = [NSString
            stringWithFormat:@"%@ impossible : %s", label, message.c_str()];
        return NO;
    }
    self.state->document = std::move(stagedDocument);
    self.state->editLog = std::move(stagedLog);
    self.state->lastHistoryDomain = HistoryDomain::Timeline;
    self.state->interaction->SelectClips(pastedIds);
    [self refreshTimelineAfterEditFromPosition:playhead];
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
        const bool renameOnly =
            std::holds_alternative<RenameProjectItemOperation>(
                self.state->projectEditLog.AppliedEntries().back().op);
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
        if (renameOnly)
            [self refreshAfterProjectRename];
        else
            [self refreshAfterProjectMutation];
        return;
    }
    const RationalTime playhead = self.state->requestedPosition;
    Document stagedDocument = self.state->document;
    EditLog stagedLog = self.state->editLog;
    if (stagedLog.Undo(stagedDocument, error, message)) {
        if (![self persistStagedDocument:stagedDocument
                                 editLog:stagedLog
                                 message:message]) {
            std::fprintf(stderr, "Unable to persist undo: %s\n",
                         message.c_str());
            return;
        }
        self.state->document = std::move(stagedDocument);
        self.state->editLog = std::move(stagedLog);
        self.state->lastHistoryDomain = HistoryDomain::Timeline;
        [self refreshTimelineAfterEditFromPosition:playhead];
        [self refreshBinControlsSelecting:nil];
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
        const bool renameOnly =
            std::holds_alternative<RenameProjectItemOperation>(
                self.state->projectEditLog.UndoneEntries().back().op);
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
        if (renameOnly)
            [self refreshAfterProjectRename];
        else
            [self refreshAfterProjectMutation];
        return;
    }
    const RationalTime playhead = self.state->requestedPosition;
    Document stagedDocument = self.state->document;
    EditLog stagedLog = self.state->editLog;
    if (stagedLog.Redo(stagedDocument, error, message)) {
        if (![self persistStagedDocument:stagedDocument
                                 editLog:stagedLog
                                 message:message]) {
            std::fprintf(stderr, "Unable to persist redo: %s\n",
                         message.c_str());
            return;
        }
        self.state->document = std::move(stagedDocument);
        self.state->editLog = std::move(stagedLog);
        self.state->lastHistoryDomain = HistoryDomain::Timeline;
        [self refreshTimelineAfterEditFromPosition:playhead];
        [self refreshBinControlsSelecting:nil];
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
    if (![self applyAndPersistTimelineOperation:Operation{
                                                    std::move(*rangeOperation)}
                                          error:error
                                        message:message]) {
        std::fprintf(stderr, "Range delete rejected (%s): %s\n",
                     EditErrorName(error), message.c_str());
        return NO;
    }
    self.state->timelineIn.reset();
    self.state->timelineOut.reset();
    self.state->interaction->SelectClip("");
    [self refreshTimelineAfterEditFromPosition:playhead];
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
        gap               ? Operation{GapDeleteOperationForSelection(
                  self.state->document, *gap, self.state->linkedSelection)}
        : ids.size() == 1 ? Operation{ClearClipOperation{ids.front(), {}}}
                          : Operation{ClearClipsOperation{ids, {}}};
    if (!gap && ripple) {
        const auto rippleOperation =
            BuildTimelineRippleDeleteSelection(self.state->document, ids);
        if (!rippleOperation) return NO;
        operation = *rippleOperation;
    }
    EditError error = EditError::None;
    std::string message;
    const RationalTime playhead = self.state->requestedPosition;
    if (![self applyAndPersistTimelineOperation:std::move(operation)
                                          error:error
                                        message:message])
        return NO;
    self.state->interaction->SelectClip("");
    [self refreshTimelineAfterEditFromPosition:playhead];
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
    if (![self applyAndPersistTimelineOperation:Operation {
            AddTransitionOperation { transition }
        }
                                          error:error
                                        message:message]) {
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"Transition refusée (%s) : %s",
                                       EditErrorName(error), message.c_str()];
        return;
    }
    [self refreshTimelineAfterEditFromPosition:self.state->requestedPosition];
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
    if (![self applyAndPersistTimelineOperation:Operation {
            RemoveTransitionOperation { transitionId }
        }
                                          error:error
                                        message:message]) {
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"Suppression refusée (%s) : %s",
                                       EditErrorName(error), message.c_str()];
        return;
    }
    [self refreshTimelineAfterEditFromPosition:self.state->requestedPosition];
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
    [self setLinkedSelectionEnabled:!self.state->linkedSelection];
}
- (void)menuFitTimeline:(id)sender {
    (void)sender;
    [self fitTimelineToViewportWidth];
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
    if (![self applyAndPersistTimelineOperation:Operation {
            RemoveTrackOperation { track->id }
        }
                                          error:error
                                        message:message]) {
        self.infoLabel.stringValue = [NSString
            stringWithFormat:@"Suppression refusée : %s", message.c_str()];
        return;
    }
    self.state->interaction->SelectClip("");
    [self refreshTimelineAfterEditFromPosition:playhead];
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
    const RationalTime playhead = self.state->requestedPosition;
    if (![self persistStagedDocument:stagedDocument
                             editLog:stagedLog
                             message:message]) {
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"Enregistrement impossible : %s",
                                       message.c_str()];
        return;
    }
    self.state->document = std::move(stagedDocument);
    self.state->editLog = std::move(stagedLog);
    self.state->lastHistoryDomain = HistoryDomain::Timeline;
    [self refreshTimelineAfterEditFromPosition:playhead];
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
    if ([self applyAndPersistTimelineOperation:std::move(operation)
                                         error:error
                                       message:message]) {
        [self refreshTimelineAfterEditFromPosition:playhead];
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
    Operation operation = GapDeleteOperationForSelection(
        self.state->document, gap, self.state->linkedSelection);
    EditError error = EditError::None;
    std::string message;
    const RationalTime playhead = self.state->requestedPosition;
    if ([self applyAndPersistTimelineOperation:std::move(operation)
                                         error:error
                                       message:message]) {
        self.state->interaction->SelectClip("");
        [self refreshTimelineAfterEditFromPosition:playhead];
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
            if (self.state->automaticProxiesEnabled &&
                (replacement->width > 1920 || codec == "hevc" ||
                 codec == "h265" || codec == "av1"))
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
            if (self.state->automaticProxiesEnabled &&
                (media->width > 1920 || codec == "hevc" || codec == "h265" ||
                 codec == "av1"))
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

- (void)refreshAfterProjectRename {
    // A rename changes browser/title metadata only. Rehydrate the active
    // document so a later timeline save cannot restore its previous name,
    // while keeping decode workers and playback caches alive.
    self.state->document =
        self.state->project.MakeDocument(self.state->activeTimelineId);
    self.programMonitorTitleLabel.stringValue =
        [NSString stringWithFormat:@"RECORD — %s",
                                   self.state->document.sequence.name.c_str()];
    self.window.title =
        [NSString stringWithFormat:@"CUTMACHINE — %s — %s",
                                   self.state->project.name.c_str(),
                                   self.state->document.sequence.name.c_str()];
    [self refreshBinControlsSelecting:self.selectedBinId ?: @"__all__"];
    [self updateSelectionInfo];
    self.state->overlayDirty = true;
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
#if defined(CUTMACHINE_UI_SMOKE_TEST)
    if (gUiSmokeTesting) ++gUiSmokeDecodeReloads;
#endif
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
        if (self.state->automaticProxiesEnabled &&
            (media->width > 1920 || codec == "hevc" || codec == "h265" ||
             codec == "av1"))
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
    data.display_sdr_preview = true;
    data.sequence_width = self.state->document.sequence.width;
    data.sequence_height = self.state->document.sequence.height;
    const double width = self.metalView.bounds.size.width;
    const double timelineHeight = [self timelineHeight];
    data.video_height = [self videoHeight];
    const double top = data.video_height;
    auto add = [&](double x, double y, double w, double h, float r, float g,
                   float b, float a = 1.0f) {
        if (w > 0.0 && h > 0.0)
            data.overlays.emplace_back(MetalRect{x, y, w, h, r, g, b, a});
    };
    const auto addColor = [&](double x, double y, double w, double h,
                              ui::theme::Color color) {
        add(x, y, w, h, color.r, color.g, color.b, color.a);
    };
    const auto addV = [&](double x, double y, double w, double h,
                          ui::theme::Color topColor,
                          ui::theme::Color bottomColor) {
        if (w > 0.0 && h > 0.0)
            data.overlays.emplace_back(
                MetalRect{x, y, w, h, topColor, bottomColor});
    };
    const auto addText =
        [&](double x, double y, double maxWidth, double pointSize,
            const std::string& text, ui::theme::Color color,
            MetalFontFace face = MetalFontFace::Mono, bool bold = false) {
            if (!text.empty() && maxWidth > 0.0)
                data.overlays.emplace_back(MetalText{x, y, maxWidth, pointSize,
                                                     text, color, face, bold});
        };
    const auto addIcon = [&](double x, double y, double size,
                             const std::string& name, ui::theme::Color color) {
        if (size > 0.0 && !name.empty())
            data.overlays.emplace_back(
                MetalIcon{x, y, size, size, name, color});
    };
    const auto addTinyText = [&](double x, double y, const std::string& text) {
        addText(x, y, text.size() * 7.0, ui::theme::kFontSizeCaption, text,
                ui::theme::kTextClip, MetalFontFace::Mono, true);
    };

    const auto topmost = std::find_if(
        self.state->requested.rbegin(), self.state->requested.rend(),
        [](const ResolvedSlot& slot) { return slot.active; });
    if (topmost != self.state->requested.rend() &&
        self.state->offlineSourceIds.count(topmost->sourceId) != 0)
        add(0.0, 0.0, width, data.video_height, 0.025f, 0.025f, 0.025f);

    addColor(0.0, top, width, timelineHeight, ui::theme::kSurfaceBase);
    addV(0.0, top, width, kTimelineToolbarHeight, ui::theme::kSurfaceControl,
         ui::theme::kSurfaceRaised);
    addColor(0.0, top + kTimelineToolbarHeight, width, kTimelineScaleHeight,
             ui::theme::kSurfaceRaised);
    addColor(0.0, top + kTimelineToolbarHeight + kTimelineScaleHeight, width,
             kTimelineRenderBandHeight, ui::theme::kSurfaceSunken);
    addColor(0.0, top + kTimelineRulerHeight - 1.0, width, 1.0,
             ui::theme::kSeparator);

    constexpr double toolStart = 4.0;
    constexpr double toolWidth = 24.0;
    const std::array<const char*, 5> toolIcons{{
        "mouse-pointer-2",
        "hand",
        "search",
        "scissors",
        "unfold-horizontal",
    }};
    for (int index = 0; index < 5; ++index) {
        const bool active = static_cast<int>([self effectiveTool]) == index;
        const bool hovered = self.state->hoveredTimelineTool == index;
        const double x = toolStart + index * toolWidth;
        addV(x, top + 2.0, toolWidth - 2.0, 22.0,
             active || hovered ? ui::theme::kSurfaceControlHi
                               : ui::theme::kSurfaceControl,
             active ? ui::theme::kSurfaceControl : ui::theme::kSurfaceRaised);
        addColor(x, top + 2.0, toolWidth - 2.0, 1.0, ui::theme::kEdgeLight);
        if (active)
            addColor(x, top + 22.0, toolWidth - 2.0, 2.0, ui::theme::kAccent);
        if (hovered)
            addColor(x, top + 2.0, toolWidth - 2.0, 2.0,
                     ui::theme::kTextPrimary);
        addIcon(x + 5.0, top + 6.0, 12.0, toolIcons[index],
                active ? ui::theme::kTextPrimary : ui::theme::kTextTertiary);
    }
    const bool snapping = self.state->interaction->SnappingEnabled();
    addV(128.0, top + 2.0, 38.0, 22.0,
         snapping ? ui::theme::kSurfaceControlHi : ui::theme::kSurfaceControl,
         ui::theme::kSurfaceRaised);
    addColor(128.0, top + 2.0, 38.0, 1.0, ui::theme::kEdgeLight);
    if (snapping) addColor(128.0, top + 22.0, 38.0, 2.0, ui::theme::kAccent);
    addIcon(134.0, top + 7.0, 11.0, "magnet",
            snapping ? ui::theme::kAccent : ui::theme::kTextTertiary);
    addText(148.0, top + 6.0, 20.0, ui::theme::kFontSizeCaption, "8",
            ui::theme::kTextSecondary, MetalFontFace::Mono, true);
    addV(170.0, top + 2.0, 44.0, 22.0,
         self.state->linkedSelection ? ui::theme::kSurfaceControlHi
                                     : ui::theme::kSurfaceControl,
         ui::theme::kSurfaceRaised);
    addColor(170.0, top + 2.0, 44.0, 1.0, ui::theme::kEdgeLight);
    if (self.state->linkedSelection)
        addColor(170.0, top + 22.0, 44.0, 2.0, ui::theme::kAccent);
    const ui::theme::Color linkColor = self.state->linkedSelection
                                           ? ui::theme::kTextPrimary
                                           : ui::theme::kTextTertiary;
    addIcon(174.0, top + 7.0, 11.0, "link", linkColor);
    addText(188.0, top + 6.0, 28.0, ui::theme::kFontSizeCaption, "A/V",
            linkColor, MetalFontFace::Mono, true);
    const std::array<const char*, 3> transportLabels{{"J", "K", "L"}};
    for (int index = 0; index < 3; ++index) {
        const bool active = index == 0   ? self.state->playbackDirection < 0
                            : index == 1 ? self.state->playbackDirection == 0
                                         : self.state->playbackDirection > 0;
        const double x = 222.0 + index * 24.0;
        addV(x, top + 2.0, 22.0, 22.0,
             active ? ui::theme::kSurfaceControlHi : ui::theme::kSurfaceControl,
             ui::theme::kSurfaceRaised);
        addColor(x, top + 2.0, 22.0, 1.0, ui::theme::kEdgeLight);
        if (active) addColor(x, top + 22.0, 22.0, 2.0, ui::theme::kAccent);
        addText(x + 7.0, top + 6.0, 8.0, ui::theme::kFontSizeCaption,
                transportLabels[index],
                active ? ui::theme::kTextPrimary : ui::theme::kTextSecondary,
                MetalFontFace::Mono, true);
    }
    addText(
        std::max(300.0, width - 102.0), top + 6.0, 96.0,
        ui::theme::kFontSizeCaption,
        FormatTimecode(self.state->requestedPosition, [self playheadFrameRate]),
        ui::theme::kTextSecondary, MetalFontFace::Mono, true);
    if ([self hasValidTimelineRange]) {
        const double rawIn =
            self.state->viewport.TimeToX(*self.state->timelineIn);
        const double rawOut =
            self.state->viewport.TimeToX(*self.state->timelineOut);
        const double left = std::max(self.state->viewport.header_width, rawIn);
        const double right = std::min(width, rawOut);
        if (right > left) {
            add(left, top + kTimelineToolbarHeight, right - left,
                kTimelineScaleHeight + kTimelineRenderBandHeight,
                ui::theme::kAccent.r, ui::theme::kAccent.g,
                ui::theme::kAccent.b, 0.18f);
        }
        if (rawIn >= self.state->viewport.header_width && rawIn <= width)
            addColor(rawIn, top + kTimelineToolbarHeight, 1.0,
                     timelineHeight - kTimelineToolbarHeight,
                     ui::theme::kMarkIn);
        if (rawOut >= self.state->viewport.header_width && rawOut <= width)
            addColor(rawOut - 1.0, top + kTimelineToolbarHeight, 1.0,
                     timelineHeight - kTimelineToolbarHeight,
                     ui::theme::kMarkOut);
    } else {
        if (self.state->timelineIn) {
            const double x =
                self.state->viewport.TimeToX(*self.state->timelineIn);
            if (x >= self.state->viewport.header_width && x <= width)
                addColor(x, top + kTimelineToolbarHeight, 1.0,
                         timelineHeight - kTimelineToolbarHeight,
                         ui::theme::kMarkIn);
        }
        if (self.state->timelineOut) {
            const double x =
                self.state->viewport.TimeToX(*self.state->timelineOut);
            if (x >= self.state->viewport.header_width && x <= width)
                addColor(x - 1.0, top + kTimelineToolbarHeight, 1.0,
                         timelineHeight - kTimelineToolbarHeight,
                         ui::theme::kMarkOut);
        }
    }
    const auto tracks = TimelineTracksInDisplayOrder(self.state->document);
    bool sawVideoTrack = false;
    bool drewAudioDivider = false;
    uint32_t videoNumber = static_cast<uint32_t>(std::count_if(
        tracks.begin(), tracks.end(),
        [](const DocumentTrack* track) { return track->kind == "video"; }));
    uint32_t audioNumber = 0;
    for (size_t index = 0; index < tracks.size(); ++index) {
        const double y = top + kTimelineRulerHeight +
                         index * self.state->viewport.track_height;
        if (y >= top + timelineHeight) break;
        const bool video = tracks[index]->kind == "video";
        const uint32_t trackNumber = video ? videoNumber-- : ++audioNumber;
        if (video) sawVideoTrack = true;
        if (!video && sawVideoTrack && !drewAudioDivider) {
            addColor(0.0, y - 2.0, width, 2.0, ui::theme::kBorderStrong);
            drewAudioDivider = true;
        }
        addColor(0.0, y, width, self.state->viewport.track_height,
                 ui::theme::kSurfaceLane);
        addV(0.0, y, self.state->viewport.header_width,
             self.state->viewport.track_height, ui::theme::kSurfaceRaised,
             ui::theme::kSurfacePanel);
        addColor(0.0, y, 4.0, self.state->viewport.track_height,
                 ui::theme::TrackTint(video));
        addText(8.0, y + 4.0, 28.0, ui::theme::kFontSizeSmall,
                std::string(video ? "V" : "A") + std::to_string(trackNumber),
                ui::theme::kTextPrimary, MetalFontFace::Mono, true);
        addText(8.0, y + 20.0, 30.0, ui::theme::kFontSizeCaption,
                video ? "VID." : "AUD.", ui::theme::kTextTertiary,
                MetalFontFace::Mono, true);

        const auto addPill = [&](double x, const std::string& label,
                                 bool active, int control,
                                 bool disabled = false) {
            const bool hovered =
                self.state->hoveredTrackId == tracks[index]->id &&
                self.state->hoveredTrackControl == control;
            addV(x, y + 15.0, 16.0, 14.0,
                 disabled            ? ui::theme::kSurfacePanel
                 : active || hovered ? ui::theme::kSurfaceControlHi
                                     : ui::theme::kSurfaceControl,
                 ui::theme::kSurfaceRaised);
            if (!disabled)
                addColor(x, y + 15.0, 16.0, 1.0, ui::theme::kEdgeLight);
            if (active) addColor(x, y + 27.0, 16.0, 2.0, ui::theme::kAccent);
            if (hovered && !disabled)
                addColor(x, y + 15.0, 16.0, 2.0, ui::theme::kTextPrimary);
            addText(x + 5.0, y + 16.0, 8.0, ui::theme::kFontSizeCaption, label,
                    disabled ? ui::theme::kTextTertiary
                    : active ? ui::theme::kTextPrimary
                             : ui::theme::kTextSecondary,
                    MetalFontFace::Mono, true);
        };
        const auto addIconPill = [&](double x, const std::string& icon,
                                     bool active, int control) {
            const bool hovered =
                self.state->hoveredTrackId == tracks[index]->id &&
                self.state->hoveredTrackControl == control;
            addV(x, y + 15.0, 16.0, 14.0,
                 active || hovered ? ui::theme::kSurfaceControlHi
                                   : ui::theme::kSurfaceControl,
                 ui::theme::kSurfaceRaised);
            addColor(x, y + 15.0, 16.0, 1.0, ui::theme::kEdgeLight);
            if (active) addColor(x, y + 27.0, 16.0, 2.0, ui::theme::kAccent);
            if (hovered)
                addColor(x, y + 15.0, 16.0, 2.0, ui::theme::kTextPrimary);
            addIcon(
                x + 3.0, y + 17.0, 10.0, icon,
                active ? ui::theme::kTextPrimary : ui::theme::kTextTertiary);
        };
        if (video) {
            addIconPill(40.0, "eye", tracks[index]->visible, 0);
            addIconPill(58.0, "lock", tracks[index]->locked, 1);
        } else {
            addPill(40.0, "S", tracks[index]->solo, 0);
            addPill(58.0, "M", tracks[index]->muted, 1);
            addIconPill(76.0, "lock", tracks[index]->locked, 2);
        }
        addColor(0.0, y + self.state->viewport.track_height - 1.0, width, 1.0,
                 ui::theme::kSeparator);
    }
    const double addTrackY = top + kTimelineRulerHeight +
                             tracks.size() * self.state->viewport.track_height;
    if (addTrackY < top + timelineHeight) {
        addColor(0.0, addTrackY, width, kAddTrackRowHeight,
                 ui::theme::kSurfaceBase);
        addColor(0.0, addTrackY, self.state->viewport.header_width,
                 kAddTrackRowHeight, ui::theme::kSurfacePanel);
        const double split = self.state->viewport.header_width * 0.5;
        addColor(split, addTrackY, 1.0, kAddTrackRowHeight,
                 ui::theme::kBorderSubtle);
        for (int half = 0; half < 2; ++half) {
            const double centerX = split * (half + 0.5);
            addColor(centerX - 6.0, addTrackY + 10.0, 12.0, 2.0,
                     ui::theme::kTextTertiary);
            addColor(centerX - 1.0, addTrackY + 5.0, 2.0, 12.0,
                     ui::theme::kTextTertiary);
        }
    }
    addColor(self.state->viewport.header_width - 1.0,
             top + kTimelineToolbarHeight, 1.0,
             timelineHeight - kTimelineToolbarHeight, ui::theme::kBorderStrong);
    const std::vector<double> tickXs = self.state->viewport.TickXs(width);
    for (size_t tickIndex = 0; tickIndex < tickXs.size(); ++tickIndex) {
        const double tickX = tickXs[tickIndex];
        const double rulerTop = top + kTimelineToolbarHeight;
        addColor(tickX, rulerTop + kTimelineScaleHeight - 7.0, 1.0, 7.0,
                 ui::theme::kTextTertiary);
        add(tickX, top + kTimelineRulerHeight, 1.0,
            std::max(0.0, timelineHeight - kTimelineRulerHeight),
            ui::theme::kLaneGrid.r, ui::theme::kLaneGrid.g,
            ui::theme::kLaneGrid.b, 0.58f);
        const RationalTime tickTime = self.state->viewport.XToTime(
            tickX, self.state->document.sequence.frame_rate.num);
        addText(tickX + 4.0, rulerTop + 4.0, 74.0, ui::theme::kFontSizeCaption,
                FormatTimecode(tickTime, [self playheadFrameRate]),
                ui::theme::kTextTertiary, MetalFontFace::Mono, true);
        if (tickIndex + 1 < tickXs.size()) {
            const double interval = tickXs[tickIndex + 1] - tickX;
            for (int subdivision = 1; subdivision < 4; ++subdivision) {
                addColor(tickX + interval * subdivision / 4.0,
                         rulerTop + kTimelineScaleHeight - 4.0, 1.0, 4.0,
                         ui::theme::kBorderStrong);
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
                addColor(left, y, right - left, height,
                         ui::theme::WithAlpha(ui::theme::kAccent, 0.18f));
                addColor(left, y, right - left, 2.0, ui::theme::kAccent);
                addColor(left, y + height - 2.0, right - left, 2.0,
                         ui::theme::kAccent);
                addColor(left, y, 2.0, height, ui::theme::kAccent);
                addColor(right - 2.0, y, 2.0, height, ui::theme::kAccent);
                for (double stripe = left + 8.0; stripe < right; stripe += 12.0)
                    addColor(stripe, y + 5.0, 1.0, std::max(0.0, height - 10.0),
                             ui::theme::WithAlpha(ui::theme::kAccentHi, 0.35f));
            }
        }
    }

    const auto clips =
        VisibleTimelineClips(self.state->document, self.state->viewport, width,
                             self.state->interaction->SelectedClipIds(),
                             self.state->interaction->TrimPreview(),
                             self.state->interaction->MovePreview());
    for (const TimelineClipRect& clip : clips) {
        double left = std::min(clip.x, clip.x + clip.width);
        double right = std::max(clip.x, clip.x + clip.width);
        const bool headVisible =
            left >= self.state->viewport.header_width && left <= width;
        left = std::max(left, self.state->viewport.header_width);
        right = std::min(right, width);
        if (right <= left) continue;
        const double y = top + clip.y;
        if (y >= top + timelineHeight) continue;
        const auto color = ClipColor(clip.source_id, clip.audio);
        if (!clip.valid)
            addColor(left, y + 2.0, right - left, 40.0,
                     ui::theme::WithAlpha(ui::theme::kError, 0.92f));
        else {
            const float previewLift = clip.preview ? 0.08f : 0.0f;
            // The moving rectangle is the exact overwrite candidate, not a
            // decorative ghost. Keep it opaque so the incoming result remains
            // readable over the clip it is about to replace.
            const float movingAlpha = 1.0f;
            const ui::theme::Color clipTop{
                std::min(1.0f, color[0] + 0.08f + previewLift),
                std::min(1.0f, color[1] + 0.08f + previewLift),
                std::min(1.0f, color[2] + 0.08f + previewLift), movingAlpha};
            const ui::theme::Color clipBottom{
                std::max(0.0f, color[0] - 0.05f + previewLift),
                std::max(0.0f, color[1] - 0.05f + previewLift),
                std::max(0.0f, color[2] - 0.05f + previewLift), movingAlpha};
            addV(left, y + 2.0, right - left, 40.0, clipTop, clipBottom);
        }
        addColor(left, y + 2.0, right - left, 1.0, ui::theme::kEdgeLight);
        if (headVisible)
            addColor(left, y + 3.0, 3.0, 38.0,
                     ui::theme::WithAlpha(ui::theme::kTextClip, 0.32f));
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
        const double clipSpan = right - left;
        add(left, y + 28.0, clipSpan, 14.0, 0.0f, 0.0f, 0.0f, 0.40f);
        if (clipSpan >= ui::theme::kTimelineClipNameMinWidth) {
            addText(left + 5.0, y + 29.0, clipSpan - 10.0,
                    ui::theme::kFontSizeSmall,
                    TimelineClipName(self.state->document, self.state->project,
                                     clip.source_id),
                    ui::theme::kTextClip, MetalFontFace::Mono, true);
        }
        add(left, y + 40.0, clipSpan, 2.0, 0.0f, 0.0f, 0.0f, 0.28f);
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
        // Every edit boundary remains readable when adjacent clips share the
        // same source. Selection remains the only clip-level emphasis.
        const double outlineWidth = std::min(1.0, right - left);
        addColor(left, y + 2.0, outlineWidth, 40.0, ui::theme::kSeparator);
        addColor(right - outlineWidth, y + 2.0, outlineWidth, 40.0,
                 ui::theme::kSeparator);
        if (clip.selected) {
            const ui::theme::Color selection =
                clip.valid ? ui::theme::kTextPrimary : ui::theme::kError;
            addColor(left, y + 2.0, clipSpan, 2.0, selection);
            addColor(left, y + 40.0, clipSpan, 2.0, selection);
            addColor(left, y + 2.0, std::min(2.0, clipSpan), 40.0, selection);
            addColor(std::max(left, right - 2.0), y + 2.0,
                     std::min(2.0, clipSpan), 40.0, selection);
        }
        if (clip.selected && self.window.firstResponder == self.metalView &&
            clipSpan > 8.0) {
            addColor(left + 4.0, y + 6.0, clipSpan - 8.0, 1.0,
                     ui::theme::kAccent);
            addColor(left + 4.0, y + 37.0, clipSpan - 8.0, 1.0,
                     ui::theme::kAccent);
            addColor(left + 4.0, y + 6.0, 1.0, 32.0, ui::theme::kAccent);
            addColor(right - 5.0, y + 6.0, 1.0, 32.0, ui::theme::kAccent);
        }
        if (clip.moving) {
            addColor(left, y + 2.0, clipSpan, 2.0, ui::theme::kAccent);
            addColor(left, y + 40.0, clipSpan, 2.0, ui::theme::kAccent);
            addColor(left, y + 2.0, std::min(2.0, clipSpan), 40.0,
                     ui::theme::kAccent);
            addColor(std::max(left, right - 2.0), y + 2.0,
                     std::min(2.0, clipSpan), 40.0, ui::theme::kAccent);
        }
    }

    // Locked lanes stay visible but receive a sparse diagonal safety hatch
    // above their clips. Axis-aligned stair steps keep this compatible with
    // the timeline's rectangle-only Metal display list.
    for (size_t index = 0; index < tracks.size(); ++index) {
        if (!tracks[index]->locked) continue;
        const double y = top + kTimelineRulerHeight +
                         index * self.state->viewport.track_height;
        const double height = self.state->viewport.track_height;
        if (y >= top + timelineHeight) continue;
        add(self.state->viewport.header_width, y,
            width - self.state->viewport.header_width, height, 0.0f, 0.0f, 0.0f,
            0.26f);
        for (double baseX = self.state->viewport.header_width - height;
             baseX < width; baseX += 16.0) {
            for (double offset = 0.0; offset < height; offset += 3.0) {
                const double x = baseX + offset;
                if (x >= self.state->viewport.header_width && x < width)
                    add(x, y + offset, std::min(4.0, width - x), 1.5,
                        ui::theme::kTextTertiary.r, ui::theme::kTextTertiary.g,
                        ui::theme::kTextTertiary.b, 0.42f);
            }
        }
    }

    // The range belongs to the timeline, not to its empty background. Draw
    // its translucent body after clips so In/Out remains readable across
    // picture, waveform and gaps alike.
    if ([self hasValidTimelineRange]) {
        const double rawIn =
            self.state->viewport.TimeToX(*self.state->timelineIn);
        const double rawOut =
            self.state->viewport.TimeToX(*self.state->timelineOut);
        const double left = std::max(self.state->viewport.header_width, rawIn);
        const double right = std::min(width, rawOut);
        if (right > left)
            add(left, top + kTimelineRulerHeight, right - left,
                std::max(0.0, timelineHeight - kTimelineRulerHeight -
                                  kTimelineZoomBarHeight),
                ui::theme::kAccent.r, ui::theme::kAccent.g,
                ui::theme::kAccent.b, 0.10f);
        if (rawIn >= self.state->viewport.header_width && rawIn <= width)
            addColor(
                rawIn, top + kTimelineRulerHeight, 1.0,
                timelineHeight - kTimelineRulerHeight - kTimelineZoomBarHeight,
                ui::theme::kMarkIn);
        if (rawOut >= self.state->viewport.header_width && rawOut <= width)
            addColor(
                rawOut - 1.0, top + kTimelineRulerHeight, 1.0,
                timelineHeight - kTimelineRulerHeight - kTimelineZoomBarHeight,
                ui::theme::kMarkOut);
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
        addColor(leftX, y, rightX - leftX, height,
                 ui::theme::WithAlpha(ui::theme::kSurfaceControlHi, 0.72f));
        addColor(leftX, y, rightX - leftX, 2.0, ui::theme::kAccent);
        addColor(leftX, y + height - 2.0, rightX - leftX, 2.0,
                 ui::theme::kAccent);
        const double cutX =
            self.state->viewport.TimeToX(rightClip->timeline_in);
        addColor(cutX - 1.0, y, 2.0, height, ui::theme::kTextPrimary);
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
            addColor(left, top + lassoTop, right - left, bottom - lassoTop,
                     ui::theme::WithAlpha(ui::theme::kAccent, 0.12f));
            addColor(left, top + lassoTop, right - left, 1.0,
                     ui::theme::kAccent);
            addColor(left, top + bottom - 1.0, right - left, 1.0,
                     ui::theme::kAccent);
            addColor(left, top + lassoTop, 1.0, bottom - lassoTop,
                     ui::theme::kAccent);
            addColor(right - 1.0, top + lassoTop, 1.0, bottom - lassoTop,
                     ui::theme::kAccent);
        }
    }

    if (self.state->interaction->SnapGuideTime()) {
        const double snapX = self.state->viewport.TimeToX(
            *self.state->interaction->SnapGuideTime());
        if (snapX >= self.state->viewport.header_width && snapX <= width)
            addColor(
                snapX, top + kTimelineRulerHeight, 1.0,
                timelineHeight - kTimelineRulerHeight - kTimelineZoomBarHeight,
                ui::theme::kAccent);
    }
    if (self.state->cutPreviewX && self.state->cutPreviewY) {
        addColor(*self.state->cutPreviewX - 1.0, top + *self.state->cutPreviewY,
                 2.0, self.state->viewport.track_height, ui::theme::kAccent);
    }

    const double zoomY = top + timelineHeight - kTimelineZoomBarHeight;
    addColor(0.0, zoomY, width, kTimelineZoomBarHeight,
             ui::theme::kSurfaceSunken);
    addColor(0.0, zoomY, self.state->viewport.header_width,
             kTimelineZoomBarHeight, ui::theme::kSurfacePanel);
    const TimelineZoomBarGeometry zoomGeometry =
        CalculateTimelineZoomBarGeometry(self.state->viewport,
                                         self.state->duration, width);
    if (zoomGeometry.thumb_width > 0.0) {
        addV(zoomGeometry.thumb_x, zoomY + 2.0, zoomGeometry.thumb_width, 10.0,
             ui::theme::kSurfaceControlHi, ui::theme::kSurfaceControl);
        addColor(zoomGeometry.thumb_x, zoomY + 2.0, zoomGeometry.handle_width,
                 10.0, ui::theme::kTextTertiary);
        addColor(zoomGeometry.thumb_x + zoomGeometry.thumb_width -
                     zoomGeometry.handle_width,
                 zoomY + 2.0, zoomGeometry.handle_width, 10.0,
                 ui::theme::kTextTertiary);
        if (self.state->zoomBarDrag)
            addColor(zoomGeometry.thumb_x, zoomY, zoomGeometry.thumb_width, 2.0,
                     ui::theme::kAccent);
    }

    const double playheadX =
        self.state->viewport.TimeToX(self.state->requestedPosition);
    if (playheadX >= self.state->viewport.header_width && playheadX <= width) {
        addColor(
            playheadX, top + kTimelineToolbarHeight, 1.0,
            timelineHeight - kTimelineToolbarHeight - kTimelineZoomBarHeight,
            ui::theme::kAccent);
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
        programData.display_sdr_preview = true;
        programData.sequence_width = self.state->document.sequence.width;
        programData.sequence_height = self.state->document.sequence.height;
        programData.video_height = self.programMonitorView.bounds.size.height;
        programData.video_zoom = self.state->programMonitorZoom;
        programData.video_scope = self.state->programVideoScope;
        programData.video_rotation_degrees.resize(candidates.size(), 0);
        programData.video_opacities.resize(candidates.size(), 1.0f);
        // Record is the canonical edited output: both monitors share the same
        // SDR presentation transform, while Record additionally displays the
        // clip's creative grade. Source remains the ungraded rush reference.
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
            sourceData.display_sdr_preview = true;
            // Both viewers use the sequence canvas. Portrait media is then
            // letterboxed at the same scale in Source and Record.
            sourceData.sequence_width = self.state->document.sequence.width;
            sourceData.sequence_height = self.state->document.sequence.height;
            sourceData.video_height = self.sourceMonitorView.bounds.size.height;
            sourceData.video_zoom = self.state->sourceMonitorZoom;
            sourceData.video_rotation_degrees = {0};
            const auto metadata =
                self.state->mediaMetadata.find(self.state->sourceMonitorId);
            if (metadata != self.state->mediaMetadata.end()) {
                if (metadata->second.metadata_complete) {
                    sourceData.video_rotation_degrees[0] =
                        metadata->second.rotation_degrees;
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

// FitDuration rejects a width at or below the track-header gutter, which is
// the right contract for it: below that there is no timeline left to scale
// into, so any answer it returned would be a lie. A view that AppKit has not
// laid out yet reports exactly such a width, though, and that is an ordinary
// transient state rather than a caller error -- so ask here instead of
// letting an uncaught C++ exception out of a Cocoa callback and abort the
// process. -timelineMetalViewDidResize: re-fits as soon as there is a real
// width, so nothing is lost by skipping.
- (void)fitTimelineToViewportWidth {
    if (!self.state) return;
    const double width = self.metalView.bounds.size.width;
    if (!std::isfinite(width) || width <= self.state->viewport.header_width)
        return;
    self.state->viewport.FitDuration(self.state->duration, width);
    self.state->viewportFitted = true;
    self.state->overlayDirty = true;
}

- (void)timelineMetalViewDidResize:(TimelineMetalView*)view {
    if (!self.state) return;
    if (view == self.metalView && self.state->renderer) {
        self.state->renderer->Resize(view.bounds);
        if (!self.state->viewportFitted) [self fitTimelineToViewportWidth];
        [self refreshTimelineChrome];
    } else if (view == self.sourceMonitorView && self.state->sourceRenderer) {
        self.state->sourceRenderer->Resize(view.bounds);
    } else if (view == self.programMonitorView && self.state->programRenderer) {
        self.state->programRenderer->Resize(view.bounds);
    }
    self.state->overlayDirty = true;
}

// The workspace is three panes -- media | editor | right dock -- so it has
// two dividers, and they cannot share one constraint. Divider 1 sits roughly
// a dock's width from the right edge, which is far past where divider 0 is
// allowed to go; answering divider 0's range for both is what collapsed the
// editor and the dock to zero width on launch, and a zero-width timeline is
// what made FitDuration throw. Every bound below is an absolute x in the
// split view, which is the coordinate space AppKit asks about.
static constexpr CGFloat kMediaPaneMinWidth = 220.0;
static constexpr CGFloat kEditorPaneMinWidth = 360.0;
static constexpr CGFloat kRightDockMinWidth = 260.0;

// AppKit exposes setPosition:ofDividerAtIndex: but no matching getter, so
// read divider 0 off the pane in front of it: in a vertical split view that
// pane's right edge is the divider.
static CGFloat FirstDividerPosition(NSSplitView* splitView) {
    NSArray<NSView*>* panes = splitView.arrangedSubviews;
    return panes.count > 0 ? NSMaxX(panes.firstObject.frame) : 0.0;
}

- (CGFloat)splitView:(NSSplitView*)splitView
    constrainSplitPosition:(CGFloat)proposedPosition
               ofSubviewAt:(NSInteger)dividerIndex {
    if (splitView == self.monitorSplitView && self.state &&
        self.state->sourceMonitorVisible && dividerIndex == 0) {
        return (splitView.bounds.size.width - splitView.dividerThickness) * 0.5;
    }
    return proposedPosition;
}

- (CGFloat)splitView:(NSSplitView*)splitView
    constrainMinCoordinate:(CGFloat)proposedMinimumPosition
               ofSubviewAt:(NSInteger)dividerIndex {
    const CGFloat divider = splitView.dividerThickness;
    if (splitView == self.workspaceSplitView) {
        if (dividerIndex == 0) return kMediaPaneMinWidth;
        return FirstDividerPosition(splitView) + divider + kEditorPaneMinWidth;
    }
    if (splitView == self.editorSplitView) return 180.0;
    if (splitView == self.monitorSplitView)
        return self.state && !self.state->sourceMonitorVisible ? 0.0 : 320.0;
    return proposedMinimumPosition;
}

- (CGFloat)splitView:(NSSplitView*)splitView
    constrainMaxCoordinate:(CGFloat)proposedMaximumPosition
               ofSubviewAt:(NSInteger)dividerIndex {
    const CGFloat divider = splitView.dividerThickness;
    if (splitView == self.workspaceSplitView) {
        const CGFloat width = splitView.bounds.size.width;
        if (dividerIndex == 0)
            return std::max<CGFloat>(kMediaPaneMinWidth,
                                     width - kRightDockMinWidth -
                                         kEditorPaneMinWidth - divider * 2.0);
        return std::max<CGFloat>(
            FirstDividerPosition(splitView) + divider + kEditorPaneMinWidth,
            width - kRightDockMinWidth - divider);
    }
    if (splitView == self.editorSplitView)
        return std::max<CGFloat>(
            180.0, splitView.bounds.size.height - 200.0 - divider);
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
    (void)notification;
    if (!self.state) return;
    [self refreshTimelineChrome];
    self.state->overlayDirty = true;
}

#if defined(CUTMACHINE_UI_SMOKE_TEST)
- (void)runUiSmokeTests {
    // This is an in-process end-to-end smoke test: the real AppDelegate has
    // loaded and locked a disposable project, built the actual AppKit tree,
    // initialized Metal, and wired every target/action. It requires a macOS
    // WindowServer, but no Accessibility permission or synthetic global input.
    UiSmokeCheck(self.window != nil && self.state != nullptr,
                 "application launches a real editor window");

    NSButton* toggle = self.sourceMonitorToggleButton;
    const NSPoint togglePoint =
        NSMakePoint(NSMidX(toggle.frame), NSMidY(toggle.frame));
    UiSmokeCheck([self.programMonitorView hitTest:togglePoint] == toggle,
                 "Source toggle is hit-testable above the Metal view");
    ClickControlThroughWindow(toggle);
    UiSmokeCheck(!self.state->sourceMonitorVisible &&
                     self.sourceMonitorPanel.superview == nil &&
                     self.programMonitorPanel.frame.size.width ==
                         self.monitorSplitView.bounds.size.width &&
                     [toggle.title isEqualToString:@"SOURCE OFF"],
                 "Source toggle collapses the Source monitor");
    ClickControlThroughWindow(toggle);
    const double viewerWidthDelta =
        std::abs(self.sourceMonitorPanel.frame.size.width -
                 self.programMonitorPanel.frame.size.width);
    const double viewerHeightDelta =
        std::abs(self.sourceMonitorPanel.frame.size.height -
                 self.programMonitorPanel.frame.size.height);
    UiSmokeCheck(
        self.state->sourceMonitorVisible &&
            self.sourceMonitorPanel.superview == self.monitorSplitView &&
            viewerWidthDelta < 1.0 && viewerHeightDelta < 1.0 &&
            [toggle.title isEqualToString:@"SOURCE ON"],
        "Source toggle restores two equally sized monitors");

    NSPopUpButton* scopePopup = self.programVideoScopePopup;
    const NSPoint scopePoint =
        NSMakePoint(NSMidX(scopePopup.frame), NSMidY(scopePopup.frame));
    UiSmokeCheck([self.programMonitorView hitTest:scopePoint] == scopePopup,
                 "Record scope selector is hit-testable above Metal");
    const VideoScopeMode initialScope = self.state->programVideoScope;
    [scopePopup
        selectItemWithTag:static_cast<NSInteger>(VideoScopeMode::Waveform)];
    [scopePopup sendAction:scopePopup.action to:scopePopup.target];
    UiSmokeCheck(
        self.state->programVideoScope == VideoScopeMode::Waveform &&
            [NSUserDefaults.standardUserDefaults
                integerForKey:kProgramVideoScopeDefaultsKey] ==
                static_cast<NSInteger>(VideoScopeMode::Waveform),
        "Record waveform toggle updates and persists local monitor state");
    [self presentNearestFrameAtDeadline:NO];
    UiSmokeCheck(!self.state->overlayDirty,
                 "Record waveform executes the Metal scope render pass");
    [scopePopup selectItemWithTag:static_cast<NSInteger>(initialScope)];
    [scopePopup sendAction:scopePopup.action to:scopePopup.target];

    std::optional<Ulid> initialTrackId;
    std::optional<Ulid> initialClipId;
    std::optional<Ulid> initialAudioTrackId;
    for (const DocumentTrack& track : self.state->document.sequence.tracks) {
        if (track.kind == "video" && !track.clips.empty()) {
            initialTrackId = track.id;
            initialClipId = track.clips.front().id;
        }
        if (track.kind == "audio") initialAudioTrackId = track.id;
    }
    UiSmokeCheck(initialTrackId.has_value() && initialClipId.has_value(),
                 "UI fixture exposes a selectable video clip");
    if (initialClipId) {
        const double rulerTimelineY = kTimelineToolbarHeight + 8.0;
        const NSPoint playheadPoint =
            NSMakePoint(self.state->viewport.TimeToX({37, 25}),
                        self.metalView.bounds.size.height - rulerTimelineY);
        const NSPoint contentPoint =
            [self.metalView convertPoint:playheadPoint
                                  toView:self.window.contentView];
        UiSmokeCheck(
            [self.window.contentView hitTest:contentPoint] == self.metalView,
            "Window hit-test routes timeline coordinates to Metal");
        SendTimelineMouseGesture(self.metalView, playheadPoint, playheadPoint);
        UiSmokeCheck(self.state->requestedPosition == RationalTime(37, 25),
                     "Timeline click moves the playhead through AppKit");

        const TimelineViewport savedViewport = self.state->viewport;
        self.state->viewport.view_start = {0, 25};
        self.state->viewport.pixels_per_second = 2000.0;
        self.state->viewport.ScrollByPixels(-10000.0, 25);
        UiSmokeCheck(self.state->viewport.view_start == RationalTime{0, 25},
                     "Timeline navigation cannot expose time before zero");
        TimelineZoomBarGeometry zoomGeometry = CalculateTimelineZoomBarGeometry(
            self.state->viewport, self.state->duration,
            self.metalView.bounds.size.width);
        const double zoomBarViewY = kTimelineZoomBarHeight * 0.5;
        const NSPoint thumbStart =
            NSMakePoint(zoomGeometry.thumb_x + zoomGeometry.thumb_width * 0.5,
                        zoomBarViewY);
        const double beforeBarZoom = self.state->viewport.pixels_per_second;
        SendTimelineMouseGesture(
            self.metalView, thumbStart,
            NSMakePoint(thumbStart.x + 40.0, thumbStart.y));
        UiSmokeCheck(self.state->viewport.view_start > RationalTime{0, 25} &&
                         std::abs(self.state->viewport.pixels_per_second -
                                  beforeBarZoom) < 0.001 &&
                         !self.state->zoomBarDrag,
                     "Dragging the bottom thumb pans the visible timeline");
        zoomGeometry = CalculateTimelineZoomBarGeometry(
            self.state->viewport, self.state->duration,
            self.metalView.bounds.size.width);
        const NSPoint rightHandle =
            NSMakePoint(zoomGeometry.thumb_x + zoomGeometry.thumb_width - 2.0,
                        zoomBarViewY);
        const double beforeHandleZoom = self.state->viewport.pixels_per_second;
        SendTimelineMouseGesture(
            self.metalView, rightHandle,
            NSMakePoint(rightHandle.x - 40.0, rightHandle.y));
        UiSmokeCheck(self.state->viewport.pixels_per_second > beforeHandleZoom,
                     "Dragging a bottom-bar handle changes timeline zoom");
        self.state->viewport = savedViewport;

        const DocumentClip* beforeMove =
            self.state->document.FindClip(*initialClipId);
        const RationalTime originalTimelineIn =
            beforeMove ? beforeMove->timeline_in : RationalTime{0, 25};
        const size_t beforeMoveEdits = self.state->editLog.AppliedCount();
        const double trackTimelineY =
            kTimelineRulerHeight + self.state->viewport.track_height * 0.5;
        const NSPoint dragStart =
            NSMakePoint(self.state->viewport.TimeToX({25, 25}),
                        self.metalView.bounds.size.height - trackTimelineY);
        const NSPoint dragEnd = NSMakePoint(dragStart.x + 60.0, dragStart.y);
        self.state->interaction->PointerDown(
            dragStart.x, trackTimelineY, self.metalView.bounds.size.width, 25);
        self.state->interaction->PointerDrag(dragEnd.x, trackTimelineY,
                                             self.metalView.bounds.size.width);
        const auto movingPreview = self.state->interaction->MovePreview();
        const TimelineRenderData movingData = [self timelineRenderData];
        const double movingX =
            movingPreview
                ? self.state->viewport.TimeToX(movingPreview->timeline_in)
                : -1.0;
        const double movingY = [self videoHeight] + kTimelineRulerHeight + 3.5;
        const bool hasOpaqueMovingBody = std::any_of(
            movingData.overlays.begin(), movingData.overlays.end(),
            [&](const MetalDrawCommand& overlay) {
                const MetalRect* rect = std::get_if<MetalRect>(&overlay);
                return rect && std::abs(rect->x - movingX) < 0.01 &&
                       std::abs(rect->y - movingY) < 0.01 &&
                       std::abs(rect->height - 40.0) < 0.01 &&
                       std::abs(rect->alpha - 1.0f) < 0.001f &&
                       std::abs(rect->bottom_alpha - 1.0f) < 0.001f;
            });
        UiSmokeCheck(
            movingPreview && movingPreview->valid && hasOpaqueMovingBody,
            "Timeline overwrite preview remains fully opaque");
        self.state->interaction->CancelDrag();
        SendTimelineMouseGesture(self.metalView, dragStart, dragEnd);
        const DocumentClip* afterMove =
            self.state->document.FindClip(*initialClipId);
        UiSmokeCheck(afterMove &&
                         afterMove->timeline_in != originalTimelineIn &&
                         self.state->editLog.AppliedCount() > beforeMoveEdits,
                     "Timeline drag moves a clip through EditLog");

        self.shortcutBindings[@"play.forward"] = @"L";
        self.shortcutBindings[@"play.reverse"] = @"J";
        self.shortcutBindings[@"play.stop"] = @"K";
        SendKeyThroughWindow(self.metalView, @"l", 37);
        SendKeyThroughWindow(self.metalView, @"l", 37);
        UiSmokeCheck(self.state->playbackDirection == 2,
                     "Second L press enables 2x forward shuttle");
        SendKeyThroughWindow(self.metalView, @"k", 40);
        SendKeyThroughWindow(self.metalView, @"j", 38);
        SendKeyThroughWindow(self.metalView, @"j", 38);
        UiSmokeCheck(self.state->playbackDirection == -2,
                     "Second J press enables 2x reverse shuttle");
        SendKeyThroughWindow(self.metalView, @"k", 40);
        UiSmokeCheck(self.state->playbackDirection == 0,
                     "K stops shuttle playback");
        [self requestTimelinePosition:{0, 25}];
        SendKeyThroughWindow(self.metalView, @"\uf701", 125);
        const RationalTime nextCut = self.state->requestedPosition;
        SendKeyThroughWindow(self.metalView, @"\uf700", 126);
        UiSmokeCheck(nextCut > RationalTime{0, 25} &&
                         self.state->requestedPosition == RationalTime{0, 25},
                     "Down/Up navigate to the next/previous exact cut");

        const auto clickTrackControl = [&](size_t trackIndex, double x) {
            const double timelineY =
                kTimelineRulerHeight +
                trackIndex * self.state->viewport.track_height +
                self.state->viewport.track_height * 0.5;
            const NSPoint point =
                NSMakePoint(x, self.metalView.bounds.size.height - timelineY);
            SendTimelineMouseGesture(self.metalView, point, point);
        };
        const DocumentTrack* videoTrack =
            self.state->document.FindTrack(*initialTrackId);
        UiSmokeCheck(videoTrack && videoTrack->visible,
                     "Video tracks start visible");
        clickTrackControl(0, 48.0);
        videoTrack = self.state->document.FindTrack(*initialTrackId);
        UiSmokeCheck(videoTrack && !videoTrack->visible,
                     "Video eye hides the track output");
        clickTrackControl(0, 48.0);
        clickTrackControl(0, 66.0);
        videoTrack = self.state->document.FindTrack(*initialTrackId);
        UiSmokeCheck(videoTrack && videoTrack->visible && videoTrack->locked,
                     "Video lock rejects edits independently of visibility");
        const TimelineRenderData lockedData = [self timelineRenderData];
        const bool hasLockHatch = std::any_of(
            lockedData.overlays.begin(), lockedData.overlays.end(),
            [&](const MetalDrawCommand& overlay) {
                const MetalRect* rect = std::get_if<MetalRect>(&overlay);
                return rect && std::abs(rect->height - 1.5) < 0.001 &&
                       std::abs(rect->alpha - 0.42f) < 0.001f &&
                       rect->x >= self.state->viewport.header_width;
            });
        UiSmokeCheck(hasLockHatch,
                     "Locked video track draws a hatch above its clips");
        clickTrackControl(0, 66.0);

        if (initialAudioTrackId) {
            const DocumentTrack* audioTrack =
                self.state->document.FindTrack(*initialAudioTrackId);
            UiSmokeCheck(audioTrack && !audioTrack->solo && !audioTrack->muted,
                         "Audio tracks start with Solo and Mute disabled");
            clickTrackControl(1, 48.0);
            clickTrackControl(1, 66.0);
            clickTrackControl(1, 84.0);
            audioTrack = self.state->document.FindTrack(*initialAudioTrackId);
            UiSmokeCheck(audioTrack && audioTrack->solo && audioTrack->muted &&
                             audioTrack->locked,
                         "Audio S, M and lock controls persist their state");
            clickTrackControl(1, 48.0);
            clickTrackControl(1, 66.0);
            clickTrackControl(1, 84.0);
        }

        self.state->timelineIn = RationalTime{10, 25};
        self.state->timelineOut = RationalTime{30, 25};
        const TimelineRenderData rangeData = [self timelineRenderData];
        size_t clipCommand = rangeData.overlays.size();
        size_t rangeCommand = rangeData.overlays.size();
        for (size_t index = 0; index < rangeData.overlays.size(); ++index) {
            const MetalRect* rect =
                std::get_if<MetalRect>(&rangeData.overlays[index]);
            if (!rect) continue;
            if (clipCommand == rangeData.overlays.size() &&
                rect->height == 40.0 &&
                rect->x >= self.state->viewport.header_width)
                clipCommand = index;
            if (std::abs(rect->alpha - 0.10f) < 0.001f) rangeCommand = index;
        }
        UiSmokeCheck(clipCommand < rangeCommand &&
                         rangeCommand < rangeData.overlays.size(),
                     "In/Out overlay is drawn above timeline clips");
        self.state->timelineIn.reset();
        self.state->timelineOut.reset();

        self.state->interaction->SelectClip(*initialClipId);
        [self updateSelectionInfo];
        NSMutableArray<NSSlider*>* sliders = [NSMutableArray array];
        CollectSliders(self.inspectorView, sliders);
        UiSmokeCheck(sliders.count == ui::inspector::GradeControls().size(),
                     "Inspector exposes every grading slider");
        if (sliders.count > 0) {
            NSSlider* exposure = sliders.firstObject;
            exposure.floatValue = 0.5f;
            [exposure sendAction:exposure.action to:exposure.target];
            NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:0.2];
            while (deadline.timeIntervalSinceNow > 0.0)
                [NSRunLoop.currentRunLoop runMode:NSDefaultRunLoopMode
                                       beforeDate:deadline];
            const DocumentClip* graded =
                self.state->document.FindClip(*initialClipId);
            const float value =
                graded ? ui::inspector::CurrentGradeControlValue(
                             graded->effects,
                             ui::inspector::GradeControls().front())
                       : 0.0f;
            UiSmokeCheck(std::abs(value - 0.5f) < 0.001f,
                         "Inspector slider commits through EditLog");
            if (graded && initialTrackId && graded->duration.value > 1) {
                const RationalTime cut = graded->timeline_in.add(
                    {graded->duration.value / 2, graded->duration.rate});
                const DocumentTrack* beforeTrack =
                    self.state->document.FindTrack(*initialTrackId);
                const size_t beforeCount =
                    beforeTrack ? beforeTrack->clips.size() : 0;
                SendKeyThroughApplication(exposure, @"c", 8);
                UiSmokeCheck([self effectiveTool] == TimelineTool::Cut,
                             "Inspector focus still accepts the Cut shortcut");
                const double timelineY =
                    kTimelineRulerHeight +
                    self.state->viewport.track_height * 0.5;
                const NSPoint cutPoint =
                    NSMakePoint(self.state->viewport.TimeToX(cut),
                                self.metalView.bounds.size.height - timelineY);
                SendTimelineMouseGesture(self.metalView, cutPoint, cutPoint);
                const DocumentTrack* afterTrack =
                    self.state->document.FindTrack(*initialTrackId);
                const DocumentClip* right = nullptr;
                if (afterTrack) {
                    for (const DocumentClip& candidate : afterTrack->clips) {
                        if (candidate.timeline_in == cut) {
                            right = &candidate;
                            break;
                        }
                    }
                }
                UiSmokeCheck(
                    afterTrack && afterTrack->clips.size() > beforeCount &&
                        right && right->effects.size() == 1 &&
                        std::abs(ui::inspector::CurrentGradeControlValue(
                                     right->effects,
                                     ui::inspector::GradeControls().front()) -
                                 0.5f) < 0.001f,
                    "Cutting from Inspector focus preserves grading");
                [self setTimelineTool:TimelineTool::Select];
            }
        }

        const DocumentTrack* multiTrack =
            self.state->document.FindTrack(*initialTrackId);
        if (multiTrack && multiTrack->clips.size() >= 2) {
            const Ulid firstSelected = multiTrack->clips[0].id;
            const Ulid secondSelected = multiTrack->clips[1].id;
            const std::string beforeDelete =
                self.state->document.SaveToString();
            const size_t beforeDeleteEdits = self.state->editLog.AppliedCount();
            self.state->interaction->SelectClips(
                {firstSelected, secondSelected});
            SendKeyThroughWindow(self.metalView, @"\x7f", 51);
            UiSmokeCheck(
                !self.state->document.FindClip(firstSelected) &&
                    !self.state->document.FindClip(secondSelected) &&
                    self.state->editLog.AppliedCount() ==
                        beforeDeleteEdits + 1 &&
                    std::holds_alternative<ClearClipsOperation>(
                        self.state->editLog.AppliedEntries().back().op),
                "Delete clears an arbitrary multi-clip selection atomically");
            [self menuUndo:nil];
            UiSmokeCheck(self.state->document.SaveToString() == beforeDelete,
                         "Undo restores a multi-clip delete byte-for-byte");
        }
    }

    const Ulid sourceId = self.state->document.sources.front().id;
    NSString* sourceIdentifier =
        [NSString stringWithUTF8String:sourceId.c_str()];
    NSString* timelineIdentifier =
        [NSString stringWithUTF8String:self.state->activeTimelineId.c_str()];
    const NSUInteger timelineIconIndex =
        [self.visibleMediaIds indexOfObject:timelineIdentifier];
    id<NSPasteboardWriting> timelineWriter =
        timelineIconIndex == NSNotFound
            ? nil
            : [self collectionView:self.mediaCollection
                  pasteboardWriterForItemAtIndexPath:
                      [NSIndexPath indexPathForItem:timelineIconIndex
                                          inSection:0]];
    UiSmokeCheck([(NSPasteboardItem*)timelineWriter
                     stringForType:kCutmachineTimelinePasteboardType] != nil,
                 "Timeline icon provides a bin-placement drag payload");

    NSString* mediaBinIdentifier = [NSString
        stringWithUTF8String:self.state->document.bins.back().id.c_str()];
    [self refreshBinControlsSelecting:mediaBinIdentifier];
    const NSUInteger iconIndex =
        [self.visibleMediaIds indexOfObject:sourceIdentifier];
    UiSmokeCheck(iconIndex == 0 && self.visibleMediaIds.count >= 7,
                 "Nested 1_RUSHES fixture exposes its first rush at index 0");
    id<NSPasteboardWriting> iconWriter =
        iconIndex == NSNotFound
            ? nil
            : [self collectionView:self.mediaCollection
                  pasteboardWriterForItemAtIndexPath:
                      [NSIndexPath indexPathForItem:iconIndex inSection:0]];
    NSSet<NSIndexPath*>* iconPaths =
        iconIndex == NSNotFound
            ? [NSSet set]
            : [NSSet setWithObject:[NSIndexPath indexPathForItem:iconIndex
                                                       inSection:0]];
    NSEvent* iconDragEvent =
        [NSEvent mouseEventWithType:NSEventTypeLeftMouseDragged
                           location:NSZeroPoint
                      modifierFlags:0
                          timestamp:NSProcessInfo.processInfo.systemUptime
                       windowNumber:self.window.windowNumber
                            context:nil
                        eventNumber:0
                         clickCount:1
                           pressure:1.0];
    UiSmokeCheck([(NSPasteboardItem*)iconWriter
                     stringForType:kCutmachineMediaPasteboardType] != nil,
                 "Icon-mode media item provides a timeline drag payload");
    UiSmokeCheck([self collectionView:self.mediaCollection
                     canDragItemsAtIndexPaths:iconPaths
                                    withEvent:iconDragEvent] &&
                     self.mediaCollection.gestureRecognizers.count == 0,
                 "Icon-mode drag is enabled without a competing click "
                 "recognizer");
    UiSmokeCheck((kMediaLocalDragOperations & NSDragOperationCopy) != 0 &&
                     (kMediaLocalDragOperations & NSDragOperationMove) != 0,
                 "Media drag supports timeline copy and bin move destinations");
    UiSmokeCheck([self.mediaCollection.registeredDraggedTypes
                     containsObject:kCutmachineMediaPasteboardType] &&
                     [self.mediaCollection.registeredDraggedTypes
                         containsObject:kCutmachineBinPasteboardType] &&
                     [self.mediaCollection.registeredDraggedTypes
                         containsObject:kCutmachineTimelinePasteboardType] &&
                     [self.mediaCollection.registeredDraggedTypes
                         containsObject:NSPasteboardTypeFileURL],
                 "Icon grid accepts media, timeline, bin, and Finder drops");
    if (iconIndex != NSNotFound && initialTrackId) {
        NSIndexPath* iconPath = [NSIndexPath indexPathForItem:iconIndex
                                                    inSection:0];
        [self.mediaCollection
            scrollToItemsAtIndexPaths:[NSSet setWithObject:iconPath]
                       scrollPosition:
                           NSCollectionViewScrollPositionNearestHorizontalEdge |
                           NSCollectionViewScrollPositionNearestVerticalEdge];
        [self.mediaCollection layoutSubtreeIfNeeded];
        NSCollectionViewItem* iconItem =
            [self.mediaCollection itemAtIndexPath:iconPath];
        NSCollectionViewLayoutAttributes* iconAttributes =
            [self.mediaCollection.collectionViewLayout
                layoutAttributesForItemAtIndexPath:iconPath];
        const NSRect visualIconFrame =
            iconItem ? [iconItem.view convertRect:iconItem.view.bounds
                                           toView:self.mediaCollection]
                     : NSZeroRect;
        const double timelineY =
            kTimelineRulerHeight + self.state->viewport.track_height * 0.5;
        const NSPoint destination =
            NSMakePoint(self.state->viewport.TimeToX({60, 25}),
                        self.metalView.bounds.size.height - timelineY);
        NSIndexPath* resolvedIconPath =
            iconItem
                ? [(ContextCollectionView*)self.mediaCollection
                      iconIndexPathAtPoint:NSMakePoint(NSMidX(visualIconFrame),
                                                       NSMidY(visualIconFrame))]
                : nil;
        UiSmokeCheck(resolvedIconPath.item == iconPath.item,
                     "Icon hit testing preserves top-to-bottom coordinates");
        bool gridSpansMultipleRows = false;
        for (NSCollectionViewItem* visibleItem in self.mediaCollection
                 .visibleItems) {
            const NSRect visibleFrame =
                [visibleItem.view convertRect:visibleItem.view.bounds
                                       toView:self.mediaCollection];
            if (std::abs(NSMidY(visibleFrame) - NSMidY(visualIconFrame)) >
                NSHeight(visualIconFrame) * 0.5) {
                gridSpansMultipleRows = true;
                break;
            }
        }
        UiSmokeCheck(gridSpansMultipleRows,
                     "Icon click fixture covers multiple visual rows");
        if (iconItem) {
            // Reproduce AppKit cell reuse after entering a bin: even if a
            // tile forwards an obsolete lower index, current visual geometry
            // must win.
            ((MediaIconView*)iconItem.view).indexPath =
                [NSIndexPath indexPathForItem:self.visibleMediaIds.count - 1
                                    inSection:0];
            self.mediaCollection.selectionIndexPaths = [NSSet set];
            SendWindowClick(
                self.mediaCollection,
                NSMakePoint(NSMidX(visualIconFrame), NSMidY(visualIconFrame)));
            UiSmokeCheck([self.mediaCollection.selectionIndexPaths
                             containsObject:iconPath],
                         "A physical click selects the first rush tile");
        }
        if (iconItem) {
            const NSPoint labelCenterInCollection = [iconItem.textField
                convertPoint:NSMakePoint(NSMidX(iconItem.textField.bounds),
                                         NSMidY(iconItem.textField.bounds))
                      toView:self.mediaCollection];
            NSIndexPath* labelPath =
                [(ContextCollectionView*)self.mediaCollection
                    iconIndexPathAtPoint:labelCenterInCollection];
            const NSPoint labelCenter = [iconItem.view
                convertPoint:NSMakePoint(NSMidX(iconItem.textField.bounds),
                                         NSMidY(iconItem.textField.bounds))
                    fromView:iconItem.textField];
            UiSmokeCheck(
                labelPath.item == iconPath.item &&
                    [iconItem.view hitTest:labelCenter] == iconItem.view &&
                    iconItem.textField.editable,
                "The first rush label resolves to its own icon item");
            const NSPoint labelWindowPoint =
                [self.mediaCollection convertPoint:labelCenterInCollection
                                            toView:nil];
            NSEvent* rightClick = [NSEvent
                mouseEventWithType:NSEventTypeRightMouseDown
                          location:labelWindowPoint
                     modifierFlags:0
                         timestamp:NSProcessInfo.processInfo.systemUptime
                      windowNumber:self.window.windowNumber
                           context:nil
                       eventNumber:0
                        clickCount:1
                          pressure:1.0];
            NSView* content = self.window.contentView;
            NSView* physicalHit = [content
                hitTest:[content convertPoint:labelWindowPoint fromView:nil]];
            UiSmokeCheck(
                [physicalHit isKindOfClass:MediaIconView.class] ||
                    physicalHit == self.mediaCollection,
                "Window hit-test routes the icon label to the unified tile "
                "handler");
            NSMenu* physicalMenu = [physicalHit menuForEvent:rightClick];
            UiSmokeCheck(physicalMenu == self.mediaCollection.menu,
                         "A physical right-click reaches the icon context "
                         "menu");
            UiSmokeCheck([self.mediaCollection.selectionIndexPaths
                             containsObject:iconPath],
                         "A physical right-click selects its icon item");
            self.mediaCollection.selectionIndexPaths = [NSSet set];
            SendWindowClick(self.mediaCollection, labelCenterInCollection);
            UiSmokeCheck([self.mediaCollection.selectionIndexPaths
                             containsObject:iconPath] &&
                             iconItem.textField.currentEditor != nil,
                         "A physical name click starts inline editing "
                         "immediately on the first rush");
            NSTextView* editor = (NSTextView*)iconItem.textField.currentEditor;
            if (editor) {
                editor.string = @"Rush renommé par clic";
                [self.window makeFirstResponder:self.mediaCollection];
            }
            const ProjectBinMetadata* clickedRename =
                self.state->project.FindBinMetadata(sourceId);
            UiSmokeCheck(clickedRename && clickedRename->display_name ==
                                              "Rush renommé par clic",
                         "Physical icon rename persists through the project "
                         "edit log");
            const int reloadsBeforeRenameHistory = gUiSmokeDecodeReloads;
            [self menuUndo:nil];
            const ProjectBinMetadata* undoneRename =
                self.state->project.FindBinMetadata(sourceId);
            UiSmokeCheck(
                (!undoneRename ||
                 undoneRename->display_name != "Rush renommé par clic") &&
                    gUiSmokeDecodeReloads == reloadsBeforeRenameHistory,
                "Undoing a rename does not restart media decoders");
            [self menuRedo:nil];
            const ProjectBinMetadata* redoneRename =
                self.state->project.FindBinMetadata(sourceId);
            UiSmokeCheck(
                redoneRename &&
                    redoneRename->display_name == "Rush renommé par clic" &&
                    gUiSmokeDecodeReloads == reloadsBeforeRenameHistory,
                "Redoing a rename stays on the lightweight refresh path");
        }
        [self.mediaCollection layoutSubtreeIfNeeded];
        iconItem = [self.mediaCollection itemAtIndexPath:iconPath];
        iconAttributes = [self.mediaCollection.collectionViewLayout
            layoutAttributesForItemAtIndexPath:iconPath];
        if (iconItem) {
            const NSRect dragFrame =
                [iconItem.view convertRect:iconItem.view.bounds
                                    toView:self.mediaCollection];
            SendWindowDragGesture(
                self.mediaCollection,
                NSMakePoint(NSMidX(dragFrame), NSMidY(dragFrame)),
                self.metalView, destination);
        }
        UiSmokeCheck(gUiSmokeIconMouseDown,
                     "Physical icon drag reaches the collection mouseDown");
        UiSmokeCheck(gUiSmokeIconDragSession,
                     "Physical icon gesture starts an AppKit drag session");
        UiSmokeCheck(iconItem && iconAttributes,
                     "Physical icon drag uses a visible collection item");
    }
    NSTableColumn* nameColumn =
        [self.mediaTable tableColumnWithIdentifier:@"name"];
    BOOL everyListNameIsEditable = nameColumn != nil;
    for (NSInteger row = 0;
         row < (NSInteger)self.visibleMediaIds.count && everyListNameIsEditable;
         ++row) {
        NSTableCellView* cell =
            (NSTableCellView*)[self tableView:self.mediaTable
                           viewForTableColumn:nameColumn
                                          row:row];
        everyListNameIsEditable =
            [cell.textField isKindOfClass:BrowserRenameTextField.class] &&
            cell.textField.editable &&
            [cell.textField.identifier hasPrefix:@"browser:"];
    }
    UiSmokeCheck(everyListNameIsEditable,
                 "List-mode names are inline-editable for every item kind");

    const auto commitInlineName = [&](NSString* identifier, NSString* name) {
        NSTextField* field = [NSTextField textFieldWithString:name];
        field.identifier = [@"browser:" stringByAppendingString:identifier];
        NSNotification* ended = [NSNotification
            notificationWithName:NSControlTextDidEndEditingNotification
                          object:field
                        userInfo:@{
                            NSTextMovementUserInfoKey : @(NSReturnTextMovement)
                        }];
        [self controlTextDidEndEditing:ended];
    };
    NSString* smokeBinIdentifier = [NSString
        stringWithUTF8String:self.state->document.bins.front().id.c_str()];
    commitInlineName(smokeBinIdentifier, @"Chutier renommé");
    commitInlineName(timelineIdentifier, @"Timeline renommée");
    commitInlineName(sourceIdentifier, @"Rush renommé");
    const ProjectBinMetadata* renamedMetadata =
        self.state->project.FindBinMetadata(sourceId);
    UiSmokeCheck(
        self.state->document.bins.front().name == "Chutier renommé" &&
            self.state->project.FindTimeline(self.state->activeTimelineId)
                    ->name == "Timeline renommée" &&
            renamedMetadata &&
            renamedMetadata->display_name == "Rush renommé" &&
            self.state->document.FindLibraryMedia(sourceId)->filename ==
                "00-fixture.mov",
        "Inline names persist for bins, timelines, and rushes without "
        "renaming the source file");
    self.state->sourceMonitor = true;
    self.state->sourceMonitorActive = true;
    self.state->sourceMonitorId = sourceId;
    self.state->sourceIn.reset();
    self.state->sourceOut.reset();
    self.state->timelineIn.reset();
    self.state->timelineOut.reset();
    self.state->requestedPosition = {0, 25};
    NSButton* insert = FindButtonWithTitle(self.sourceMonitorView, @"INSÉRER");
    UiSmokeCheck(insert != nil, "Source Insert button exists");
    if (insert && initialTrackId) {
        const NSPoint insertPoint =
            NSMakePoint(NSMidX(insert.frame), NSMidY(insert.frame));
        UiSmokeCheck([self.sourceMonitorView hitTest:insertPoint] == insert,
                     "Source Insert button is hit-testable above Metal");
        const DocumentTrack* beforeTrack =
            self.state->document.FindTrack(*initialTrackId);
        const size_t before = beforeTrack ? beforeTrack->clips.size() : 0;
        [insert performClick:nil];
        const DocumentTrack* edited =
            self.state->document.FindTrack(*initialTrackId);
        UiSmokeCheck(edited && edited->clips.size() > before,
                     "Source Insert button performs a persisted edit");
    }

    NSButton* overwrite =
        FindButtonWithTitle(self.sourceMonitorView, @"ÉCRASER");
    UiSmokeCheck(overwrite != nil, "Source Overwrite button exists");
    if (overwrite) {
        const NSPoint overwritePoint =
            NSMakePoint(NSMidX(overwrite.frame), NSMidY(overwrite.frame));
        UiSmokeCheck(
            [self.sourceMonitorView hitTest:overwritePoint] == overwrite,
            "Source Overwrite button is hit-testable above Metal");
        const size_t before = self.state->editLog.AppliedCount();
        [overwrite performClick:nil];
        UiSmokeCheck(self.state->editLog.AppliedCount() > before &&
                         [self.infoLabel.stringValue containsString:@"écrasé"],
                     "Source Overwrite button performs a persisted edit");
    }

    if (initialTrackId) {
        const size_t before = self.state->editLog.AppliedCount();
        const double timelineY =
            kTimelineRulerHeight + self.state->viewport.track_height * 0.5;
        const NSPoint point =
            NSMakePoint(self.state->viewport.TimeToX({75, 25}),
                        self.metalView.bounds.size.height - timelineY);
        const BOOL accepted = [self
            timelineDropMedia:[NSString stringWithUTF8String:sourceId.c_str()]
                  atViewPoint:point];
        UiSmokeCheck(accepted && self.state->editLog.AppliedCount() > before,
                     "Media drop performs a persisted timeline edit");
    }

    CAMetalLayer* sourceLayer = (CAMetalLayer*)self.sourceMonitorView.layer;
    CAMetalLayer* programLayer = (CAMetalLayer*)self.programMonitorView.layer;
    UiSmokeCheck(sourceLayer.pixelFormat == programLayer.pixelFormat &&
                     sourceLayer.colorspace != nullptr &&
                     programLayer.colorspace != nullptr &&
                     CFEqual(sourceLayer.colorspace, programLayer.colorspace) &&
                     sourceLayer.wantsExtendedDynamicRangeContent ==
                         programLayer.wantsExtendedDynamicRangeContent,
                 "Source and Record use the same Metal presentation policy");

    std::fprintf(stdout, "UI smoke: %d failure(s)\n", gUiSmokeFailures);
    std::fflush(stdout);
    // -terminate: exits the process before main can return the failure count.
    // Stop the run loop and wake it with a harmless application event so
    // ctest receives a non-zero status for every failed smoke assertion.
    [NSApp stop:nil];
    NSEvent* wake =
        [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                           location:NSZeroPoint
                      modifierFlags:0
                          timestamp:NSProcessInfo.processInfo.systemUptime
                       windowNumber:self.window.windowNumber
                            context:nil
                            subtype:0
                              data1:0
                              data2:0];
    [NSApp postEvent:wake atStart:NO];
}
#endif

- (void)applicationWillTerminate:(NSNotification*)notification {
    (void)notification;
    if (self.state) self.state->exportCancel.store(true);
    if (self.state && self.state->mediaTasks)
        self.state->mediaTasks->CancelAll();
    if (self.state->audioPlayback) self.state->audioPlayback->Stop();
    [self.displayTimer invalidate];
    self.displayTimer = nil;
    if (self.timelineShortcutMonitor) {
        [NSEvent removeMonitor:self.timelineShortcutMonitor];
        self.timelineShortcutMonitor = nil;
    }
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
    if (_timelineShortcutMonitor)
        [NSEvent removeMonitor:_timelineShortcutMonitor];
    delete _state;
}

@end

int main(int argc, char* argv[]) {
#if defined(CUTMACHINE_UI_SMOKE_TEST)
    if (argc == 2 && std::string(argv[1]) == "--ui-smoke") {
        std::string error;
        if (!PrepareUiSmokeProject(error)) {
            std::fprintf(stderr, "Unable to prepare UI smoke project: %s\n",
                         error.c_str());
            return 1;
        }
        gUiSmokeTesting = true;
    }
#endif
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
    if (argc > 2 || (argc == 2 && argv[1][0] == '-'
#if defined(CUTMACHINE_UI_SMOKE_TEST)
                     && !gUiSmokeTesting
#endif
                     )) {
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
        NSString* documentPath = nil;
#if defined(CUTMACHINE_UI_SMOKE_TEST)
        if (gUiSmokeTesting)
            documentPath =
                [NSString stringWithUTF8String:gUiSmokeProjectPath.c_str()];
        else
#endif
            documentPath =
                argc == 2 ? [NSString stringWithUTF8String:argv[1]] : nil;
        AppDelegate* delegate =
            [[AppDelegate alloc] initWithDocumentPath:documentPath];
        NSApp.delegate = delegate;
        [NSApp run];
    }
#if defined(CUTMACHINE_UI_SMOKE_TEST)
    if (gUiSmokeTesting) {
        std::error_code ignored;
        std::filesystem::remove_all(gUiSmokeRoot, ignored);
        return gUiSmokeFailures == 0 ? 0 : 1;
    }
#endif
    return 0;
}
