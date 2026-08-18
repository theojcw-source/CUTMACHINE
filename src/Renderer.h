#pragma once

#include "ColorEffects.h"
#include "Document.h"
#include "UiTheme.h"
#include "VideoScopes.h"

#import <AppKit/AppKit.h>

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

struct AVFrame;

struct MetalRect {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    float red = 0.0f;
    float green = 0.0f;
    float blue = 0.0f;
    float alpha = 1.0f;
    float bottom_red = 0.0f;
    float bottom_green = 0.0f;
    float bottom_blue = 0.0f;
    float bottom_alpha = 1.0f;

    MetalRect() = default;
    MetalRect(double itemX, double itemY, double itemWidth, double itemHeight,
              float itemRed, float itemGreen, float itemBlue,
              float itemAlpha = 1.0f)
        : x(itemX),
          y(itemY),
          width(itemWidth),
          height(itemHeight),
          red(itemRed),
          green(itemGreen),
          blue(itemBlue),
          alpha(itemAlpha),
          bottom_red(itemRed),
          bottom_green(itemGreen),
          bottom_blue(itemBlue),
          bottom_alpha(itemAlpha) {}
    MetalRect(double itemX, double itemY, double itemWidth, double itemHeight,
              const ui::theme::Color& topColor,
              const ui::theme::Color& bottomColor)
        : x(itemX),
          y(itemY),
          width(itemWidth),
          height(itemHeight),
          red(topColor.r),
          green(topColor.g),
          blue(topColor.b),
          alpha(topColor.a),
          bottom_red(bottomColor.r),
          bottom_green(bottomColor.g),
          bottom_blue(bottomColor.b),
          bottom_alpha(bottomColor.a) {}
};

enum class MetalFontFace { Sans, Mono };

struct MetalText {
    double x = 0.0;
    double y = 0.0;
    double max_width = 0.0;
    double point_size = 11.0;
    std::string text;
    ui::theme::Color color = ui::theme::kTextPrimary;
    MetalFontFace face = MetalFontFace::Mono;
    bool bold = false;
};

struct MetalIcon {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    std::string name;
    ui::theme::Color color = ui::theme::kTextPrimary;
};

using MetalDrawCommand = std::variant<MetalRect, MetalText, MetalIcon>;

struct TimelineRenderData {
    double video_height = 0.0;
    // Zero fits the image to the viewer. Positive values are native-pixel
    // magnifications: 1.0 = 100%, 2.0 = 200%.
    double video_zoom = 0.0;
    int32_t sequence_width = 1920;
    int32_t sequence_height = 1080;
    ColorManagementSettings color_management;
    // Local monitor policy only. The document and export keep their requested
    // output transform; this switches the drawable to a Rec.709 SDR preview.
    bool display_sdr_preview = false;
    VideoScopeMode video_scope = VideoScopeMode::Off;
    std::vector<int32_t> video_rotation_degrees;
    std::vector<float> video_opacities;
    // Parallel to the frames passed to RenderFrames: the resolved color.*
    // grade stack (F1.3, see ColorEffects.h) for the clip each frame came
    // from. A slot beyond this vector's size, or a default-constructed
    // (count == 0) entry, renders with no grading applied. Renderer.mm is a
    // read-only consumer here -- it never looks at DocumentClip/ClipEffect
    // itself, only this already-resolved, already-float form.
    std::vector<ResolvedColorGrade> video_color_grades;
    // Ordered overlay commands. Text and icons stay in the same display list
    // as rectangles so the playhead and selection strokes preserve their
    // exact z-order instead of being forced into a second overlay pass.
    std::vector<MetalDrawCommand> overlays;
};

class Renderer {
public:
    Renderer();
    ~Renderer();

    bool Initialize(NSView* view);
    void Resize(NSRect bounds);
    // Frames are ordered bottom-to-top and composited in that order. Null
    // entries are timeline holes and reveal lower tracks.
    bool RenderFrames(const std::vector<AVFrame*>& frames,
                      const TimelineRenderData& timeline);

private:
    struct Impl;
    Impl* impl_;
};
