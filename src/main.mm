#import <AppKit/AppKit.h>

extern "C" {
#include <libavutil/frame.h>
}

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "Cli.h"
#include "DecodeWorker.h"
#include "Document.h"
#include "FrameCache.h"
#include "Ingest.h"
#include "PerformanceMetrics.h"
#include "Renderer.h"
#include "Timeline.h"

namespace {

constexpr size_t kGlobalCacheBudget = 2000000000ULL;  // 2.0 GB, all sources.

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
    std::unique_ptr<Timeline> timeline;
    std::unique_ptr<Renderer> renderer;
    std::unique_ptr<FrameCache> frameCache;
    std::unique_ptr<PerformanceMetrics> performanceMetrics;
    std::map<Ulid, std::unique_ptr<DecodeWorker>> workers;
    std::vector<Ulid> videoTrackIds;
    std::array<ResolvedSlot, 2> requested;
    std::array<RenderedSlot, 2> rendered;
    RationalTime duration{0, 1};
    int64_t requestedTick = 0;
};

}  // namespace

@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property(nonatomic, strong) NSWindow* window;
@property(nonatomic, strong) NSSlider* slider;
@property(nonatomic, strong) NSView* metalView;
@property(nonatomic, strong) NSTextField* hudLabel;
@property(nonatomic, strong) NSTimer* displayTimer;
@property(nonatomic, copy) NSString* documentPath;
@property(nonatomic, assign) AppState* state;
@property(nonatomic, assign) NSUInteger hudTick;
@end

@implementation AppDelegate

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
    self.state->timeline = std::make_unique<Timeline>(self.state->document);
    try {
        self.state->duration = self.state->timeline->Duration();
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "Invalid timeline duration: %s\n",
                     exception.what());
        return NO;
    }
    constexpr int64_t kLargestExactlyRepresentableSliderInteger = INT64_C(1)
                                                                  << 53;
    if (self.state->duration.value >
        kLargestExactlyRepresentableSliderInteger) {
        std::fprintf(
            stderr,
            "Timeline has too many ticks for NSSlider to transport exactly\n");
        return NO;
    }

    self.state->frameCache = std::make_unique<FrameCache>(kGlobalCacheBudget);
    self.state->performanceMetrics = std::make_unique<PerformanceMetrics>();
    const std::filesystem::path baseDirectory =
        std::filesystem::absolute(std::filesystem::path(documentPath))
            .parent_path();
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

    std::vector<const DocumentTrack*> videoTracks;
    for (const DocumentTrack& track : self.state->document.tracks) {
        if (track.kind == "video") {
            videoTracks.push_back(&track);
        }
    }
    std::sort(videoTracks.begin(), videoTracks.end(),
              [](const DocumentTrack* left, const DocumentTrack* right) {
                  return left->index < right->index;
              });
    if (videoTracks.size() > 2) {
        std::fprintf(stderr,
                     "Document has %zu video tracks; this milestone renders "
                     "only the first two\n",
                     videoTracks.size());
    }
    for (size_t index = 0; index < std::min<size_t>(2, videoTracks.size());
         ++index) {
        self.state->videoTrackIds.push_back(videoTracks[index]->id);
    }
    return YES;
}

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    (void)notification;
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

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

    NSView* content = [[NSView alloc] initWithFrame:windowRect];
    self.window.contentView = content;
    self.metalView = [[NSView alloc]
        initWithFrame:NSMakeRect(0.0, 80.0, windowRect.size.width,
                                 windowRect.size.height - 80.0)];
    self.metalView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [content addSubview:self.metalView];

    self.slider = [[NSSlider alloc]
        initWithFrame:NSMakeRect(20.0, 18.0, windowRect.size.width - 40.0,
                                 28.0)];
    self.slider.autoresizingMask = NSViewWidthSizable;
    self.slider.minValue = 0.0;
    // NSSlider's transport is necessarily double. The application converts
    // it immediately to an integer tick; all timeline calculations remain
    // rational.
    self.slider.maxValue = static_cast<double>(
        std::max<int64_t>(0, self.state->duration.value - 1));
    self.slider.continuous = YES;
    self.slider.target = self;
    self.slider.action = @selector(sliderChanged:);
    [content addSubview:self.slider];

    self.hudLabel = [NSTextField labelWithString:@"HUD warming up…"];
    self.hudLabel.frame =
        NSMakeRect(20.0, 50.0, windowRect.size.width - 40.0, 18.0);
    self.hudLabel.autoresizingMask = NSViewWidthSizable;
    self.hudLabel.font =
        [NSFont monospacedDigitSystemFontOfSize:12.0
                                         weight:NSFontWeightRegular];
    self.hudLabel.textColor = NSColor.secondaryLabelColor;
    self.hudLabel.lineBreakMode = NSLineBreakByTruncatingTail;
    [content addSubview:self.hudLabel];

    self.state->renderer = std::make_unique<Renderer>();
    if (!self.state->renderer->Initialize(self.metalView)) {
        std::fprintf(stderr, "Renderer initialization failed\n");
        [NSApp terminate:nil];
        return;
    }

    [self.window center];
    [self.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    for (auto& worker : self.state->workers) {
        worker.second->Start();
    }
    [self requestTimelineTick:0];
    self.displayTimer =
        [NSTimer scheduledTimerWithTimeInterval:(1.0 / 60.0)
                                         target:self
                                       selector:@selector(displayTick:)
                                       userInfo:nil
                                        repeats:YES];
}

