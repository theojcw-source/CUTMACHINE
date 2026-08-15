#pragma once

// AppKit/Foundation-only. Unverified beyond a manual read -- see the F2.1
// report.
//
// PHILOSOPHY.md's amended non-buts: interface state persistence is a local
// preference, never project truth. "Panneaux déplaçables" (movable panels)
// are named explicitly as the kind of thing that may now be *remembered for
// comfort*, not the kind of thing that must exist. F2.1 does not build a
// rearrangeable layout at all (see PanelLayout.h) -- the only interface
// state this file remembers is which of the already-fixed tabs was last
// selected in a dock, and it remembers that in NSUserDefaults only.
//
// Nothing in this file ever touches Document, Project, ProjectStorage or
// any project package on disk. Re-opening the same project on a different
// machine, with a different (or absent) NSUserDefaults value here, must
// select the same tab a fresh install would: FixedPanelLayout()'s first
// entry for that dock, never something the project file remembers.

#import <Foundation/Foundation.h>

#include "PanelLayout.h"

@interface CMUiPreferences : NSObject

+ (instancetype)sharedPreferences;

// `identifier` should be a PanelSlotDescriptor.identifier from
// PanelLayout.h. Passing anything else is harmless (it round-trips) but
// pointless, since CMPanelHostView only ever asks for identifiers that are
// already in the fixed layout.
- (void)setLastActiveIdentifier:(NSString*)identifier
                         forDock:(ui::PanelDock)dock;

// Returns nil when no preference has been recorded yet (fresh install, or a
// project opened on a machine that never touched this dock before) --
// callers fall back to FixedPanelLayout()'s first slot for that dock, never
// to anything read from the project.
- (nullable NSString*)lastActiveIdentifierForDock:(ui::PanelDock)dock;

@end
