#import "InspectorView.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "InspectorGradeControls.h"
#include "UiTheme.h"
#include "UiThemeAppKit.h"

#import "UiComponents.h"

namespace {

CGFloat CGF(double value) { return static_cast<CGFloat>(value); }

// Section headers (CMMakeSectionHeader) get a slightly taller slot than a
// CMControlRowView row so the caption-sized text has breathing room above
// and below it.
constexpr CGFloat kSectionHeaderHeight = 20.0;

// A grading slider commits at most this often after the user stops
// dragging -- "reasonable UX: don't fire one operation per pixel of slider
// drag" (F2.2 scope). Every intermediate drag position still updates the
// row's value label immediately, so the number the user sees is never
// stale; only the EditLog::Apply call (and the undo entry it creates) is
// debounced.
constexpr NSTimeInterval kGradeCommitDebounceSeconds = 0.12;

// Strips a filesystem path down to its final component for a compact "Nom"
// row. This app targets macOS only, so '/' is the only separator that
// matters.
NSString* CMBasename(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    const std::string base =
        slash == std::string::npos ? path : path.substr(slash + 1);
    return base.empty() ? @"(sans nom)"
                        : [NSString stringWithUTF8String:base.c_str()];
}

// A small local timecode formatter. This duplicates the handful of lines
// main.mm's own file-local (anonymous-namespace) TimelineTimecode already
// computes -- main.mm does not export it, and F2.2's instructions ask for
// as small a main.mm diff as possible, so a few duplicated lines here beat
// widening main.mm's surface just to share one formatter with a second
// translation unit.
NSString* CMInspectorTimecodeString(const RationalTime& time, MediaRate rate) {
    if (rate.num <= 0 || rate.den <= 0) return @"00:00:00:00";
    const int64_t absoluteFrames =
        std::max<int64_t>(0, time.to_frames(rate.num, rate.den));
    const int64_t nominalFps =
        std::max<int64_t>(1, (rate.num + rate.den - 1) / rate.den);
    const int64_t frames = absoluteFrames % nominalFps;
    const int64_t totalSeconds = absoluteFrames / nominalFps;
    return [NSString
        stringWithFormat:@"%02lld:%02lld:%02lld:%02lld", totalSeconds / 3600,
                         (totalSeconds / 60) % 60, totalSeconds % 60, frames];
}

// Temperature reads as whole Kelvin (matching its GradeControlSpec's
// precision_den == 1); every other knob is a signed +/- amount.
NSString* CMFormatGradeValue(const ui::inspector::GradeControlSpec& spec,
                             float value) {
    if (std::string(spec.type) == "color.temperature")
        return [NSString stringWithFormat:@"%.0f K", value];
    return [NSString stringWithFormat:@"%+.2f", value];
}

}  // namespace

@interface CMInspectorView ()
- (CMControlRowView*)cmAddPropertyRowWithLabel:(NSString*)labelText
                                    valueField:
                                        (NSTextField* __strong*)outValueField;
- (CMControlRowView*)cmAddGradeRowForSpec:
                         (const ui::inspector::GradeControlSpec&)spec
                                    index:(NSInteger)index
                                   slider:(NSSlider* __strong*)outSlider
                               valueLabel:(NSTextField* __strong*)outValueLabel;
- (void)cmLayoutForSize:(NSSize)size;
- (void)cmGradeSliderChanged:(NSSlider*)sender;
- (void)cmScheduleGradeCommitAtIndex:(NSInteger)index value:(float)value;
- (void)cmCommitGradeAtIndex:(NSInteger)index value:(float)value;
- (void)cmInvalidateAllGradeTimers;
@end

