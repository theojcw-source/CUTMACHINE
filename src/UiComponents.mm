#import "UiComponents.h"

#include <algorithm>

#include "UiTheme.h"
#include "UiThemeAppKit.h"

namespace {

CGFloat CGF(double value) { return static_cast<CGFloat>(value); }

}  // namespace

@interface CMIndustrialButton : NSButton
@end

@implementation CMIndustrialButton {
    NSTrackingArea* _trackingArea;
    BOOL _hovered;
    CALayer* _hoverEdge;
    CALayer* _activeEdge;
}

- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        self.bordered = NO;
        self.wantsLayer = YES;
        self.focusRingType = NSFocusRingTypeExterior;
        _hoverEdge = [CALayer layer];
        _activeEdge = [CALayer layer];
        [self.layer addSublayer:_hoverEdge];
        [self.layer addSublayer:_activeEdge];
    }
    return self;
}

- (BOOL)wantsUpdateLayer {
    return YES;
}

- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    if (_trackingArea) [self removeTrackingArea:_trackingArea];
    _trackingArea = [[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:NSTrackingInVisibleRect | NSTrackingMouseEnteredAndExited |
                     NSTrackingActiveInActiveApp
               owner:self
            userInfo:nil];
    [self addTrackingArea:_trackingArea];
}

- (void)mouseEntered:(NSEvent*)event {
    (void)event;
    _hovered = YES;
    self.needsDisplay = YES;
}

- (void)mouseExited:(NSEvent*)event {
    (void)event;
    _hovered = NO;
    self.needsDisplay = YES;
}

- (void)setEnabled:(BOOL)enabled {
    [super setEnabled:enabled];
    self.needsDisplay = YES;
}

- (void)setState:(NSControlStateValue)state {
    [super setState:state];
    self.needsDisplay = YES;
}

- (void)setTitle:(NSString*)title {
    [super setTitle:title];
    self.needsDisplay = YES;
}

- (void)updateLayer {
    const BOOL pressed = self.highlighted;
    const BOOL active = self.state == NSControlStateValueOn;
    ui::theme::Color background = ui::theme::kSurfaceControl;
    if (!self.enabled)
        background = ui::theme::kSurfacePanel;
    else if (pressed)
        background = ui::theme::Mix(ui::theme::kSurfaceControl,
                                    ui::theme::kSurfaceBase, 0.20f);
    else if (_hovered || active)
        background = ui::theme::kSurfaceControlHi;
    self.layer.backgroundColor = CMThemeColor(background).CGColor;
    self.layer.cornerRadius = 0.0;

    const ui::theme::Color textColor =
        self.enabled
            ? (active ? ui::theme::kTextPrimary : ui::theme::kTextSecondary)
            : ui::theme::kTextTertiary;
    NSMutableParagraphStyle* style = [[NSMutableParagraphStyle alloc] init];
    style.alignment = NSTextAlignmentCenter;
    self.attributedTitle = [[NSAttributedString alloc]
        initWithString:self.title ?: @""
            attributes:@{
                NSForegroundColorAttributeName : CMThemeColor(textColor),
                NSFontAttributeName :
                    CMFont(ui::theme::kFontSizeSmall, NSFontWeightSemibold),
                NSParagraphStyleAttributeName : style,
            }];

    _hoverEdge.frame = CGRectMake(0.0, self.bounds.size.height - 2.0,
                                  self.bounds.size.width, 2.0);
    _hoverEdge.backgroundColor = CMThemeColor(ui::theme::kTextPrimary).CGColor;
    _hoverEdge.hidden = !_hovered || pressed || !self.enabled;
    _activeEdge.frame = CGRectMake(0.0, 0.0, self.bounds.size.width, 2.0);
    _activeEdge.backgroundColor = CMThemeColor(ui::theme::kAccent).CGColor;
    _activeEdge.hidden = !active || !self.enabled;
}

@end

@interface CMPanelChromeView ()
- (void)layoutChromeForSize:(NSSize)size;
@end

@implementation CMPanelChromeView {
    NSView* _headerView;
    NSTextField* _titleLabel;
    NSView* _contentView;
}

- (instancetype)initWithFrame:(NSRect)frame title:(NSString*)title {
    if ((self = [super initWithFrame:frame])) {
        self.wantsLayer = YES;
        self.layer.backgroundColor = CMSurfacePanelColor().CGColor;

        _headerView = [[NSView alloc] initWithFrame:NSZeroRect];
        _headerView.wantsLayer = YES;
        _headerView.layer.backgroundColor = CMSurfaceRaisedColor().CGColor;
        [self addSubview:_headerView];

        NSView* headerBorder = [[NSView alloc] initWithFrame:NSZeroRect];
        headerBorder.wantsLayer = YES;
        headerBorder.layer.backgroundColor = CMBorderSubtleColor().CGColor;
        headerBorder.identifier = @"CMPanelChromeHeaderBorder";
        [_headerView addSubview:headerBorder];

        _titleLabel = [NSTextField labelWithString:title ?: @""];
        _titleLabel.font =
            CMFont(ui::theme::kFontSizeSection, NSFontWeightSemibold);
        _titleLabel.textColor = CMTextSecondaryColor();
        _titleLabel.lineBreakMode = NSLineBreakByTruncatingTail;
        [_headerView addSubview:_titleLabel];

        _contentView = [[NSView alloc] initWithFrame:NSZeroRect];
        [self addSubview:_contentView];

        [self layoutChromeForSize:frame.size];
    }
    return self;
}

