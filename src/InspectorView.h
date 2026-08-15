#pragma once

// AppKit-only. This sandbox has no Xcode/AppKit toolchain, so this file
// (and InspectorView.mm) is unverified beyond a manual read -- same
// disclaimer as UiComponents.h/PanelHostView.h carry for F2.1. See the
// F2.2 report for exactly what "unverified" covers here.
//
// F2.2 -- ROADMAP.md: the Inspector panel's content view, installed into
// PanelSlot::Inspector via CMPanelHostView's -setContentView:forSlot: (see
// main.mm's -applicationDidFinishLaunching). Shows the currently selected
// clip's properties and its color.* grading stack (F1.3, ColorEffects.h)
// as a set of sliders built on UiComponents.h's CMControlRowView -- the
// exact pairing UiComponents.h's own file comment already anticipates
// ("e.g. an Inspector property row showing 'Exposition' / a slider /
// '+0.4'").
//
// This view holds no EditLog of its own: main.mm remains the single owner
// of edit state, matching every other in-place editor already in main.mm.
// Two entry points connect it to that state:
//
//   - -reloadWithDocument:selectedClipId: refreshes every row from a
//     Document snapshot the caller passes in. main.mm calls this from
//     -updateSelectionInfo, the hook already fired after every selection
//     change and every edit (see main.mm's existing call sites).
//   - -delegate is notified with a freshly built SetClipEffectsOperation
//     whenever a grading control commits an edit (drag-end or short
//     debounce, never one operation per pixel of drag -- see
//     InspectorView.mm). CMInspectorViewDelegate matches this codebase's
//     existing delegate-protocol convention (AppDelegate already conforms
//     to NSWindowDelegate/NSSplitViewDelegate) rather than introducing a
//     block-based callback, which nothing else in this project uses. The
//     delegate is responsible for actually calling EditLog::Apply with the
//     operation and for any follow-up (persistEdits, marking the frame
//     dirty for re-render) -- this view never mutates Document/DocumentClip
//     directly (PHILOSOPHY.md principle 2/3).

#import <AppKit/AppKit.h>

#include "Document.h"
#include "Operations.h"

@class CMInspectorView;

@protocol CMInspectorViewDelegate <NSObject>

// `operation` is ready to submit to EditLog::Apply as-is (clip_id and the
// clip's complete new effects vector already set) -- see the file comment
// above.
- (void)inspectorView:(CMInspectorView*)inspectorView
    didCommitClipEffects:(SetClipEffectsOperation)operation;

@end

@interface CMInspectorView : NSView

@property(nonatomic, weak, nullable) id<CMInspectorViewDelegate> delegate;

// Refreshes every property row and grading slider from `document`'s
// current state for `clipId`. Pass an empty Ulid (or the id of a clip the
// document no longer has) to show the "no selection" placeholder instead.
- (void)reloadWithDocument:(const Document&)document
             selectedClipId:(const Ulid&)clipId;

@end
