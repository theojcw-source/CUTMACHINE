#import "UiThemeAppKit.h"

NSColor* CMThemeColor(const ui::theme::Color& color) {
    return [NSColor colorWithSRGBRed:color.r
                               green:color.g
                                blue:color.b
                               alpha:color.a];
}

NSFont* CMFont(double pointSize, NSFontWeight weight) {
    return [NSFont systemFontOfSize:pointSize weight:weight];
}

NSColor* CMSurfacePanelColor(void) {
    return CMThemeColor(ui::theme::kSurfacePanel);
}

NSColor* CMSurfaceRaisedColor(void) {
    return CMThemeColor(ui::theme::kSurfaceRaised);
}

NSColor* CMSurfaceControlColor(void) {
    return CMThemeColor(ui::theme::kSurfaceControl);
}

NSColor* CMBorderSubtleColor(void) {
    return CMThemeColor(ui::theme::kBorderSubtle);
}

NSColor* CMTextPrimaryColor(void) {
    return CMThemeColor(ui::theme::kTextPrimary);
}

NSColor* CMTextSecondaryColor(void) {
    return CMThemeColor(ui::theme::kTextSecondary);
}

NSColor* CMAccentColor(void) { return CMThemeColor(ui::theme::kAccent); }
