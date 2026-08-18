#pragma once

// AppKit-only. This compiles against a real macOS SDK, but nothing here has
// been exercised at runtime yet -- see VISUAL_QA_CHECKLIST.md for what still
// needs walking through (same caveat as PanelHostView.h/UiComponents.h).
//
// Legacy playback transport for ROADMAP.md F2.5: play/pause, frame step,
// scrub bar, current/total timecode. The ATELIER redesign removed its dock
// slot and no longer builds this AppKit view; the pure exact-time boundary in
// TransportBar.h remains independently tested.
//
// Like every other view in UiComponents.h, this is dumb chrome: it owns no
// playhead state of its own and mutates nothing in AppState/Document. Every
// value it *reads* comes from a -setPosition:duration:rate:/-setPlaying:
// call the owner makes; every value it *writes back* is a raw [0, 1] slider
// fraction handed to the target/action the owner wires onto scrubSlider --
// turning that fraction into a RationalTime, through TransportBar.h's
// ScrubBarRange and then TimelineView.h's QuantizePlayheadPosition, is the
// owner's job (main.mm), not this view's. This view itself never stores a
// pixel or fraction as if it were playhead state.

#import <AppKit/AppKit.h>

#include "Document.h"
#include "RationalTime.h"

@interface CMTransportView : NSView

// Target/action are nil on construction; the owner (main.mm) wires these up
// once, at startup, the same way it wires CMMakeStyledButton buttons
// elsewhere. -scrubSlider is continuous so dragging updates the playhead
// live, matching the main timeline's own ruler-drag scrub behavior.
@property(nonatomic, readonly) NSButton* stepBackButton;
@property(nonatomic, readonly) NSButton* playPauseButton;
@property(nonatomic, readonly) NSButton* stepForwardButton;
@property(nonatomic, readonly) NSSlider* scrubSlider;

- (instancetype)initWithFrame:(NSRect)frame;

// Updates the current/total timecode labels and the scrub knob position.
// Call this from the same choke point every playhead move already goes
// through (main.mm's -requestResolvedPosition:) so this view never
// disagrees with the timeline about "now". `rate` is the display rate used
// to format both timecodes -- pass the same MediaRate
// -[AppDelegate playheadFrameRate] already computes.
- (void)setPosition:(RationalTime)position
           duration:(RationalTime)duration
               rate:(MediaRate)rate;

// Swaps the play/pause glyph. `playing` is whether playbackDirection != 0.
- (void)setPlaying:(BOOL)playing;

@end
