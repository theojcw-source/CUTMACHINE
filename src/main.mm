#import <AppKit/AppKit.h>

extern "C" {
#include <libavutil/frame.h>
}

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "DecodeWorker.h"
#include "FrameCache.h"
#include "PerformanceMetrics.h"
#include "Renderer.h"

namespace {

constexpr FrameCache::SourceId kFirstSourceId = 1;
constexpr FrameCache::SourceId kSecondSourceId = 2;
constexpr size_t kGlobalCacheBudget = 2000000000ULL;  // 2.0 GB, all sources.

}  // namespace

@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property(nonatomic, strong) NSWindow* window;
@property(nonatomic, strong) NSSlider* slider;
@property(nonatomic, strong) NSView* metalView;
@property(nonatomic, strong) NSTextField* hudLabel;
@property(nonatomic, strong) NSTimer* displayTimer;
@property(nonatomic, assign) Renderer* renderer;
@property(nonatomic, assign) FrameCache* frameCache;
@property(nonatomic, assign) PerformanceMetrics* performanceMetrics;
@property(nonatomic, assign) DecodeWorker* firstWorker;
@property(nonatomic, assign) DecodeWorker* secondWorker;
@property(nonatomic, assign) int64_t requestedFrame;
@property(nonatomic, assign) int64_t renderedFirstFrame;
@property(nonatomic, assign) int64_t renderedSecondFrame;
@property(nonatomic, assign) NSUInteger hudTick;
@property(nonatomic, copy) NSString* firstInputPath;
@property(nonatomic, copy) NSString* secondInputPath;
@end

@implementation AppDelegate

- (instancetype)initWithFirstInputPath:(NSString*)firstInputPath
                       secondInputPath:(NSString*)secondInputPath {
    if ((self = [super init])) {
        _firstInputPath = [firstInputPath copy];
        _secondInputPath = [secondInputPath copy];
        _renderedFirstFrame = -1;
        _renderedSecondFrame = -1;
    }
    return self;
}

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    (void)notification;
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

    const NSRect windowRect = NSMakeRect(0.0, 0.0, 1600.0, 960.0);
    self.window = [[NSWindow alloc]
        initWithContentRect:windowRect
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                             NSWindowStyleMaskResizable)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    self.window.title = @"CUTMACHINE — XAVC S-I 4K frame viewer";
    self.window.delegate = self;

    NSView* content = [[NSView alloc] initWithFrame:windowRect];
    self.window.contentView = content;

    self.metalView = [[NSView alloc]
        initWithFrame:NSMakeRect(0.0, 80.0, windowRect.size.width,
                                 windowRect.size.height - 80.0)];
    self.metalView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [content addSubview:self.metalView];

    self.slider = [[NSSlider alloc]
        initWithFrame:NSMakeRect(20.0, 18.0, windowRect.size.width - 40.0, 28.0)];
    self.slider.autoresizingMask = NSViewWidthSizable;
    self.slider.minValue = 0.0;
    self.slider.maxValue = 0.0;
    self.slider.continuous = YES;
    self.slider.target = self;
    self.slider.action = @selector(sliderChanged:);
    [content addSubview:self.slider];

    self.hudLabel = [NSTextField labelWithString:@"HUD warming up…"];
    self.hudLabel.frame = NSMakeRect(20.0, 50.0, windowRect.size.width - 40.0, 18.0);
    self.hudLabel.autoresizingMask = NSViewWidthSizable;
    self.hudLabel.font = [NSFont monospacedDigitSystemFontOfSize:12.0
                                                          weight:NSFontWeightRegular];
    self.hudLabel.textColor = NSColor.secondaryLabelColor;
    self.hudLabel.lineBreakMode = NSLineBreakByTruncatingTail;
    [content addSubview:self.hudLabel];

    self.renderer = new Renderer();
    if (!self.renderer->Initialize(self.metalView)) {
        std::fprintf(stderr, "Renderer initialization failed\n");
        [NSApp terminate:nil];
        return;
    }

    self.frameCache = new FrameCache(kGlobalCacheBudget);
    self.performanceMetrics = new PerformanceMetrics();
    self.firstWorker = new DecodeWorker(kFirstSourceId, *self.frameCache,
                                        *self.performanceMetrics);
    const std::string firstPath(self.firstInputPath.UTF8String ?: "");
    if (!self.firstWorker->Open(firstPath, 5)) {
        std::fprintf(stderr, "Unable to open media: %s\n", firstPath.c_str());
        [NSApp terminate:nil];
        return;
    }
    if (self.secondInputPath) {
        self.secondWorker = new DecodeWorker(kSecondSourceId, *self.frameCache,
                                             *self.performanceMetrics);
        const std::string secondPath(self.secondInputPath.UTF8String ?: "");
        if (!self.secondWorker->Open(secondPath, 5)) {
            std::fprintf(stderr, "Unable to open second media: %s\n", secondPath.c_str());
            [NSApp terminate:nil];
            return;
        }
    }

    int64_t frameCount = self.firstWorker->FrameCount();
    if (self.secondWorker) {
        frameCount = std::min(frameCount, self.secondWorker->FrameCount());
    }
    self.slider.maxValue = static_cast<double>(std::max<int64_t>(0, frameCount - 1));
    [self.window center];
    [self.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];

    self.firstWorker->Start();
    self.firstWorker->RequestFrame(0);
    if (self.secondWorker) {
        self.secondWorker->Start();
        self.secondWorker->RequestFrame(0);
    }
    self.displayTimer = [NSTimer scheduledTimerWithTimeInterval:(1.0 / 60.0)
                                                         target:self
                                                       selector:@selector(displayTick:)
                                                       userInfo:nil
                                                        repeats:YES];
}

