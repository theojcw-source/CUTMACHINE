#import "PanelHostView.h"

#include <algorithm>
#include <vector>

#include "UiPreferences.h"
#include "UiTheme.h"
#include "UiThemeAppKit.h"

namespace {

CGFloat CGF(double value) { return static_cast<CGFloat>(value); }

NSAttributedString* CMTabTitle(NSString* title, NSColor* color) {
    NSMutableParagraphStyle* style = [[NSMutableParagraphStyle alloc] init];
    style.alignment = NSTextAlignmentCenter;
    NSDictionary* attributes = @{
        NSForegroundColorAttributeName : color,
        NSFontAttributeName :
            CMFont(ui::theme::kFontSizeSmall, NSFontWeightMedium),
        NSParagraphStyleAttributeName : style,
    };
    return [[NSAttributedString alloc] initWithString:title ?: @""
                                           attributes:attributes];
}

void CMSetTopEdge(NSButton* button, bool visible) {
    button.wantsLayer = YES;
    CALayer* edge = nil;
    for (CALayer* layer in button.layer.sublayers) {
        if ([layer.name isEqualToString:@"CMActiveTopEdge"]) {
            edge = layer;
            break;
        }
    }
    if (!edge) {
        edge = [CALayer layer];
        edge.name = @"CMActiveTopEdge";
        [button.layer addSublayer:edge];
    }
    edge.backgroundColor = CMThemeColor(ui::theme::kAccent).CGColor;
    edge.frame =
        CGRectMake(0.0, std::max<CGFloat>(0.0, button.bounds.size.height - 2.0),
                   button.bounds.size.width, 2.0);
    edge.hidden = !visible;
}

}  // namespace

@interface CMPanelHostView ()
- (NSView*)cmMakePlaceholderForDescriptor:
    (const ui::PanelSlotDescriptor*)descriptor;
- (void)cmLayoutHostForSize:(NSSize)size;
- (void)cmUpdateTabHighlighting;
- (void)cmUpdateContentVisibility;
- (void)cmTabButtonPressed:(NSButton*)sender;
@end

@implementation CMPanelHostView {
    ui::PanelDock _dock;
    std::vector<const ui::PanelSlotDescriptor*> _slots;
    NSView* _tabStrip;
    NSView* _contentContainer;
    NSMutableDictionary<NSNumber*, NSButton*>* _tabButtons;
    NSMutableDictionary<NSNumber*, NSView*>* _slotContentViews;
    ui::PanelSlot _selectedSlot;
}

- (instancetype)initWithFrame:(NSRect)frame dock:(ui::PanelDock)dock {
    if ((self = [super initWithFrame:frame])) {
        _dock = dock;
        _slots = ui::PanelSlotsInDock(dock);
        _tabButtons = [NSMutableDictionary dictionary];
        _slotContentViews = [NSMutableDictionary dictionary];

        self.wantsLayer = YES;
        self.layer.backgroundColor = CMSurfacePanelColor().CGColor;

        _tabStrip = [[NSView alloc] initWithFrame:NSZeroRect];
        _tabStrip.wantsLayer = YES;
        _tabStrip.layer.backgroundColor = CMSurfaceRaisedColor().CGColor;
        [self addSubview:_tabStrip];

        _contentContainer = [[NSView alloc] initWithFrame:NSZeroRect];
        [self addSubview:_contentContainer];

        for (const ui::PanelSlotDescriptor* descriptor : _slots) {
            NSButton* button = [NSButton
                buttonWithTitle:[NSString
                                    stringWithUTF8String:descriptor->title]
                         target:self
                         action:@selector(cmTabButtonPressed:)];
            button.bordered = NO;
            button.tag = static_cast<NSInteger>(descriptor->slot);
            [_tabStrip addSubview:button];
            _tabButtons[@(static_cast<NSInteger>(descriptor->slot))] = button;

            NSView* placeholder =
                [self cmMakePlaceholderForDescriptor:descriptor];
            placeholder.hidden = YES;
            [_contentContainer addSubview:placeholder];
            _slotContentViews[@(static_cast<NSInteger>(descriptor->slot))] =
                placeholder;
        }

        _selectedSlot =
            _slots.empty() ? ui::PanelSlot::Inspector : _slots.front()->slot;
        NSString* remembered = [CMUiPreferences.sharedPreferences
            lastActiveIdentifierForDock:dock];
        if (remembered) {
            const ui::PanelSlotDescriptor* rememberedDescriptor =
                ui::FindPanelSlotByIdentifier(remembered.UTF8String);
            // Only honor a remembered identifier that still names a slot
            // fixed to *this* dock -- a stale or foreign value falls back
            // to the compiled-in default rather than guessing.
            if (rememberedDescriptor && rememberedDescriptor->dock == dock)
                _selectedSlot = rememberedDescriptor->slot;
        }

        [self cmLayoutHostForSize:frame.size];
        [self cmUpdateTabHighlighting];
        [self cmUpdateContentVisibility];
    }
    return self;
}

