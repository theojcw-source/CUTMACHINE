#import "UiThemeAppKit.h"

NSColor* CMColor(const ui::theme::Color& color) {
    return [NSColor colorWithSRGBRed:color.r
                                green:color.g
                                 blue:color.b
                                alpha:color.a];
}

NSFont* CMFont(double pointSize, NSFontWeight weight) {
    return [NSFont systemFontOfSize:pointSize weight:weight];
}

NSColor* CMSurfacePanelColor(void) {
    return CMColor(ui::theme::kSurfacePanel);
}

NSColor* CMSurfaceRaisedColor(void) {
    return CMColor(ui::theme::kSurfaceRaised);
}

NSColor* CMSurfaceControlColor(void) {
    return CMColor(ui::theme::kSurfaceControl);
}

NSColor* CMBorderSubtleColor(void) {
    return CMColor(ui::theme::kBorderSubtle);
}

NSColor* CMTextPrimaryColor(void) {
    return CMColor(ui::theme::kTextPrimary);
}

NSColor* CMTextSecondaryColor(void) {
    return CMColor(ui::theme::kTextSecondary);
}

NSColor* CMAccentBlueColor(void) {
    return CMColor(ui::theme::kAccentBlue);
}
