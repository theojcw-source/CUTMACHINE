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

struct CompositeParameters {
    float secondOpacity;
    uint hasSecondFrame;
};

fragment float4 fragment_main(VertexOut in [[stage_in]],
                              texture2d<float> yFirst [[texture(0)]],
                              texture2d<float> uFirst [[texture(1)]],
                              texture2d<float> vFirst [[texture(2)]],
                              texture2d<float> ySecond [[texture(3)]],
                              texture2d<float> uSecond [[texture(4)]],
                              texture2d<float> vSecond [[texture(5)]],
                              sampler planeSampler [[sampler(0)]],
                              constant CompositeParameters& parameters [[buffer(0)]]) {
    const float3 first = sampleBT709(in.uv, yFirst, uFirst, vFirst, planeSampler);
    if (parameters.hasSecondFrame == 0) {
        return float4(first, 1.0);
    }
    const float3 second = sampleBT709(in.uv, ySecond, uSecond, vSecond,
                                      planeSampler);
    return float4(mix(first, second, clamp(parameters.secondOpacity, 0.0, 1.0)),
                  1.0);
}
