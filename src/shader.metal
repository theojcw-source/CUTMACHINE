#include <metal_stdlib>
using namespace metal;

struct VertexOut {
    float4 position [[position]];
    float2 uv;
};

vertex VertexOut vertex_main(uint vertexId [[vertex_id]]) {
    const float2 positions[6] = {
        float2(-1.0, -1.0), float2(1.0, -1.0), float2(1.0, 1.0),
        float2(-1.0, -1.0), float2(1.0, 1.0),  float2(-1.0, 1.0),
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

float3 sampleYUV(float2 uv, texture2d<float> yTexture,
                 texture2d<float> uTexture, texture2d<float> vTexture,
                 sampler planeSampler, float redFromCr, float greenFromCb,
                 float greenFromCr, float blueFromCb, float sampleScale,
                 float yOffset, float yScale, float chromaOffset,
                 float chromaScale) {
    // R16 textures normalize against 65535 although FFmpeg stores 9-12 bit
    // planar samples in the low bits. sampleScale restores code/maxCode.
    const float yCode = yTexture.sample(planeSampler, uv).r * sampleScale;
    const float uCode = uTexture.sample(planeSampler, uv).r * sampleScale;
    const float vCode = vTexture.sample(planeSampler, uv).r * sampleScale;
    const float y = (yCode - yOffset) * yScale;
    const float cb = (uCode - chromaOffset) * chromaScale;
    const float cr = (vCode - chromaOffset) * chromaScale;

    return float3(y + redFromCr * cr, y + greenFromCb * cb + greenFromCr * cr,
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
        const float3 low =
            (signal * 1023.0 - 95.0) * 0.01125 / (171.2102946929 - 95.0);
        return select(low, high, signal >= 171.2102946929 / 1023.0);
    }
    const float3 positive = max(signal, 0.0);
    return select(positive / 4.5, pow((positive + 0.099) / 1.099, 1.0 / 0.45),
                  positive >= 0.081);
}

float3 mulRows(float3 value, float3 row0, float3 row1, float3 row2) {
    return float3(dot(value, row0), dot(value, row1), dot(value, row2));
}

float3 sourceToAP1(float3 rgb, int gamut) {
    float3 ap0;
    if (gamut == 1) {  // ACES reference CSC: S-Gamut3.Cine to ACES2065-1.
        ap0 = mulRows(rgb, float3(0.6387886672, 0.2723514337, 0.0888598991),
                      float3(-0.0039159060, 1.0880732309, -0.0841573249),
                      float3(-0.0299072021, -0.0264325799, 1.0563397820));
    } else if (gamut == 2) {  // ACES reference CSC: S-Gamut3 to ACES2065-1.
        ap0 = mulRows(rgb, float3(0.7529825954, 0.1433702162, 0.1036471884),
                      float3(0.0217076974, 1.0153188355, -0.0370265329),
                      float3(-0.0094160527, 0.0033704179, 1.0060456349));
    } else {
        float3 xyzD65;
        if (gamut == 3) {
            xyzD65 = mulRows(rgb, float3(0.636958, 0.144617, 0.168881),
                             float3(0.262700, 0.677998, 0.059302),
                             float3(0.000000, 0.028073, 1.060985));
        } else {
            xyzD65 = mulRows(rgb, float3(0.412391, 0.357584, 0.180481),
                             float3(0.212639, 0.715169, 0.072192),
                             float3(0.019331, 0.119195, 0.950532));
        }
        const float3 xyzD60 =
            mulRows(xyzD65, float3(1.013030, 0.006105, -0.014971),
                    float3(0.007698, 0.998165, -0.005032),
                    float3(-0.002841, 0.004685, 0.924507));
        ap0 = mulRows(xyzD60, float3(1.049811, 0.000000, -0.000097),
                      float3(-0.495903, 1.373313, 0.098240),
                      float3(0.000000, 0.000000, 0.991252));
    }
    // ACES2065-1 (AP0) to the AP1 primaries shared by ACEScg/ACEScct.
    return mulRows(ap0, float3(1.4514393161, -0.2365107469, -0.2149285693),
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
    const float3 low = (value - 0.0729055341958355) / 10.5402377416545;
    return select(low, high, value > breakpoint);
}

float3 ap1ToOutput(float3 ap1, int outputGamut) {
    const float3 xyzD60 =
        mulRows(ap1, float3(0.6624541811, 0.1340042065, 0.1561876870),
                float3(0.2722287168, 0.6740817658, 0.0536895174),
                float3(-0.0055746495, 0.0040607335, 1.0103391003));
    const float3 xyzD65 =
        mulRows(xyzD60, float3(0.9872240087, -0.0061132286, 0.0159532883),
                float3(-0.0075983718, 1.0018614847, 0.0053300358),
                float3(0.0030725771, -0.0050959615, 1.0816806031));
    if (outputGamut == 1) {
        return mulRows(xyzD65, float3(1.716651, -0.355671, -0.253366),
                       float3(-0.666684, 1.616481, 0.015769),
                       float3(0.017640, -0.042771, 0.942103));
    }
    return mulRows(xyzD65, float3(3.240970, -1.537383, -0.498611),
                   float3(-0.969244, 1.875968, 0.041555),
                   float3(0.055630, -0.203977, 1.056972));
}

float3 mapLinearOutputToDisplay(float3 color, int outputGamut,
                                int outputTransfer) {
    const bool hlg = outputTransfer == 1;
    const float maximum = hlg ? (1.0 / 0.2944028442) : 1.0;
    const float knee = hlg ? 0.9 : 0.72;
    const float3 weights = outputGamut == 1
                               ? float3(0.2627, 0.6780, 0.0593)
                               : float3(0.2126, 0.7152, 0.0722);
    const float luminance = dot(color, weights);
    if (luminance > knee) {
        const float shoulder =
            maximum - (maximum - knee) *
                          exp(-(luminance - knee) / (maximum - knee));
        color *= shoulder / luminance;
    }

    const float mappedLuminance = dot(color, weights);
    const float positiveRoom = max(maximum - mappedLuminance, 1e-9);
    const float negativeRoom = max(mappedLuminance, 1e-9);
    const float3 chroma = color - mappedLuminance;
    const float3 excursion = select(-chroma / negativeRoom,
                                    chroma / positiveRoom, chroma >= 0.0);
    const float ratio = max(excursion.r, max(excursion.g, excursion.b));
    constexpr float chromaKnee = 0.75;
    constexpr float chromaLimit = 0.98;
    if (ratio > chromaKnee) {
        const float compressed =
            chromaKnee + (chromaLimit - chromaKnee) *
                              (1.0 - exp(-(ratio - chromaKnee) /
                                         (chromaLimit - chromaKnee)));
        color = mappedLuminance + chroma * (compressed / ratio);
    }
    return clamp(color, 0.0, maximum);
}

float3 encodeOutput(float3 linear, int transfer) {
    linear = max(linear, 0.0);
    if (transfer == 1) {
        constexpr float a = 0.17883277;
        constexpr float b = 0.28466892;
        constexpr float c = 0.55991073;
        return clamp(select(sqrt(3.0 * linear), a * log(12.0 * linear - b) + c,
                            linear > (1.0 / 12.0)),
                     0.0, 1.0);
    }
    return clamp(select(4.5 * linear, 1.099 * pow(linear, 0.45) - 0.099,
                        linear >= 0.018),
                 0.0, 1.0);
}

// --- Creative color grading (F1.3) -----------------------------------
//
// Interprets a clip's resolved color.* effect stack (Document.h's
// ClipEffect/EffectParamValue, ColorEffects.h's EffectRegistry). Every knob
// below takes a plain float `amount` -- crossing from the document's exact
// num/den fraction to a float happens exactly once, in
// ColorEffects.cc's EffectParamValueToFloat, before this file ever sees the
// value (see that file's comment, and README.md's description of
// TimelineViewport as the analogous time-to-pixel boundary).
//
// GradeEntry/ClipGradeParameters mirror ColorEffects.h's
// ResolvedColorEffect/ResolvedColorGrade and Renderer.mm's local repacking
// struct by hand, the same way PresentationParameters/OutputParameters below
// already mirror their Renderer.mm counterparts: Metal Shading Language
// cannot include a C++ header, so the three definitions are kept in sync by
// convention, not by a shared include.
struct GradeEntry {
    int kind;
    float amount;
};

struct ClipGradeParameters {
    int count;
    // Plain scalars, not int3: unlike the trailing vector padding fields
    // elsewhere in this file (PresentationParameters::padding,
    // SolidParameters::colorPadding), this padding sits *before* more
    // fields. A vector type's larger alignment would shift where `entries`
    // starts relative to Renderer.mm's plain `int32_t padding[3]`, so both
    // sides use an array of scalars to guarantee identical byte offsets.
    int padding[3];
    // Matches ColorEffects.h's kMaxColorEffectsPerClip.
    GradeEntry entries[16];
};

// ACEScct's log branch is (log2(linearAP1)+9.72)/17.52 (see
// linearAP1ToACEScct above), so adding a constant to an ACEScct-encoded
// channel is exactly a *2^(constant*17.52) multiply in linear light. Several
// knobs below use this to stay in log space rather than round-tripping
// through linearAP1ToACEScct/acesCctToLinearAP1 a second time.
constant float kAcesCctLogSlope = 17.52;
// linearAP1ToACEScct(0.18), i.e. 18% middle grey in ACEScct code value.
constant float kAcesCctMiddleGrey = 0.4135884;
constant float3 kLumaWeights = float3(0.2126, 0.7152, 0.0722);

float3 applyExposure(float3 color, float amount) {
    // amount is in stops. Adding amount/17.52 to the log-encoded code is the
    // log-space equivalent of CIExposureAdjust's inputEV, i.e. linear *=
    // 2^amount (see kAcesCctLogSlope above). The affine low-code branch near
    // black is a close, not exact, match; the deviation stays confined to
    // sub-visible shadow detail.
    return color + amount / kAcesCctLogSlope;
}

float3 applyContrast(float3 color, float amount) {
    // Pivots around ACEScct's encoding of 18% middle grey so contrast moves
    // shadows and highlights apart without shifting exposure. amount == 0 is
    // a no-op; +/-1 doubles/removes the slope, mirroring CIColorControls'
    // inputContrast convention (its neutral value 1.0 corresponds to our 0).
    return (color - kAcesCctMiddleGrey) * (1.0 + amount) + kAcesCctMiddleGrey;
}

float3 applySaturation(float3 color, float amount) {
    // Rec.709 luma weights, mirroring CIColorControls' inputSaturation:
    // amount == 0 is a no-op, -1 fully desaturates, +1 doubles the existing
    // saturation.
    const float luma = dot(color, kLumaWeights);
    return mix(float3(luma), color, 1.0 + amount);
}

float3 applyVibrance(float3 color, float amount) {
    // A selective saturation boost that protects already-saturated pixels
    // (and, by extension, skin tones) more than the flat applySaturation
    // move above -- the same intent as CIVibrance, hand-written because
    // there is no single closed-form matrix for it (unlike exposure/
    // contrast/saturation/temperature/tint above and below, which do).
    // existingSaturation is a cheap proxy: the spread between the brightest
    // and dimmest channel, relative to their average.
    const float luma = dot(color, kLumaWeights);
    const float channelMax = max(color.r, max(color.g, color.b));
    const float channelMin = min(color.r, min(color.g, color.b));
    const float average = (color.r + color.g + color.b) / 3.0;
    const float existingSaturation =
        clamp((channelMax - channelMin) / max(abs(average), 1e-4), 0.0, 1.0);
    const float weight = 1.0 - existingSaturation;
    return mix(color, mix(float3(luma), color, 1.0 + amount), weight);
}

float3 applyTemperature(float3 color, float kelvin) {
    // A higher entered Kelvin reads as "the source light was cooler/bluer
    // than neutral daylight", so the compensation warms the image -- the
    // same sign convention as CITemperatureAndTint's inputNeutral and most
    // raw-converter temperature sliders. kStrength keeps the practical
    // +/-3000K range tasteful rather than extreme; it is not a physical
    // constant.
    constexpr float kNeutralKelvin = 6500.0;
    constexpr float kStrength = 0.6;
    const float deltaStops =
        log2(max(kelvin, 1000.0) / kNeutralKelvin) * kStrength;
    color.r += deltaStops / kAcesCctLogSlope;
    color.b -= deltaStops / kAcesCctLogSlope;
    return color;
}

float3 applyTint(float3 color, float amount) {
    // Positive amount shifts toward magenta (down green); negative toward
    // green -- the conventional sign of a tint slider paired with the
    // temperature axis above.
    constexpr float kStrength = 0.5;
    color.g -= (amount * kStrength) / kAcesCctLogSlope;
    return color;
}

float3 applyHighlights(float3 color, float amount) {
    // No single built-in composes inline here the way CIColorControls does
    // for exposure/contrast/saturation, so this is a hand-written luminance
    // mask: positive amount recovers (darkens) the brightest code values,
    // negative boosts them. The mask fades in above the midtones so shadows
    // and midtones are untouched.
    const float luma = dot(color, kLumaWeights);
    const float mask = smoothstep(0.4, 0.9, luma);
    return color - amount * mask * 0.5;
}

float3 applyShadows(float3 color, float amount) {
    // Mirror of applyHighlights: positive amount lifts (brightens) the
    // darkest code values, negative crushes them, fading out above the
    // midtones.
    const float luma = dot(color, kLumaWeights);
    const float mask = 1.0 - smoothstep(0.05, 0.5, luma);
    return color + amount * mask * 0.5;
}

// Applies one clip's resolved grade stack in order. Entries whose kind isn't
// one of the eight above are left untouched -- exactly the registry's
// "unregistered color.* type is a no-op" rule (ColorEffects.h), so a stack
// containing a future curve/wheel/LUT entry still renders every stage this
// build understands instead of failing to render the clip at all.
float3 applyColorGrade(float3 color, constant ClipGradeParameters& grade) {
    for (int i = 0; i < grade.count; ++i) {
        const float amount = grade.entries[i].amount;
        switch (grade.entries[i].kind) {
            case 0:
                color = applyExposure(color, amount);
                break;
            case 1:
                color = applyContrast(color, amount);
                break;
            case 2:
                color = applySaturation(color, amount);
                break;
            case 3:
                color = applyVibrance(color, amount);
                break;
            case 4:
                color = applyTemperature(color, amount);
                break;
            case 5:
                color = applyTint(color, amount);
                break;
            case 6:
                color = applyHighlights(color, amount);
                break;
            case 7:
                color = applyShadows(color, amount);
                break;
            default:
                break;
        }
    }
    return color;
}

bool presentationUV(float2 outputUV, float left, float top, float width,
                    float height, int quarterTurns, thread float2& codedUV) {
    if (outputUV.x < left || outputUV.x > left + width || outputUV.y < top ||
        outputUV.y > top + height)
        return false;
    const float2 displayUV =
        (outputUV - float2(left, top)) / float2(width, height);
    switch (quarterTurns) {
        case 1:
            codedUV = float2(1.0 - displayUV.y, displayUV.x);
            break;
        case 2:
            codedUV = float2(1.0 - displayUV.x, 1.0 - displayUV.y);
            break;
        case 3:
            codedUV = float2(displayUV.y, 1.0 - displayUV.x);
            break;
        default:
            codedUV = displayUV;
            break;
    }
    return true;
}

fragment float4 fragment_working(VertexOut in [[stage_in]],
                                 texture2d<float> yTexture [[texture(0)]],
                                 texture2d<float> uTexture [[texture(1)]],
                                 texture2d<float> vTexture [[texture(2)]],
                                 sampler planeSampler [[sampler(0)]],
                                 constant PresentationParameters& parameters
                                 [[buffer(0)]],
                                 constant ClipGradeParameters& grade
                                 [[buffer(1)]]) {
    float2 codedUV;
    if (!presentationUV(in.uv, parameters.left, parameters.top,
                        parameters.width, parameters.height,
                        parameters.quarterTurns, codedUV))
        return float4(0.0);
    float3 color = sampleYUV(
        codedUV, yTexture, uTexture, vTexture, planeSampler,
        parameters.redFromCr, parameters.greenFromCb, parameters.greenFromCr,
        parameters.blueFromCb, parameters.sampleScale, parameters.yOffset,
        parameters.yScale, parameters.chromaOffset, parameters.chromaScale);
    if (parameters.colorManagementEnabled != 0) {
        color = sourceToAP1(decodeTransfer(color, parameters.inputTransfer),
                            parameters.inputGamut);
        if (parameters.useAcescct != 0) {
            // Creative operations belong between these two calls. Composite
            // storage remains scene-linear AP1 so alpha blending is correct.
            color = linearAP1ToACEScct(color);
            color = applyColorGrade(color, grade);
            color = acesCctToLinearAP1(color);
        } else {
            // Color-managed but not working in ACEScct: grade the
            // scene-linear AP1 signal directly. Every knob above still
            // composes (mix/dot/log2 are meaningful on any float3), just
            // without the log-domain calibration the ACEScct branch gets.
            color = applyColorGrade(color, grade);
        }
    } else {
        // No color management: grade the clamped display-encoded signal
        // directly, same as a non-ACES editor would. Composing here too
        // (rather than only in the ACEScct branch) keeps grading usable
        // with ColorManagementSettings::enabled at its default of false.
        color = clamp(applyColorGrade(color, grade), 0.0, 1.0);
    }
    return float4(color, clamp(parameters.opacity, 0.0, 1.0));
}

struct OutputParameters {
    int colorManagementEnabled;
    int outputGamut;
    int outputTransfer;
    int scopeMode;
    uint drawableWidth;
    uint drawableHeight;
    uint scopeSampleStep;
    uint padding;
};

float3 displaySignal(float3 working, constant OutputParameters& parameters) {
    if (parameters.colorManagementEnabled == 0)
        return clamp(working, 0.0, 1.0);
    float3 output = ap1ToOutput(working, parameters.outputGamut);
    output = mapLinearOutputToDisplay(output, parameters.outputGamut,
                                      parameters.outputTransfer);
    if (parameters.outputTransfer == 1) output *= 0.2944028442;
    return encodeOutput(output, parameters.outputTransfer);
}

constant uint kScopeWaveformWidth = 256;
constant uint kScopeWaveformHeight = 128;
constant uint kScopeParadeWidth = 128;
constant uint kScopeParadeHeight = 128;
constant uint kScopeVectorSize = 256;

kernel void compute_video_scope(
    texture2d<float, access::read> workingTexture [[texture(0)]],
    device atomic_uint* histogram [[buffer(0)]],
    constant OutputParameters& parameters [[buffer(1)]],
    uint2 gid [[thread_position_in_grid]]) {
    const uint2 source = gid * parameters.scopeSampleStep;
    if (source.x >= parameters.drawableWidth ||
        source.y >= parameters.drawableHeight || parameters.scopeMode == 0)
        return;
    const float3 signal = displaySignal(workingTexture.read(source).rgb,
                                        parameters);
    if (parameters.scopeMode == 1) {
        const float3 weights = parameters.outputGamut == 1
                                   ? float3(0.2627, 0.6780, 0.0593)
                                   : float3(0.2126, 0.7152, 0.0722);
        const uint x = min(kScopeWaveformWidth - 1,
                           source.x * kScopeWaveformWidth /
                               max(parameters.drawableWidth, 1u));
        const uint y = min(kScopeWaveformHeight - 1,
                           uint(clamp(dot(signal, weights), 0.0, 1.0) *
                                float(kScopeWaveformHeight - 1)));
        atomic_fetch_add_explicit(
            &histogram[y * kScopeWaveformWidth + x], 1u,
            memory_order_relaxed);
    } else if (parameters.scopeMode == 2) {
        const uint x = min(kScopeParadeWidth - 1,
                           source.x * kScopeParadeWidth /
                               max(parameters.drawableWidth, 1u));
        for (uint channel = 0; channel < 3; ++channel) {
            const uint y = min(kScopeParadeHeight - 1,
                               uint(clamp(signal[channel], 0.0, 1.0) *
                                    float(kScopeParadeHeight - 1)));
            const uint index = channel * kScopeParadeWidth *
                                   kScopeParadeHeight +
                               y * kScopeParadeWidth + x;
            atomic_fetch_add_explicit(&histogram[index], 1u,
                                      memory_order_relaxed);
        }
    } else if (parameters.scopeMode == 3) {
        const float3 weights = parameters.outputGamut == 1
                                   ? float3(0.2627, 0.6780, 0.0593)
                                   : float3(0.2126, 0.7152, 0.0722);
        const float y = dot(signal, weights);
        const float cb = (signal.b - y) / (2.0 * (1.0 - weights.b));
        const float cr = (signal.r - y) / (2.0 * (1.0 - weights.r));
        const uint xBin = min(kScopeVectorSize - 1,
                              uint(clamp(cb + 0.5, 0.0, 1.0) *
                                   float(kScopeVectorSize - 1)));
        const uint yBin = min(kScopeVectorSize - 1,
                              uint(clamp(cr + 0.5, 0.0, 1.0) *
                                   float(kScopeVectorSize - 1)));
        atomic_fetch_add_explicit(
            &histogram[yBin * kScopeVectorSize + xBin], 1u,
            memory_order_relaxed);
    }
}

float scopeGrid(float2 uv, int mode) {
    if (mode == 3) {
        const float radius = length((uv - 0.5) * 2.0);
        const float circle = 1.0 - smoothstep(0.008, 0.018, abs(radius - 0.75));
        const float axes =
            max(1.0 - smoothstep(0.002, 0.006, abs(uv.x - 0.5)),
                1.0 - smoothstep(0.002, 0.006, abs(uv.y - 0.5)));
        return max(circle, axes) * 0.18;
    }
    const float horizontal =
        max(1.0 - smoothstep(0.002, 0.006, abs(uv.y - 0.25)),
            max(1.0 - smoothstep(0.002, 0.006, abs(uv.y - 0.5)),
                1.0 - smoothstep(0.002, 0.006, abs(uv.y - 0.75))));
    return horizontal * 0.16;
}

float4 scopeOverlay(float4 base, float2 pixel,
                    device atomic_uint* histogram,
                    constant OutputParameters& parameters) {
    if (parameters.scopeMode == 0) return base;
    const float panelWidth = min(float(parameters.drawableWidth) * 0.42, 520.0);
    const float panelHeight = min(float(parameters.drawableHeight) * 0.38,
                                  panelWidth * 0.62);
    const float2 origin =
        float2(12.0, float(parameters.drawableHeight) - panelHeight - 12.0);
    if (pixel.x < origin.x || pixel.y < origin.y ||
        pixel.x >= origin.x + panelWidth || pixel.y >= origin.y + panelHeight)
        return base;
    const float2 uv = (pixel - origin) / float2(panelWidth, panelHeight);
    float3 trace = float3(0.0);
    if (parameters.scopeMode == 1) {
        const uint x = min(kScopeWaveformWidth - 1,
                           uint(uv.x * float(kScopeWaveformWidth)));
        const uint y = min(kScopeWaveformHeight - 1,
                           uint((1.0 - uv.y) *
                                float(kScopeWaveformHeight - 1)));
        const uint count = atomic_load_explicit(
            &histogram[y * kScopeWaveformWidth + x], memory_order_relaxed);
        trace = float3(1.0 - exp(-float(count) * 0.18));
    } else if (parameters.scopeMode == 2) {
        const uint channel = min(2u, uint(uv.x * 3.0));
        const float localX = fract(uv.x * 3.0);
        const uint x = min(kScopeParadeWidth - 1,
                           uint(localX * float(kScopeParadeWidth)));
        const uint y = min(kScopeParadeHeight - 1,
                           uint((1.0 - uv.y) *
                                float(kScopeParadeHeight - 1)));
        const uint index = channel * kScopeParadeWidth * kScopeParadeHeight +
                           y * kScopeParadeWidth + x;
        const float intensity =
            1.0 - exp(-float(atomic_load_explicit(&histogram[index],
                                                  memory_order_relaxed)) *
                      0.18);
        trace[channel] = intensity;
    } else {
        const uint x = min(kScopeVectorSize - 1,
                           uint(uv.x * float(kScopeVectorSize)));
        const uint y = min(kScopeVectorSize - 1,
                           uint((1.0 - uv.y) * float(kScopeVectorSize - 1)));
        const uint count = atomic_load_explicit(
            &histogram[y * kScopeVectorSize + x], memory_order_relaxed);
        trace = float3(0.65, 1.0, 0.78) *
                (1.0 - exp(-float(count) * 0.12));
    }
    const float grid = scopeGrid(uv, parameters.scopeMode);
    const float3 panel = max(trace, float3(grid));
    return float4(mix(base.rgb, panel, 0.88), base.a);
}

fragment float4 fragment_output(VertexOut in [[stage_in]],
                                texture2d<float> workingTexture [[texture(0)]],
                                sampler textureSampler [[sampler(0)]],
                                device atomic_uint* histogram [[buffer(1)]],
                                constant OutputParameters& parameters
                                [[buffer(0)]]) {
    float4 working = workingTexture.sample(textureSampler, in.uv);
    const float4 output =
        float4(displaySignal(working.rgb, parameters), working.a);
    return scopeOverlay(output, in.position.xy, histogram, parameters);
}

struct SolidParameters {
    float4 rect;
    float4 topColor;
    float4 bottomColor;
    float2 drawableSize;
    float2 padding;
    int colorManagementEnabled;
    int outputTransfer;
    int2 colorPadding;
};

struct SolidVertexOut {
    float4 position [[position]];
    float4 color;
};

vertex SolidVertexOut vertex_solid(uint vertexId [[vertex_id]],
                                   constant SolidParameters& parameters
                                   [[buffer(0)]]) {
    const float2 corners[6] = {
        float2(0.0, 0.0), float2(1.0, 0.0), float2(1.0, 1.0),
        float2(0.0, 0.0), float2(1.0, 1.0), float2(0.0, 1.0),
    };
    const float2 pixel =
        parameters.rect.xy + corners[vertexId] * parameters.rect.zw;
    const float2 ndc = float2(pixel.x / parameters.drawableSize.x * 2.0 - 1.0,
                              1.0 - pixel.y / parameters.drawableSize.y * 2.0);
    SolidVertexOut out;
    out.position = float4(ndc, 0.0, 1.0);
    out.color =
        mix(parameters.topColor, parameters.bottomColor, corners[vertexId].y);
    return out;
}

float4 uiWorkingColor(float4 color, int colorManagementEnabled,
                      int outputTransfer) {
    float3 rgb = color.rgb;
    if (colorManagementEnabled != 0) {
        // UI colours are authored in sRGB. Put them in the same linear AP1
        // working buffer as video so translucent timeline shapes blend in
        // linear light rather than in encoded HLG.
        rgb =
            select(rgb / 12.92, pow((rgb + 0.055) / 1.055, 2.4), rgb > 0.04045);
        rgb = sourceToAP1(rgb, 0);
        if (outputTransfer == 1) {
            // The output pass maps scene reflection 0.9 to signal HLG 0.75.
            rgb *= 0.9;
        }
    }
    return float4(rgb, color.a);
}

fragment float4 fragment_solid(SolidVertexOut in [[stage_in]],
                               constant SolidParameters& parameters
                               [[buffer(0)]]) {
    return uiWorkingColor(in.color, parameters.colorManagementEnabled,
                          parameters.outputTransfer);
}

struct TextParameters {
    float4 rect;
    float4 uv;
    float4 color;
    float2 drawableSize;
    float2 padding;
    int colorManagementEnabled;
    int outputTransfer;
    int2 colorPadding;
};

struct TextVertexOut {
    float4 position [[position]];
    float2 uv;
};

vertex TextVertexOut vertex_text(uint vertexId [[vertex_id]],
                                 constant TextParameters& parameters
                                 [[buffer(0)]]) {
    const float2 corners[6] = {
        float2(0.0, 0.0), float2(1.0, 0.0), float2(1.0, 1.0),
        float2(0.0, 0.0), float2(1.0, 1.0), float2(0.0, 1.0),
    };
    const float2 pixel =
        parameters.rect.xy + corners[vertexId] * parameters.rect.zw;
    TextVertexOut out;
    out.position =
        float4(pixel.x / parameters.drawableSize.x * 2.0 - 1.0,
               1.0 - pixel.y / parameters.drawableSize.y * 2.0, 0.0, 1.0);
    out.uv = mix(parameters.uv.xy, parameters.uv.zw, corners[vertexId]);
    return out;
}

fragment float4 fragment_text(TextVertexOut in [[stage_in]],
                              constant TextParameters& parameters [[buffer(0)]],
                              texture2d<float> atlas [[texture(0)]],
                              sampler textureSampler [[sampler(0)]]) {
    const float coverage = atlas.sample(textureSampler, in.uv).r;
    float4 color = parameters.color;
    color.a *= coverage;
    return uiWorkingColor(color, parameters.colorManagementEnabled,
                          parameters.outputTransfer);
}
