#pragma once

// AppKit-only. Every AppKit type it touches (NSColor, NSFont) is a thin,
// stateless projection of UiTheme.h's plain-C++ tokens, kept in its own
// file so UiTheme.h itself stays portable and testable on Linux.
//
// The `CM` prefix on every design-system symbol here and in
// UiComponents.h/PanelHostView.h/UiPreferences.h stands for CUTMACHINE, the
// same way `Cutmachine` does on CutmachineSplitView (main.mm) -- not
// CoreMedia.
//
// The token-to-NSColor entry point is deliberately CMThemeColor and not the
// shorter CMColor: <AppKit/AppKit.h> transitively pulls in ColorSync's
// deprecated QD headers, which already define a `CMColor` union. Taking that
// name compiles nowhere and fails only against a real macOS SDK, so keep the
// `Theme` infix on this one.

#import <AppKit/AppKit.h>

#include "UiTheme.h"

// Converts a portable token into the matching NSColor. Cocoa-facing design
// system code (UiComponents.mm, PanelHostView.mm) and future F2.2-F2.5
// panels should go through this rather than re-deriving NSColor literals --
// see UiTheme.h's file comment for why.
NSColor* CMThemeColor(const ui::theme::Color& color);

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
