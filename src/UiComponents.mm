#import "UiComponents.h"

#include <algorithm>

#include "UiTheme.h"
#include "UiThemeAppKit.h"

namespace {

CGFloat CGF(double value) {
    return static_cast<CGFloat>(value);
}

}  // namespace

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
        _titleLabel.font = CMFont(ui::theme::kFontSizeSection,
                                  NSFontWeightSemibold);
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
    _headerView.frame = NSMakeRect(0.0, size.height - headerHeight,
                                   size.width, headerHeight);
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
        _valueLabel.font =
            [NSFont monospacedDigitSystemFontOfSize:CGF(ui::theme::kFontSizeBody)
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
    _controlContainer.frame =
        NSMakeRect(controlX, 0.0,
                   std::max<CGFloat>(0.0, size.width - controlX - inset),
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
    NSTextField* header =
        [NSTextField labelWithString:title.uppercaseString ?: @""];
    header.font = CMFont(ui::theme::kFontSizeCaption, NSFontWeightSemibold);
    header.textColor = CMColor(ui::theme::kTextTertiary);
    return header;
}

NSButton* CMMakeStyledButton(NSString* title, id target, SEL action) {
    NSButton* button = [NSButton buttonWithTitle:title
                                           target:target
                                           action:action];
    button.bezelStyle = NSBezelStyleRounded;
    button.controlSize = NSControlSizeSmall;
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
            NSButton* button = [NSButton buttonWithTitle:title ?: @""
                                                   target:self
                                                   action:@selector
                                                   (cmButtonPressed:)];
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
        _buttons[index].frame =
            NSMakeRect(static_cast<CGFloat>(index) * tabWidth, 0.0, tabWidth,
                      size.height);
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
            active ? CMColor(ui::theme::kSurfaceControlActive).CGColor
                   : NSColor.clearColor.CGColor;
        NSMutableParagraphStyle* style = [[NSMutableParagraphStyle alloc] init];
        style.alignment = NSTextAlignmentCenter;
        button.attributedTitle = [[NSAttributedString alloc]
            initWithString:button.title
                attributes:@{
                  NSForegroundColorAttributeName :
                      active ? CMAccentBlueColor() : CMTextSecondaryColor(),
                  NSFontAttributeName : CMFont(ui::theme::kFontSizeSmall,
                                              NSFontWeightMedium),
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