@implementation CMInspectorView {
    Ulid _clipId;
    // The selected clip's effects as of the last reload/commit. Every
    // grading commit is built from this snapshot via
    // ui::inspector::WithGradeControlValue (InspectorGradeControls.h),
    // never a direct mutation -- see the file comment in InspectorView.h.
    std::vector<ClipEffect> _effects;

    NSTextField* _placeholderLabel;
    NSView* _body;  // Everything else; hidden while _placeholderLabel shows.

    CMControlRowView* _nameRow;
    CMControlRowView* _trackRow;
    CMControlRowView* _startRow;
    CMControlRowView* _durationRow;
    NSTextField* _nameValueField;
    NSTextField* _trackValueField;
    NSTextField* _startValueField;
    NSTextField* _durationValueField;

    NSMutableArray<NSSlider*>* _gradeSliders;
    NSMutableArray<NSTextField*>* _gradeValueLabels;
    // One slot per GradeControls() entry: an NSTimer while a commit is
    // pending, NSNull otherwise (NSMutableArray cannot hold nil).
    NSMutableArray* _gradeDebounceTimers;

    // Top-to-bottom layout order: (view, slot height). A nil view is a
    // plain spacer (the gap between the properties and grading sections).
    // Built once in -initWithFrame:, replayed by
    // -cmLayoutForSize: on every resize -- same frame-based approach as
    // CMControlRowView/CMPanelChromeView (UiComponents.mm), no Auto
    // Layout anywhere in this project.
    std::vector<std::pair<NSView*, CGFloat>> _layoutItems;
}

- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        _clipId = Ulid{};

        _placeholderLabel =
            [NSTextField labelWithString:@"Aucun clip sélectionné"];
        _placeholderLabel.font =
            CMFont(ui::theme::kFontSizeSmall, NSFontWeightRegular);
        _placeholderLabel.textColor = CMThemeColor(ui::theme::kTextTertiary);
        _placeholderLabel.alignment = NSTextAlignmentCenter;
        _placeholderLabel.lineBreakMode = NSLineBreakByWordWrapping;
        [self addSubview:_placeholderLabel];

        _body = [[NSView alloc] initWithFrame:NSZeroRect];
        [self addSubview:_body];

        NSTextField* propertiesHeader = CMMakeSectionHeader(@"Propriétés");
        [_body addSubview:propertiesHeader];
        _nameRow = [self cmAddPropertyRowWithLabel:@"Nom"
                                        valueField:&_nameValueField];
        _trackRow = [self cmAddPropertyRowWithLabel:@"Piste"
                                         valueField:&_trackValueField];
        _startRow = [self cmAddPropertyRowWithLabel:@"Début"
                                         valueField:&_startValueField];
        _durationRow = [self cmAddPropertyRowWithLabel:@"Durée"
                                            valueField:&_durationValueField];

        NSTextField* gradeHeader =
            CMMakeSectionHeader(@"Correction colorimétrique");
        [_body addSubview:gradeHeader];

        const std::vector<ui::inspector::GradeControlSpec>& controls =
            ui::inspector::GradeControls();
        _gradeSliders = [NSMutableArray arrayWithCapacity:controls.size()];
        _gradeValueLabels = [NSMutableArray arrayWithCapacity:controls.size()];
        _gradeDebounceTimers =
            [NSMutableArray arrayWithCapacity:controls.size()];
        std::vector<CMControlRowView*> gradeRows;
        gradeRows.reserve(controls.size());
        for (size_t index = 0; index < controls.size(); ++index) {
            NSSlider* slider = nil;
            NSTextField* valueLabel = nil;
            CMControlRowView* row =
                [self cmAddGradeRowForSpec:controls[index]
                                     index:static_cast<NSInteger>(index)
                                    slider:&slider
                                valueLabel:&valueLabel];
            [_gradeSliders addObject:slider];
            [_gradeValueLabels addObject:valueLabel];
            [_gradeDebounceTimers addObject:NSNull.null];
            gradeRows.push_back(row);
        }

        _layoutItems.push_back({propertiesHeader, kSectionHeaderHeight});
        _layoutItems.push_back({_nameRow, CGF(ui::theme::kControlRowHeight)});
        _layoutItems.push_back({_trackRow, CGF(ui::theme::kControlRowHeight)});
        _layoutItems.push_back({_startRow, CGF(ui::theme::kControlRowHeight)});
        _layoutItems.push_back(
            {_durationRow, CGF(ui::theme::kControlRowHeight)});
        _layoutItems.push_back({nil, CGF(ui::theme::kSpaceM)});  // spacer
        _layoutItems.push_back({gradeHeader, kSectionHeaderHeight});
        for (CMControlRowView* row : gradeRows)
            _layoutItems.push_back({row, CGF(ui::theme::kControlRowHeight)});

        // Nothing selected yet -- placeholder shows until the first
        // -reloadWithDocument:selectedClipId: call (main.mm makes one
        // right after installing this view).
        _placeholderLabel.hidden = NO;
        _body.hidden = YES;

        [self cmLayoutForSize:frame.size];
    }
    return self;
}

