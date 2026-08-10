#include <metal_stdlib>
using namespace metal;

struct VertexOut {
    float4 position [[position]];
    float2 uv;
};

vertex VertexOut vertex_main(uint vertexId [[vertex_id]]) {
    const float2 positions[6] = {
        float2(-1.0, -1.0), float2( 1.0, -1.0), float2( 1.0,  1.0),
        float2(-1.0, -1.0), float2( 1.0,  1.0), float2(-1.0,  1.0),
    };
    const float2 uvs[6] = {
        float2(0.0, 1.0), float2(1.0, 1.0), float2(1.0, 0.0),
        float2(0.0, 1.0), float2(1.0, 0.0), float2(0.0, 0.0),
    };
    VertexOut out;
    out.position = float4(positions[vertexId], 0.0, 1.0);
    out.uv = uvs[vertexId];
    return out;
}

float3 sampleBT709(float2 uv,
                   texture2d<float> yTexture,
                   texture2d<float> uTexture,
                   texture2d<float> vTexture,
                   sampler planeSampler) {
    // yuv422p10le stores 10-bit code values in the low bits of uint16_t words.
    constexpr float r16To10 = 65535.0 / 1023.0;
    const float y10 = yTexture.sample(planeSampler, uv).r * r16To10;
    const float u10 = uTexture.sample(planeSampler, uv).r * r16To10;
    const float v10 = vTexture.sample(planeSampler, uv).r * r16To10;

    // BT.709 video range: Y 64..940, Cb/Cr 64..960 with neutral at 512.
    const float y = (y10 - 64.0 / 1023.0) * (1023.0 / 876.0);
    const float cb = (u10 - 512.0 / 1023.0) * (1023.0 / 896.0);
    const float cr = (v10 - 512.0 / 1023.0) * (1023.0 / 896.0);

    const float3 rgb = float3(
        y + 1.792741 * cr,
        y - 0.213249 * cb - 0.532909 * cr,
        y + 2.112402 * cb
    );
    return clamp(rgb, 0.0, 1.0);
}

struct PresentationParameters {
    float left;
    float top;
    float width;
    float height;
    int quarterTurns;
    float opacity;
};

bool presentationUV(float2 outputUV, float left, float top, float width,
                    float height, int quarterTurns, thread float2& codedUV) {
    if (outputUV.x < left || outputUV.x > left + width ||
        outputUV.y < top || outputUV.y > top + height)
        return false;
    const float2 displayUV =
        (outputUV - float2(left, top)) / float2(width, height);
    switch (quarterTurns) {
        case 1: codedUV = float2(1.0 - displayUV.y, displayUV.x); break;
        case 2: codedUV = float2(1.0 - displayUV.x,
                                 1.0 - displayUV.y); break;
        case 3: codedUV = float2(displayUV.y, 1.0 - displayUV.x); break;
        default: codedUV = displayUV; break;
    }
    return true;
}

fragment float4 fragment_main(VertexOut in [[stage_in]],
                              texture2d<float> yTexture [[texture(0)]],
                              texture2d<float> uTexture [[texture(1)]],
                              texture2d<float> vTexture [[texture(2)]],
                              sampler planeSampler [[sampler(0)]],
                              constant PresentationParameters& parameters [[buffer(0)]]) {
    float2 codedUV;
    if (!presentationUV(in.uv, parameters.left, parameters.top,
                        parameters.width, parameters.height,
                        parameters.quarterTurns, codedUV))
        return float4(0.0);
    const float3 color = sampleBT709(codedUV, yTexture, uTexture, vTexture,
                                     planeSampler);
    return float4(color, clamp(parameters.opacity, 0.0, 1.0));
}

struct SolidParameters {
    float4 rect;
    float4 color;
    float2 drawableSize;
    float2 padding;
};

struct SolidVertexOut {
    float4 position [[position]];
};

vertex SolidVertexOut vertex_solid(
    uint vertexId [[vertex_id]],
    constant SolidParameters& parameters [[buffer(0)]]) {
    const float2 corners[6] = {
        float2(0.0, 0.0), float2(1.0, 0.0), float2(1.0, 1.0),
        float2(0.0, 0.0), float2(1.0, 1.0), float2(0.0, 1.0),
    };
    const float2 pixel = parameters.rect.xy +
                         corners[vertexId] * parameters.rect.zw;
    const float2 ndc = float2(pixel.x / parameters.drawableSize.x * 2.0 - 1.0,
                              1.0 - pixel.y / parameters.drawableSize.y * 2.0);
    SolidVertexOut out;
    out.position = float4(ndc, 0.0, 1.0);
    return out;
}

fragment float4 fragment_solid(
    constant SolidParameters& parameters [[buffer(0)]]) {
    return parameters.color;
}
