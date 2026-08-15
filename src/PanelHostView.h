#pragma once

// AppKit-only. Unverified beyond a manual read -- see the F2.1 report.
//
// A fixed-slot tab host for one dock (ROADMAP.md F2.1). It builds its tabs
// once, in PanelLayout.h's FixedPanelLayout() order, for whichever slots
// are assigned to `dock`, and never adds, removes or reorders them
// afterward -- see PanelLayout.h's file comment for why. Each future panel
// ticket (F2.2 Inspector, F2.4 Chat, ...) calls -setContentView:forSlot:
// once, at startup, to install its real content in the slot this host
// already reserved for it. Until a ticket does that, the slot shows a
// plain placeholder so the host is inspectable/screenshot-able on its own.
//
// The only state this view remembers across launches is *which
// already-fixed tab was last selected*, via CMUiPreferences (NSUserDefaults
// -- a local preference, never project state). Tab selection itself, by
// mouse or by -selectSlot:, is not a rearrangement: the set of tabs and
// their order are compiled into PanelLayout.h and cannot change at runtime.

#import <AppKit/AppKit.h>

#include "PanelLayout.h"

@interface CMPanelHostView : NSView

- (instancetype)initWithFrame:(NSRect)frame dock:(ui::PanelDock)dock;

// `slot` must be one of the slots PanelLayout.h assigns to this host's
// dock. Passing any other slot is a programmer error and is a no-op.
// Passing nil reverts the slot to its placeholder.
- (void)setContentView:(nullable NSView*)view forSlot:(ui::PanelSlot)slot;

// Programmatic tab switch (e.g. selecting a clip could switch the right
// dock to Inspector). This changes which already-fixed tab is visible, not
// the set of tabs -- see the file comment above.
- (void)selectSlot:(ui::PanelSlot)slot;
- (ui::PanelSlot)selectedSlot;

@end
