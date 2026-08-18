#pragma once

// UI-2026-08 -- the Metal surfaces need readable labels and controls without
// turning CoreText or SVG parsing into per-frame work. This atlas owns one R8
// texture containing a bounded Latin glyph set and the build-rasterized
// Lucide icons. Renderer.mm only receives quads and UVs from it.

#include "Renderer.h"

#import <Metal/Metal.h>

#include <string>
#include <vector>

struct MetalAtlasQuad {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    float u0 = 0.0f;
    float v0 = 0.0f;
    float u1 = 0.0f;
    float v1 = 0.0f;
    ui::theme::Color color;
};

class MetalUiAtlas {
public:
    MetalUiAtlas();
    ~MetalUiAtlas();

    bool Initialize(id<MTLDevice> device);
    id<MTLTexture> texture() const;
    void AppendTextQuads(const MetalText& text,
                         std::vector<MetalAtlasQuad>& quads) const;
    bool IconQuad(const MetalIcon& icon, MetalAtlasQuad& quad) const;

private:
    struct Impl;
    Impl* impl_;
};