- (void)sliderChanged:(NSSlider*)sender {
    self.requestedFrame = std::llround(sender.doubleValue);
    sender.doubleValue = static_cast<double>(self.requestedFrame);
    self.firstWorker->RequestFrame(self.requestedFrame);
    if (self.secondWorker) {
        self.secondWorker->RequestFrame(self.requestedFrame);
    }
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
    if (!self.frameCache || !self.renderer) {
        return;
    }
    int64_t firstCachedIndex = -1;
    int64_t secondCachedIndex = -1;
    AVFrame* firstFrame = self.frameCache->GetNearest(
        kFirstSourceId, self.requestedFrame, firstCachedIndex);
    AVFrame* secondFrame = self.secondWorker
        ? self.frameCache->GetNearest(kSecondSourceId, self.requestedFrame,
                                      secondCachedIndex)
        : nullptr;
    if (!firstFrame) {
        if (isDisplayDeadline) {
            self.performanceMetrics->RecordDrop();
        }
        av_frame_free(&secondFrame);
        return;
    }
    const bool firstMissing = firstCachedIndex != self.requestedFrame;
    const bool secondMissing = self.secondWorker &&
                               secondCachedIndex != self.requestedFrame;
    if (isDisplayDeadline && (firstMissing || secondMissing)) {
        self.performanceMetrics->RecordDrop();
    }
    if (firstCachedIndex != self.renderedFirstFrame ||
        secondCachedIndex != self.renderedSecondFrame) {
        if (self.renderer->RenderFrames(firstFrame, secondFrame, 0.5f)) {
            self.renderedFirstFrame = firstCachedIndex;
            self.renderedSecondFrame = secondCachedIndex;
        }
    }
    av_frame_free(&firstFrame);
    av_frame_free(&secondFrame);
}

- (void)updateHUD {
    const PerformanceMetrics::Snapshot snapshot =
        self.performanceMetrics->GetSnapshot();
    const double residentGB = static_cast<double>(self.frameCache->TotalBytes()) / 1.0e9;
    self.hudLabel.stringValue = [NSString stringWithFormat:
        @"hit p50/p95/p99 %.3f/%.3f/%.3f ms    miss %.2f/%.2f/%.2f ms    "
         "hit-rate %.1f%%    resident %.2f GB    drops %llu    in-flight %d",
        snapshot.hitP50Ms, snapshot.hitP95Ms, snapshot.hitP99Ms,
        snapshot.missP50Ms, snapshot.missP95Ms, snapshot.missP99Ms,
        snapshot.hitRate * 100.0, residentGB,
        static_cast<unsigned long long>(snapshot.drops), snapshot.framesInFlight];
}

- (void)windowDidResize:(NSNotification*)notification {
    (void)notification;
    if (self.renderer) {
        self.renderer->Resize(self.metalView.bounds);
    }
}

- (void)applicationWillTerminate:(NSNotification*)notification {
    (void)notification;
    [self.displayTimer invalidate];
    self.displayTimer = nil;
    if (self.firstWorker) {
        self.firstWorker->Stop();
    }
    if (self.secondWorker) {
        self.secondWorker->Stop();
    }
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
    (void)sender;
    return YES;
}

- (void)dealloc {
    [_displayTimer invalidate];
    if (_firstWorker) {
        _firstWorker->Stop();
    }
    if (_secondWorker) {
        _secondWorker->Stop();
    }
    delete _secondWorker;
    delete _firstWorker;
    delete _frameCache;
    delete _performanceMetrics;
    delete _renderer;
}

@end


int main(int argc, char* argv[]) {
    if (argc < 2 || argc > 3) {
        std::fprintf(stderr,
                     "Usage: %s /path/to/track1.mp4 [/path/to/track2.mp4]\n",
                     argv[0]);
        return 2;
    }

    @autoreleasepool {
        [NSApplication sharedApplication];
        NSString* firstInputPath = [NSString stringWithUTF8String:argv[1]];
        NSString* secondInputPath = argc == 3
            ? [NSString stringWithUTF8String:argv[2]]
            : nil;
        AppDelegate* delegate = [[AppDelegate alloc]
            initWithFirstInputPath:firstInputPath
                   secondInputPath:secondInputPath];
        NSApp.delegate = delegate;
        [NSApp run];
    }
    return 0;
}
