#pragma once

// AppKit-only. This sandbox has no Xcode/AppKit toolchain, so this file is
// unverified beyond a manual read -- see the F2.1 report for exactly what
// that means. Every AppKit type it touches (NSColor, NSFont) is a thin,
// stateless projection of UiTheme.h's plain-C++ tokens, kept in its own
// file so UiTheme.h itself stays portable and testable on Linux.
//
// The `CM` prefix on every design-system symbol here and in
// UiComponents.h/PanelHostView.h/UiPreferences.h stands for CUTMACHINE, the
// same way `Cutmachine` does on CutmachineSplitView (main.mm) -- not
// CoreMedia. No file in this project imports <CoreMedia/CoreMedia.h>, so
// there is no symbol collision, only the naming coincidence.

#import <AppKit/AppKit.h>

#include "UiTheme.h"

// Converts a portable token into the matching NSColor. Cocoa-facing design
// system code (UiComponents.mm, PanelHostView.mm) and future F2.2-F2.5
// panels should go through this rather than re-deriving NSColor literals --
// see UiTheme.h's file comment for why.
NSColor* CMColor(const ui::theme::Color& color);

// A system font at one of UiTheme.h's type-scale sizes. Matches the rest of
// main.mm's preference for the system font family (SF) over a custom one.
NSFont* CMFont(double pointSize, NSFontWeight weight);

// Convenience wrappers for the tokens panel chrome reaches for most often.
NSColor* CMSurfacePanelColor(void);
NSColor* CMSurfaceRaisedColor(void);
NSColor* CMSurfaceControlColor(void);
NSColor* CMBorderSubtleColor(void);
NSColor* CMTextPrimaryColor(void);
NSColor* CMTextSecondaryColor(void);
NSColor* CMAccentBlueColor(void);
