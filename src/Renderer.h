#pragma once

#include "ColorEffects.h"
#include "Document.h"

#import <AppKit/AppKit.h>

#include <cstdint>
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
};

struct TimelineRenderData {
    double video_height = 0.0;
    // Zero fits the image to the viewer. Positive values are native-pixel
    // magnifications: 1.0 = 100%, 2.0 = 200%.
    double video_zoom = 0.0;
    int32_t sequence_width = 1920;
    int32_t sequence_height = 1080;
    ColorManagementSettings color_management;
    std::vector<int32_t> video_rotation_degrees;
    std::vector<float> video_opacities;
    // Parallel to the frames passed to RenderFrames: the resolved color.*
    // grade stack (F1.3, see ColorEffects.h) for the clip each frame came
    // from. A slot beyond this vector's size, or a default-constructed
    // (count == 0) entry, renders with no grading applied. Renderer.mm is a
    // read-only consumer here -- it never looks at DocumentClip/ClipEffect
    // itself, only this already-resolved, already-float form.
    std::vector<ResolvedColorGrade> video_color_grades;
    std::vector<MetalRect> rectangles;
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
