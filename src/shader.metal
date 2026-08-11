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

float3 sampleYUV(float2 uv,
                   texture2d<float> yTexture,
                   texture2d<float> uTexture,
                   texture2d<float> vTexture,
                   sampler planeSampler,
                   float redFromCr,
                   float greenFromCb,
                   float greenFromCr,
                   float blueFromCb,
                   float sampleScale,
                   float yOffset,
                   float yScale,
                   float chromaOffset,
                   float chromaScale) {
    // R16 textures normalize against 65535 although FFmpeg stores 9-12 bit
    // planar samples in the low bits. sampleScale restores code/maxCode.
    const float yCode = yTexture.sample(planeSampler, uv).r * sampleScale;
    const float uCode = uTexture.sample(planeSampler, uv).r * sampleScale;
    const float vCode = vTexture.sample(planeSampler, uv).r * sampleScale;
    const float y = (yCode - yOffset) * yScale;
    const float cb = (uCode - chromaOffset) * chromaScale;
    const float cr = (vCode - chromaOffset) * chromaScale;

    return float3(y + redFromCr * cr,
                  y + greenFromCb * cb + greenFromCr * cr,
                  y + blueFromCb * cb);
}

struct PresentationParameters {
    float left;
    float top;
    float width;
    float height;
    int quarterTurns;
    float opacity;
    int colorManagementEnabled;
    int inputGamut;
    int inputTransfer;
    int useAcescct;
    float redFromCr;
    float greenFromCb;
    float greenFromCr;
    float blueFromCb;
    float sampleScale;
    float yOffset;
    float yScale;
    float chromaOffset;
    float chromaScale;
    float2 padding;
};

float3 decodeTransfer(float3 signal, int transfer) {
    if (transfer == 2) return signal;
    if (transfer == 1) {
        // Sony's published full-range formula. 420/1023 maps to 18% scene
        // reflection and the linear branch changes at 171.2102946929/1023.
        const float3 high =
            pow(10.0, (signal * 1023.0 - 420.0) / 261.5) * 0.19 - 0.01;
        const float3 low = (signal * 1023.0 - 95.0) * 0.01125 /
                           (171.2102946929 - 95.0);
        return select(low, high, signal >= 171.2102946929 / 1023.0);
    }
    const float3 positive = max(signal, 0.0);
    return select(positive / 4.5,
                  pow((positive + 0.099) / 1.099, 1.0 / 0.45),
                  positive >= 0.081);
}

float3 mulRows(float3 value, float3 row0, float3 row1, float3 row2) {
    return float3(dot(value, row0), dot(value, row1), dot(value, row2));
}

float3 sourceToAP1(float3 rgb, int gamut) {
    float3 ap0;
    if (gamut == 1) { // ACES reference CSC: S-Gamut3.Cine to ACES2065-1.
        ap0 = mulRows(rgb,
                     float3(0.6387886672, 0.2723514337, 0.0888598991),
                     float3(-0.0039159060, 1.0880732309, -0.0841573249),
                     float3(-0.0299072021, -0.0264325799, 1.0563397820));
    } else if (gamut == 2) { // ACES reference CSC: S-Gamut3 to ACES2065-1.
        ap0 = mulRows(rgb,
                     float3(0.7529825954, 0.1433702162, 0.1036471884),
                     float3(0.0217076974, 1.0153188355, -0.0370265329),
                     float3(-0.0094160527, 0.0033704179, 1.0060456349));
    } else {
        float3 xyzD65;
        if (gamut == 3) {
            xyzD65 = mulRows(rgb,
                            float3(0.636958, 0.144617, 0.168881),
                            float3(0.262700, 0.677998, 0.059302),
                            float3(0.000000, 0.028073, 1.060985));
        } else {
            xyzD65 = mulRows(rgb,
                            float3(0.412391, 0.357584, 0.180481),
                            float3(0.212639, 0.715169, 0.072192),
                            float3(0.019331, 0.119195, 0.950532));
        }
        const float3 xyzD60 = mulRows(
            xyzD65,
            float3(1.013030, 0.006105, -0.014971),
            float3(0.007698, 0.998165, -0.005032),
            float3(-0.002841, 0.004685, 0.924507));
        ap0 = mulRows(xyzD60,
                      float3(1.049811, 0.000000, -0.000097),
                      float3(-0.495903, 1.373313, 0.098240),
                      float3(0.000000, 0.000000, 0.991252));
    }
    // ACES2065-1 (AP0) to the AP1 primaries shared by ACEScg/ACEScct.
    return mulRows(ap0,
                   float3(1.4514393161, -0.2365107469, -0.2149285693),
                   float3(-0.0765537734, 1.1762296998, -0.0996759264),
                   float3(0.0083161484, -0.0060324498, 0.9977163014));
}

float3 linearAP1ToACEScct(float3 value) {
    const float3 high = (log2(value) + 9.72) / 17.52;
    const float3 low = value * 10.5402377416545 + 0.0729055341958355;
    return select(low, high, value > 0.0078125);
}

