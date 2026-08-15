#import "TransportView.h"

#include <algorithm>

#include "Timecode.h"
#include "TransportBar.h"
#include "UiTheme.h"
#include "UiThemeAppKit.h"

namespace {

CGFloat CGF(double value) {
    return static_cast<CGFloat>(value);
}

}  // namespace

@interface CMTransportView ()
- (void)layoutTransportForSize:(NSSize)size;
@end

@implementation CMTransportView {
    NSButton* _stepBackButton;
    NSButton* _playPauseButton;
    NSButton* _stepForwardButton;
    NSSlider* _scrubSlider;
    NSTextField* _currentTimecodeLabel;
    NSTextField* _totalTimecodeLabel;
}

- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        self.wantsLayer = YES;
        self.layer.backgroundColor = CMSurfacePanelColor().CGColor;

        _stepBackButton = CMMakeStyledButton(@"⏮", nil, nil);
        _stepBackButton.toolTip = @"Image précédente (←)";
        [self addSubview:_stepBackButton];

        _playPauseButton = CMMakeStyledButton(@"▶", nil, nil);
        _playPauseButton.toolTip = @"Lecture / Pause (Espace)";
        [self addSubview:_playPauseButton];

        _stepForwardButton = CMMakeStyledButton(@"⏭", nil, nil);
        _stepForwardButton.toolTip = @"Image suivante (→)";
        [self addSubview:_stepForwardButton];

        _currentTimecodeLabel = [NSTextField labelWithString:@"00:00:00:00"];
        _currentTimecodeLabel.font = [NSFont
            monospacedDigitSystemFontOfSize:CGF(ui::theme::kFontSizeBody)
                                      weight:NSFontWeightMedium];
        _currentTimecodeLabel.textColor = CMTextPrimaryColor();
        _currentTimecodeLabel.alignment = NSTextAlignmentCenter;
        [self addSubview:_currentTimecodeLabel];

        _scrubSlider = CMMakeStyledSlider(0.0, 1.0, nil, nil);
        _scrubSlider.continuous = YES;
        [self addSubview:_scrubSlider];

        _totalTimecodeLabel = [NSTextField labelWithString:@"00:00:00:00"];
        _totalTimecodeLabel.font = [NSFont
            monospacedDigitSystemFontOfSize:CGF(ui::theme::kFontSizeBody)
                                      weight:NSFontWeightRegular];
        _totalTimecodeLabel.textColor = CMTextSecondaryColor();
        _totalTimecodeLabel.alignment = NSTextAlignmentCenter;
        [self addSubview:_totalTimecodeLabel];

        [self layoutTransportForSize:frame.size];
    }
    return self;
}

- (void)layoutTransportForSize:(NSSize)size {
    const CGFloat inset = CGF(ui::theme::kSpaceL);
    const CGFloat gap = CGF(ui::theme::kSpaceS);
    const CGFloat stepButtonSize = CGF(ui::theme::kControlRowHeight + 4.0);
    const CGFloat playButtonWidth = stepButtonSize * 1.3;
    const CGFloat timecodeWidth = 100.0;
    const CGFloat rowHeight = std::min<CGFloat>(size.height, stepButtonSize);
    const CGFloat rowY =
        std::max<CGFloat>(0.0, (size.height - rowHeight) / 2.0);

    CGFloat x = inset;
    _stepBackButton.frame = NSMakeRect(x, rowY, stepButtonSize, rowHeight);
    x += stepButtonSize + gap;
    _playPauseButton.frame = NSMakeRect(x, rowY, playButtonWidth, rowHeight);
    x += playButtonWidth + gap;
    _stepForwardButton.frame = NSMakeRect(x, rowY, stepButtonSize, rowHeight);
    x += stepButtonSize + gap * 2.0;

    _currentTimecodeLabel.frame =
        NSMakeRect(x, rowY, timecodeWidth, rowHeight);
    x += timecodeWidth + gap;

    const CGFloat totalX =
        std::max<CGFloat>(x, size.width - inset - timecodeWidth);
    const CGFloat sliderWidth = std::max<CGFloat>(20.0, totalX - gap - x);
    _scrubSlider.frame = NSMakeRect(x, rowY, sliderWidth, rowHeight);

    _totalTimecodeLabel.frame =
        NSMakeRect(totalX, rowY, timecodeWidth, rowHeight);
}

- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    [self layoutTransportForSize:newSize];
}

- (NSButton*)stepBackButton {
    return _stepBackButton;
}

- (NSButton*)playPauseButton {
    return _playPauseButton;
}

- (NSButton*)stepForwardButton {
    return _stepForwardButton;
}

- (NSSlider*)scrubSlider {
    return _scrubSlider;
}

- (void)setPosition:(RationalTime)position
           duration:(RationalTime)duration
               rate:(MediaRate)rate {
    _currentTimecodeLabel.stringValue = [NSString
        stringWithUTF8String:FormatTimecode(position, rate).c_str()];
    _totalTimecodeLabel.stringValue = [NSString
        stringWithUTF8String:FormatTimecode(duration, rate).c_str()];
    const ScrubBarRange range{duration};
    _scrubSlider.doubleValue = range.TimeToFraction(position);
}

- (void)setPlaying:(BOOL)playing {
    _playPauseButton.title = playing ? @"⏸" : @"▶";
}

@end