- (void)layoutChromeForSize:(NSSize)size {
    const CGFloat headerHeight = CGF(ui::theme::kPanelHeaderHeight);
    const CGFloat inset = CGF(ui::theme::kSpaceM);
    _headerView.frame =
        NSMakeRect(0.0, size.height - headerHeight, size.width, headerHeight);
    for (NSView* subview in _headerView.subviews) {
        if ([subview.identifier isEqualToString:@"CMPanelChromeHeaderBorder"])
            subview.frame = NSMakeRect(0.0, 0.0, size.width, 1.0);
    }
    _titleLabel.frame =
        NSMakeRect(inset, 0.0, std::max<CGFloat>(0.0, size.width - 2 * inset),
                   headerHeight);
    _contentView.frame =
        NSMakeRect(0.0, 0.0, size.width,
                   std::max<CGFloat>(0.0, size.height - headerHeight));
}

- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    [self layoutChromeForSize:newSize];
}

- (NSString*)title {
    return _titleLabel.stringValue;
}

- (void)setTitle:(NSString*)title {
    _titleLabel.stringValue = title ?: @"";
}

- (NSView*)contentView {
    return _contentView;
}

@end

@interface CMControlRowView ()
- (void)layoutRowForSize:(NSSize)size;
@end

@implementation CMControlRowView {
    NSTextField* _label;
    NSTextField* _valueLabel;
    NSView* _controlContainer;
}

- (instancetype)initWithFrame:(NSRect)frame labelText:(NSString*)labelText {
    if ((self = [super initWithFrame:frame])) {
        _label = [NSTextField labelWithString:labelText ?: @""];
        _label.font = CMFont(ui::theme::kFontSizeBody, NSFontWeightRegular);
        _label.textColor = CMTextPrimaryColor();
        _label.lineBreakMode = NSLineBreakByTruncatingTail;
        [self addSubview:_label];

        _valueLabel = [NSTextField labelWithString:@""];
        _valueLabel.font = [NSFont
            monospacedDigitSystemFontOfSize:CGF(ui::theme::kFontSizeBody)
                                     weight:NSFontWeightRegular];
        _valueLabel.textColor = CMTextSecondaryColor();
        _valueLabel.alignment = NSTextAlignmentRight;
        [self addSubview:_valueLabel];

        _controlContainer = [[NSView alloc] initWithFrame:NSZeroRect];
        [self addSubview:_controlContainer];

        [self layoutRowForSize:frame.size];
    }
    return self;
}

- (void)layoutRowForSize:(NSSize)size {
    // label | value | control, left to right -- see UiComponents.h.
    constexpr CGFloat kLabelWidth = 108.0;
    constexpr CGFloat kValueWidth = 56.0;
    const CGFloat inset = CGF(ui::theme::kSpaceM);
    const CGFloat gap = CGF(ui::theme::kSpaceS);
    _label.frame = NSMakeRect(inset, 0.0, kLabelWidth, size.height);
    _valueLabel.frame =
        NSMakeRect(inset + kLabelWidth, 0.0, kValueWidth, size.height);
    const CGFloat controlX = inset + kLabelWidth + kValueWidth + gap;
    _controlContainer.frame = NSMakeRect(
        controlX, 0.0, std::max<CGFloat>(0.0, size.width - controlX - inset),
        size.height);
}

- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    [self layoutRowForSize:newSize];
}

- (NSTextField*)label {
    return _label;
}

- (NSTextField*)valueLabel {
    return _valueLabel;
}

- (NSView*)controlContainer {
    return _controlContainer;
}

@end

NSTextField* CMMakeSectionHeader(NSString* title) {
    NSString* caption =
        [NSString stringWithFormat:@"— %@", title.uppercaseString ?: @""];
    NSTextField* header = [NSTextField labelWithString:caption];
    header.font = CMFont(ui::theme::kFontSizeCaption, NSFontWeightSemibold);
    header.textColor = CMThemeColor(ui::theme::kTextTertiary);
    NSMutableAttributedString* attributed = [[NSMutableAttributedString alloc]
        initWithString:caption
            attributes:@{
                NSForegroundColorAttributeName :
                    CMThemeColor(ui::theme::kTextTertiary),
                NSFontAttributeName :
                    CMFont(ui::theme::kFontSizeCaption, NSFontWeightSemibold),
            }];
    [attributed
        addAttributes:@{
            NSForegroundColorAttributeName : CMThemeColor(ui::theme::kAccent),
            NSFontAttributeName :
                CMFont(ui::theme::kFontSizeCaption, NSFontWeightBold),
        }
                range:NSMakeRange(0, std::min<NSUInteger>(1, caption.length))];
    header.attributedStringValue = attributed;
    return header;
}