float3 acesCctToLinearAP1(float3 value) {
    constexpr float breakpoint = 0.155251141552511;
    const float3 high = exp2(value * 17.52 - 9.72);
    const float3 low =
        (value - 0.0729055341958355) / 10.5402377416545;
    return select(low, high, value > breakpoint);
}

float3 ap1ToOutput(float3 ap1, int outputGamut) {
    const float3 xyzD60 = mulRows(
        ap1,
        float3(0.6624541811, 0.1340042065, 0.1561876870),
        float3(0.2722287168, 0.6740817658, 0.0536895174),
        float3(-0.0055746495, 0.0040607335, 1.0103391003));
    const float3 xyzD65 = mulRows(
        xyzD60,
        float3(0.9872240087, -0.0061132286, 0.0159532883),
        float3(-0.0075983718, 1.0018614847, 0.0053300358),
        float3(0.0030725771, -0.0050959615, 1.0816806031));
    if (outputGamut == 1) {
        return mulRows(xyzD65,
                       float3(1.716651, -0.355671, -0.253366),
                       float3(-0.666684, 1.616481, 0.015769),
                       float3(0.017640, -0.042771, 0.942103));
    }
    return mulRows(xyzD65,
                   float3(3.240970, -1.537383, -0.498611),
                   float3(-0.969244, 1.875968, 0.041555),
                   float3(0.055630, -0.203977, 1.056972));
}

float3 encodeOutput(float3 linear, int transfer) {
    linear = max(linear, 0.0);
    if (transfer == 1) {
        constexpr float a = 0.17883277;
        constexpr float b = 0.28466892;
        constexpr float c = 0.55991073;
        return clamp(select(sqrt(3.0 * linear),
                            a * log(12.0 * linear - b) + c,
                            linear > (1.0 / 12.0)),
                     0.0, 1.0);
    }
    return clamp(select(4.5 * linear,
                        1.099 * pow(linear, 0.45) - 0.099,
                        linear >= 0.018),
                 0.0, 1.0);
}

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

fragment float4 fragment_working(VertexOut in [[stage_in]],
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
    float3 color = sampleYUV(
        codedUV, yTexture, uTexture, vTexture, planeSampler,
        parameters.redFromCr, parameters.greenFromCb, parameters.greenFromCr,
        parameters.blueFromCb, parameters.sampleScale,
        parameters.yOffset, parameters.yScale, parameters.chromaOffset,
        parameters.chromaScale);
    if (parameters.colorManagementEnabled != 0) {
        color = sourceToAP1(decodeTransfer(color, parameters.inputTransfer),
                            parameters.inputGamut);
        if (parameters.useAcescct != 0) {
            // Creative operations belong between these two calls. Composite
            // storage remains scene-linear AP1 so alpha blending is correct.
            color = acesCctToLinearAP1(linearAP1ToACEScct(color));
        }
    } else {
        color = clamp(color, 0.0, 1.0);
    }
    return float4(color, clamp(parameters.opacity, 0.0, 1.0));
}

struct OutputParameters {
    int colorManagementEnabled;
    int outputGamut;
    int outputTransfer;
    int padding;
};

fragment float4 fragment_output(
    VertexOut in [[stage_in]],
    texture2d<float> workingTexture [[texture(0)]],
    sampler textureSampler [[sampler(0)]],
    constant OutputParameters& parameters [[buffer(0)]]) {
    float4 working = workingTexture.sample(textureSampler, in.uv);
    if (parameters.colorManagementEnabled == 0) return working;
    float3 output = ap1ToOutput(working.rgb, parameters.outputGamut);
    if (parameters.outputTransfer == 1) {
        // BT.2408 places HDR Reference White at HLG signal 0.75. Sony scene
        // reflection 0.9 is therefore scaled to inverseOETF(0.75).
        output *= 0.2944028442;
    }
    return float4(encodeOutput(output, parameters.outputTransfer), working.a);
}

struct SolidParameters {
    float4 rect;
    float4 color;
    float2 drawableSize;
    float2 padding;
    int colorManagementEnabled;
    int outputTransfer;
    int2 colorPadding;
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
    float3 rgb = parameters.color.rgb;
    if (parameters.colorManagementEnabled != 0) {
        // UI colours are authored in sRGB. Put them in the same linear AP1
        // working buffer as video so translucent timeline shapes blend in
        // linear light rather than in encoded HLG.
        rgb = select(rgb / 12.92, pow((rgb + 0.055) / 1.055, 2.4),
                     rgb > 0.04045);
        rgb = sourceToAP1(rgb, 0);
        if (parameters.outputTransfer == 1) {
            // The output pass maps scene reflection 0.9 to signal HLG 0.75.
            rgb *= 0.9;
        }
    }
    return float4(rgb, parameters.color.a);
}