- (void)dealloc {
    [self cmInvalidateAllGradeTimers];
}

- (CMControlRowView*)cmAddPropertyRowWithLabel:(NSString*)labelText
                                    valueField:
                                        (NSTextField* __strong*)outValueField {
    CMControlRowView* row = [[CMControlRowView alloc] initWithFrame:NSZeroRect
                                                          labelText:labelText];
    // Property rows show a free-form string (a filename, a timecode) wider
    // than the fixed 56pt valueLabel column comfortably fits, so they use
    // the wider controlContainer instead and leave valueLabel empty/hidden
    // -- exactly the case UiComponents.h's own doc comment calls out.
    row.valueLabel.hidden = YES;
    NSTextField* value = [NSTextField labelWithString:@""];
    value.font = CMFont(ui::theme::kFontSizeBody, NSFontWeightRegular);
    value.textColor = CMTextSecondaryColor();
    value.lineBreakMode = NSLineBreakByTruncatingTail;
    value.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    value.frame = row.controlContainer.bounds;
    [row.controlContainer addSubview:value];
    *outValueField = value;
    [_body addSubview:row];
    return row;
}

- (CMControlRowView*)
    cmAddGradeRowForSpec:(const ui::inspector::GradeControlSpec&)spec
                   index:(NSInteger)index
                  slider:(NSSlider* __strong*)outSlider
              valueLabel:(NSTextField* __strong*)outValueLabel {
    CMControlRowView* row = [[CMControlRowView alloc]
        initWithFrame:NSZeroRect
            labelText:[NSString stringWithUTF8String:spec.label]];
    NSSlider* slider = CMMakeStyledSlider(spec.min_value, spec.max_value, self,
                                          @selector(cmGradeSliderChanged:));
    slider.tag = index;
    // Continuous: the action fires on every drag tick, not only on mouse
    // up. -cmGradeSliderChanged: uses that to keep the value label live
    // while only *scheduling* (debouncing) the actual committed edit --
    // see kGradeCommitDebounceSeconds above.
    slider.continuous = YES;
    slider.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    slider.frame = row.controlContainer.bounds;
    [row.controlContainer addSubview:slider];
    [_body addSubview:row];
    *outSlider = slider;
    *outValueLabel = row.valueLabel;
    return row;
}

- (void)cmLayoutForSize:(NSSize)size {
    const CGFloat inset = CGF(ui::theme::kSpaceM);
    _placeholderLabel.frame =
        NSMakeRect(inset, 0.0, std::max<CGFloat>(0.0, size.width - 2 * inset),
                   size.height);
    _body.frame = NSMakeRect(0.0, 0.0, size.width, size.height);

    CGFloat y = size.height - inset;
    for (const auto& item : _layoutItems) {
        NSView* view = item.first;
        const CGFloat height = item.second;
        y -= height;
        if (view) {
            if ([view isKindOfClass:[CMControlRowView class]]) {
                // CMControlRowView insets its own subviews (UiComponents.mm),
                // so the row itself spans the full width.
                view.frame = NSMakeRect(0.0, y, size.width, height);
            } else {
                // A bare section header (CMMakeSectionHeader) has no
                // built-in inset of its own.
                view.frame = NSMakeRect(
                    inset, y, std::max<CGFloat>(0.0, size.width - 2 * inset),
                    height);
            }
        }
        y -= CGF(ui::theme::kSpaceXxs);  // breathing room between rows
    }
}

- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    [self cmLayoutForSize:newSize];
}