NSButton* CMMakeStyledButton(NSString* title, id target, SEL action) {
    CMIndustrialButton* button =
        [[CMIndustrialButton alloc] initWithFrame:NSZeroRect];
    button.title = title ?: @"";
    button.target = target;
    button.action = action;
    button.font = CMFont(ui::theme::kFontSizeSmall, NSFontWeightMedium);
    return button;
}

NSButton* CMMakeStyledToggle(NSString* title, id target, SEL action) {
    NSButton* toggle = CMMakeStyledButton(title, target, action);
    [toggle setButtonType:NSButtonTypePushOnPushOff];
    return toggle;
}

NSSlider* CMMakeStyledSlider(double minValue, double maxValue, id target,
                             SEL action) {
    NSSlider* slider = [[NSSlider alloc] initWithFrame:NSZeroRect];
    slider.minValue = minValue;
    slider.maxValue = maxValue;
    slider.target = target;
    slider.action = action;
    slider.controlSize = NSControlSizeSmall;
    slider.trackFillColor = CMThemeColor(ui::theme::kAccent);
    return slider;
}

@interface CMTabStripView ()
- (void)cmLayoutStripForSize:(NSSize)size;
- (void)cmUpdateHighlighting;
- (void)cmButtonPressed:(NSButton*)sender;
@end

@implementation CMTabStripView {
    NSMutableArray<NSButton*>* _buttons;
}

- (instancetype)initWithFrame:(NSRect)frame titles:(NSArray<NSString*>*)titles {
    if ((self = [super initWithFrame:frame])) {
        self.wantsLayer = YES;
        self.layer.backgroundColor = CMSurfaceRaisedColor().CGColor;

        _buttons = [NSMutableArray array];
        for (NSString* title in titles) {
            NSButton* button =
                [NSButton buttonWithTitle:title ?: @""
                                   target:self
                                   action:@selector(cmButtonPressed:)];
            button.bordered = NO;
            button.tag = static_cast<NSInteger>(_buttons.count);
            [self addSubview:button];
            [_buttons addObject:button];
        }
        _selectedIndex = 0;
        [self cmLayoutStripForSize:frame.size];
        [self cmUpdateHighlighting];
    }
    return self;
}

- (void)cmLayoutStripForSize:(NSSize)size {
    const CGFloat tabWidth =
        _buttons.count == 0 ? 0.0
                            : size.width / static_cast<CGFloat>(_buttons.count);
    for (NSUInteger index = 0; index < _buttons.count; ++index) {
        _buttons[index].frame = NSMakeRect(
            static_cast<CGFloat>(index) * tabWidth, 0.0, tabWidth, size.height);
    }
}

- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    [self cmLayoutStripForSize:newSize];
}

- (void)cmUpdateHighlighting {
    for (NSUInteger index = 0; index < _buttons.count; ++index) {
        NSButton* button = _buttons[index];
        const BOOL active = static_cast<NSInteger>(index) == _selectedIndex;
        button.wantsLayer = YES;
        button.layer.backgroundColor =
            CMThemeColor(active ? ui::theme::kSurfaceControlHi
                                : ui::theme::kSurfaceControl)
                .CGColor;
        button.layer.cornerRadius = 0.0;
        CALayer* edge = nil;
        for (CALayer* candidate in button.layer.sublayers) {
            if ([candidate.name isEqualToString:@"CMTabTopEdge"]) {
                edge = candidate;
                break;
            }
        }
        if (!edge) {
            edge = [CALayer layer];
            edge.name = @"CMTabTopEdge";
            [button.layer addSublayer:edge];
        }
        edge.frame = CGRectMake(
            0.0, std::max<CGFloat>(0.0, button.bounds.size.height - 2.0),
            button.bounds.size.width, 2.0);
        edge.backgroundColor = CMThemeColor(ui::theme::kAccent).CGColor;
        edge.hidden = !active;
        NSMutableParagraphStyle* style = [[NSMutableParagraphStyle alloc] init];
        style.alignment = NSTextAlignmentCenter;
        button.attributedTitle = [[NSAttributedString alloc]
            initWithString:button.title
                attributes:@{
                    NSForegroundColorAttributeName : active
                        ? CMTextPrimaryColor()
                        : CMThemeColor(ui::theme::kTextTertiary),
                    NSFontAttributeName :
                        CMFont(ui::theme::kFontSizeSmall, NSFontWeightMedium),
                    NSParagraphStyleAttributeName : style,
                }];
    }
}

- (void)cmButtonPressed:(NSButton*)sender {
    [self selectIndex:sender.tag];
}

- (void)selectIndex:(NSInteger)index {
    if (index < 0 || index >= static_cast<NSInteger>(_buttons.count)) return;
    if (index == _selectedIndex) return;
    _selectedIndex = index;
    [self cmUpdateHighlighting];
    if (self.target && self.action &&
        [self.target respondsToSelector:self.action]) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Warc-performSelector-leaks"
        [self.target performSelector:self.action withObject:self];
#pragma clang diagnostic pop
    }
}

@end