- (void)requestTimelineTick:(int64_t)tick {
    self.state->requestedTick = tick;
    self.state->requested = {};
    const RationalTime position{tick, self.state->duration.rate};
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

- (void)sliderChanged:(NSSlider*)sender {
    const int64_t tick = static_cast<int64_t>(sender.integerValue);
    sender.integerValue = static_cast<NSInteger>(tick);
    [self requestTimelineTick:tick];
    [self presentNearestFrameAtDeadline:NO];
}

- (void)displayTick:(NSTimer*)timer {
    (void)timer;
    [self presentNearestFrameAtDeadline:YES];
    if (++self.hudTick % 6 == 0) {
        [self updateHUD];
    }
}

- (void)presentNearestFrameAtDeadline:(BOOL)isDisplayDeadline {
    if (!self.state->frameCache || !self.state->renderer) {
        return;
    }

    std::array<AVFrame*, 2> frames = {nullptr, nullptr};
    std::array<RenderedSlot, 2> candidates = {};
    bool missing = false;
    for (size_t slot = 0; slot < 2; ++slot) {
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
    if (!(candidates[0] == self.state->rendered[0]) ||
        !(candidates[1] == self.state->rendered[1])) {
        if (self.state->renderer->RenderFrames(frames[0], frames[1], 0.5f)) {
            self.state->rendered = candidates;
        }
    }
    av_frame_free(&frames[0]);
    av_frame_free(&frames[1]);
}

- (void)updateHUD {
    const PerformanceMetrics::Snapshot snapshot =
        self.state->performanceMetrics->GetSnapshot();
    const double residentGB =
        static_cast<double>(self.state->frameCache->TotalBytes()) / 1.0e9;
    self.hudLabel.stringValue = [NSString
        stringWithFormat:@"tick %lld/%lld @ %d    hit p50/p95/p99 "
                         @"%lld.%03lld/%lld.%03lld/%lld.%03lld ms    "
                          "miss %lld.%03lld/%lld.%03lld/%lld.%03lld ms    "
                          "hit-rate %.1f%%    resident %.2f GB    "
                          "drops %llu    in-flight %d",
                         static_cast<long long>(self.state->requestedTick),
                         static_cast<long long>(self.state->duration.value),
                         self.state->duration.rate,
                         static_cast<long long>(snapshot.hitP50Us / 1000),
                         static_cast<long long>(snapshot.hitP50Us % 1000),
                         static_cast<long long>(snapshot.hitP95Us / 1000),
                         static_cast<long long>(snapshot.hitP95Us % 1000),
                         static_cast<long long>(snapshot.hitP99Us / 1000),
                         static_cast<long long>(snapshot.hitP99Us % 1000),
                         static_cast<long long>(snapshot.missP50Us / 1000),
                         static_cast<long long>(snapshot.missP50Us % 1000),
                         static_cast<long long>(snapshot.missP95Us / 1000),
                         static_cast<long long>(snapshot.missP95Us % 1000),
                         static_cast<long long>(snapshot.missP99Us / 1000),
                         static_cast<long long>(snapshot.missP99Us % 1000),
                         snapshot.hitRate * 100.0, residentGB,
                         static_cast<unsigned long long>(snapshot.drops),
                         snapshot.framesInFlight];
}

- (void)windowDidResize:(NSNotification*)notification {
    (void)notification;
    if (self.state->renderer) {
        self.state->renderer->Resize(self.metalView.bounds);
    }
}

- (void)applicationWillTerminate:(NSNotification*)notification {
    (void)notification;
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
    if ((argc == 4 || argc == 5) &&
        std::string(argv[1]) == "--ingest" &&
        (argc == 4 || std::string(argv[4]) == "--recursive")) {
        std::string output;
        const int result =
            IngestCommand(argv[2], argv[3], argc == 5, output);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    if (argc != 2 || (argc >= 2 && argv[1][0] == '-')) {
        std::fprintf(
            stderr,
            "Usage: %s /path/to/timeline.json\n"
            "       %s --describe /path/to/timeline.json\n"
            "       %s --apply-op /path/to/timeline.json '<op.json>'\n"
            "       %s --ingest /path/to/timeline.json /path/to/media "
            "[--recursive]\n",
            argv[0], argv[0], argv[0], argv[0]);
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