- (void)reloadWithDocument:(const Document&)document
            selectedClipId:(const Ulid&)clipId {
    // Any grading commit still debounced against the *previous* selection
    // must not land on whatever clip happens to be selected when its timer
    // fires.
    [self cmInvalidateAllGradeTimers];

    const DocumentClip* clip =
        clipId.empty() ? nullptr : document.FindClip(clipId);
    if (!clip) {
        _clipId = Ulid{};
        _effects.clear();
        _placeholderLabel.hidden = NO;
        _body.hidden = YES;
        return;
    }

    _clipId = clipId;
    _effects = clip->effects;
    _placeholderLabel.hidden = YES;
    _body.hidden = NO;

    const DocumentSource* source = document.FindSource(clip->source_id);
    _nameValueField.stringValue =
        source ? CMBasename(source->path)
               : [NSString stringWithUTF8String:clip->id.c_str()];

    const DocumentTrack* track = document.FindTrackForClip(clip->id);
    if (track) {
        NSString* kind = track->kind == "audio" ? @"Audio" : @"Vidéo";
        _trackValueField.stringValue =
            [NSString stringWithFormat:@"%@ · piste %d", kind, track->index];
    } else {
        _trackValueField.stringValue = @"—";
    }

    const MediaRate rate = document.sequence.frame_rate;
    _startValueField.stringValue =
        CMInspectorTimecodeString(clip->timeline_in, rate);
    _durationValueField.stringValue =
        CMInspectorTimecodeString(clip->duration, rate);

    const std::vector<ui::inspector::GradeControlSpec>& controls =
        ui::inspector::GradeControls();
    for (size_t index = 0; index < controls.size(); ++index) {
        const ui::inspector::GradeControlSpec& spec = controls[index];
        const float value =
            ui::inspector::CurrentGradeControlValue(_effects, spec);
        _gradeSliders[index].floatValue = value;
        _gradeValueLabels[index].stringValue = CMFormatGradeValue(spec, value);
    }
}

- (void)cmGradeSliderChanged:(NSSlider*)sender {
    const std::vector<ui::inspector::GradeControlSpec>& controls =
        ui::inspector::GradeControls();
    const NSInteger index = sender.tag;
    if (index < 0 || static_cast<size_t>(index) >= controls.size()) return;

    const ui::inspector::GradeControlSpec& spec =
        controls[static_cast<size_t>(index)];
    const float value = sender.floatValue;
    // The number the user sees updates on every drag tick; only the
    // committed operation is debounced (see below).
    _gradeValueLabels[index].stringValue = CMFormatGradeValue(spec, value);
    [self cmScheduleGradeCommitAtIndex:index value:value];
}

- (void)cmScheduleGradeCommitAtIndex:(NSInteger)index value:(float)value {
    id previous = _gradeDebounceTimers[static_cast<NSUInteger>(index)];
    if ([previous isKindOfClass:[NSTimer class]])
        [(NSTimer*)previous invalidate];

    __weak CMInspectorView* weakSelf = self;
    NSTimer* timer = [NSTimer
        scheduledTimerWithTimeInterval:kGradeCommitDebounceSeconds
                               repeats:NO
                                 block:^(NSTimer* _Nonnull firedTimer) {
                                   (void)firedTimer;
                                   [weakSelf cmCommitGradeAtIndex:index
                                                            value:value];
                                 }];
    _gradeDebounceTimers[static_cast<NSUInteger>(index)] = timer;
}

- (void)cmCommitGradeAtIndex:(NSInteger)index value:(float)value {
    _gradeDebounceTimers[static_cast<NSUInteger>(index)] = NSNull.null;
    if (_clipId.empty() || !self.delegate) return;

    const std::vector<ui::inspector::GradeControlSpec>& controls =
        ui::inspector::GradeControls();
    if (index < 0 || static_cast<size_t>(index) >= controls.size()) return;

    const ui::inspector::GradeControlSpec& spec =
        controls[static_cast<size_t>(index)];
    // Committed from the exact fraction QuantizeGradeControlValue produces
    // -- never the raw slider float -- so what lands in the document is
    // the same num/den boundary crossing ColorEffects.h documents
    // (PHILOSOPHY.md principle 4).
    _effects = ui::inspector::WithGradeControlValue(_effects, spec, value);

    SetClipEffectsOperation operation;
    operation.clip_id = _clipId;
    operation.effects = _effects;
    [self.delegate inspectorView:self
            didCommitClipEffects:std::move(operation)];
}

- (void)cmInvalidateAllGradeTimers {
    for (NSUInteger index = 0; index < _gradeDebounceTimers.count; ++index) {
        id timer = _gradeDebounceTimers[index];
        if ([timer isKindOfClass:[NSTimer class]]) [(NSTimer*)timer invalidate];
        _gradeDebounceTimers[index] = NSNull.null;
    }
}

@end