- (NSView*)cmMakePlaceholderForDescriptor:
    (const ui::PanelSlotDescriptor*)descriptor {
    NSView* placeholder = [[NSView alloc] initWithFrame:NSZeroRect];
    NSTextField* label = [NSTextField
        labelWithString:[NSString stringWithFormat:@"%s — à venir",
                                                   descriptor->title]];
    label.font = CMFont(ui::theme::kFontSizeSmall, NSFontWeightRegular);
    label.textColor = CMThemeColor(ui::theme::kTextTertiary);
    label.alignment = NSTextAlignmentCenter;
    label.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    label.frame = placeholder.bounds;
    [placeholder addSubview:label];
    return placeholder;
}

- (void)cmLayoutHostForSize:(NSSize)size {
    const CGFloat tabStripHeight =
        _slots.size() <= 1 ? 0.0 : CGF(ui::theme::kTabStripHeight);
    _tabStrip.hidden = tabStripHeight == 0.0;
    _tabStrip.frame = NSMakeRect(0.0, size.height - tabStripHeight, size.width,
                                 tabStripHeight);
    _contentContainer.frame =
        NSMakeRect(0.0, 0.0, size.width,
                   std::max<CGFloat>(0.0, size.height - tabStripHeight));

    const CGFloat tabWidth =
        _slots.empty() ? 0.0 : size.width / static_cast<CGFloat>(_slots.size());
    for (size_t index = 0; index < _slots.size(); ++index) {
        NSButton* button =
            _tabButtons[@(static_cast<NSInteger>(_slots[index]->slot))];
        button.frame = NSMakeRect(static_cast<CGFloat>(index) * tabWidth, 0.0,
                                  tabWidth, tabStripHeight);
        CMSetTopEdge(button, _slots[index]->slot == _selectedSlot);
    }
    for (NSView* contentView in _slotContentViews.allValues)
        contentView.frame = _contentContainer.bounds;
}

- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    [self cmLayoutHostForSize:newSize];
}

- (void)cmUpdateTabHighlighting {
    for (const ui::PanelSlotDescriptor* descriptor : _slots) {
        NSButton* button =
            _tabButtons[@(static_cast<NSInteger>(descriptor->slot))];
        const BOOL active = descriptor->slot == _selectedSlot;
        button.wantsLayer = YES;
        button.layer.backgroundColor =
            CMThemeColor(active ? ui::theme::kSurfaceControlHi
                                : ui::theme::kSurfaceControl)
                .CGColor;
        button.layer.cornerRadius = 0.0;
        CMSetTopEdge(button, active);
        button.attributedTitle =
            CMTabTitle([NSString stringWithUTF8String:descriptor->title],
                       active ? CMTextPrimaryColor()
                              : CMThemeColor(ui::theme::kTextTertiary));
    }
}

- (void)cmUpdateContentVisibility {
    for (NSNumber* key in _slotContentViews)
        _slotContentViews[key].hidden =
            key.integerValue != static_cast<NSInteger>(_selectedSlot);
}

- (void)cmTabButtonPressed:(NSButton*)sender {
    [self selectSlot:static_cast<ui::PanelSlot>(sender.tag)];
}

- (void)setContentView:(nullable NSView*)view forSlot:(ui::PanelSlot)slot {
    const ui::PanelSlotDescriptor* descriptor = ui::FindPanelSlot(slot);
    if (!descriptor || descriptor->dock != _dock) return;

    NSNumber* key = @(static_cast<NSInteger>(slot));
    NSView* previous = _slotContentViews[key];
    [previous removeFromSuperview];

    NSView* installed =
        view ? view : [self cmMakePlaceholderForDescriptor:descriptor];
    installed.frame = _contentContainer.bounds;
    installed.hidden = slot != _selectedSlot;
    [_contentContainer addSubview:installed];
    _slotContentViews[key] = installed;
}

- (void)selectSlot:(ui::PanelSlot)slot {
    const ui::PanelSlotDescriptor* descriptor = ui::FindPanelSlot(slot);
    if (!descriptor || descriptor->dock != _dock) return;
    if (slot == _selectedSlot) return;

    _selectedSlot = slot;
    [self cmUpdateTabHighlighting];
    [self cmUpdateContentVisibility];
    [CMUiPreferences.sharedPreferences
        setLastActiveIdentifier:[NSString
                                    stringWithUTF8String:descriptor->identifier]
                        forDock:_dock];
}

- (ui::PanelSlot)selectedSlot {
    return _selectedSlot;
}

@end
