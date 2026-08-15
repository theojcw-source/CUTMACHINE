#pragma once

// AppKit-only. Unverified beyond a manual read -- see the F2.1 report.
//
// Reusable chrome for ROADMAP.md F2.1: the primitives F2.2 (Inspector),
// F2.3 (Media panel), F2.4 (Chat) and F2.5 (playback controls) each build
// their specific content on top of. Styled from UiTheme.h's tokens instead
// of stock AppKit bezels/colors -- PHILOSOPHY.md principle 8 ("le rendu est
// intégralement dessiné, donc rien n'oblige à reproduire l'esthétique par
// défaut"). Layout is frame-based with autoresizing masks, matching every
// other view in main.mm (this project does not use Auto Layout anywhere).

#import <AppKit/AppKit.h>

// A titled container: a themed header bar with a title label, and a plain
// content view below it that the owner fills in. `contentView`'s frame
// tracks the chrome's bounds automatically as the chrome is resized.
@interface CMPanelChromeView : NSView

@property(nonatomic, copy) NSString* title;
@property(nonatomic, readonly) NSView* contentView;

- (instancetype)initWithFrame:(NSRect)frame title:(NSString*)title;

@end

// A fixed-height row pairing a left-aligned label with a right-hand
// container the caller positions its own control into (button, slider,
// popup, ...). An optional monospaced value label sits between them --
// e.g. an Inspector property row showing "Exposition" / a slider / "+0.4".
// `valueLabel` and `controlContainer` are always present; leave either
// empty/hidden when a given row does not need it.
@interface CMControlRowView : NSView

@property(nonatomic, readonly) NSTextField* label;
@property(nonatomic, readonly) NSTextField* valueLabel;
@property(nonatomic, readonly) NSView* controlContainer;

- (instancetype)initWithFrame:(NSRect)frame labelText:(NSString*)labelText;

@end

// A small-caps section label used to break up a panel's content into named
// groups (e.g. Inspector's "Transformation" / "Couleur" sections).
NSTextField* CMMakeSectionHeader(NSString* title);

// Buttons/sliders styled to match the timeline's palette. Behave like their
// stock AppKit counterparts (same target/action wiring); only the visual
// styling differs.
NSButton* CMMakeStyledButton(NSString* title, id target, SEL action);
NSButton* CMMakeStyledToggle(NSString* title, id target, SEL action);
NSSlider* CMMakeStyledSlider(double minValue, double maxValue, id target,
                             SEL action);

// A lightweight, non-dock-bound tab strip: an ordered list of titles, evenly
// divided across the strip's width, styled the same way CMPanelHostView
// (PanelHostView.h) highlights its active dock-level tab. Unlike
// CMPanelHostView, this is not tied to PanelLayout.h's fixed dock/slot
// registry -- it is for tabs *within* a single already-fixed panel slot
// (e.g. ROADMAP.md F2.3's Media/Audio/Captions tabs, all inside the one
// PanelSlot::Media the Left dock reserves -- see PanelLayout.h's file
// comment on why that slot does not use CMPanelHostView's own tab chrome).
@interface CMTabStripView : NSView

@property(nonatomic, readonly) NSInteger selectedIndex;
@property(nonatomic, weak) id target;
// Invoked with `self` as the sole argument on every tab change, mouse or
// programmatic (-selectIndex:), mirroring CMPanelHostView's target/action
// shape for its own tab buttons.
@property(nonatomic) SEL action;

- (instancetype)initWithFrame:(NSRect)frame titles:(NSArray<NSString*>*)titles;
- (void)selectIndex:(NSInteger)index;

@end
