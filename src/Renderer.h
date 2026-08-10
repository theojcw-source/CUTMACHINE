#pragma once

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
    std::vector<int32_t> video_rotation_degrees;
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
