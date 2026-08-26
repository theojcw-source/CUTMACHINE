#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct ColorManagementSettings;

struct RgbColor {
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
};

struct YuvCodeParameters {
    float sample_scale = 1.0f;
    float y_offset = 0.0f;
    float y_scale = 1.0f;
    float chroma_offset = 0.5f;
    float chroma_scale = 1.0f;
};

struct YuvMatrixParameters {
    float red_from_cr = 1.5748f;
    float green_from_cb = -0.187324f;
    float green_from_cr = -0.468124f;
    float blue_from_cb = 1.8556f;
};

// COLOR-2026-08 -- the OCIO library and config are both pinned because their
// output is part of the deterministic render contract. The config is compiled
// into OpenColorIO itself, so rendering never consults $OCIO or the filesystem.
inline constexpr const char* kOpenColorIoConfig =
    "studio-config-v2.2.0_aces-v1.3_ocio-v2.4";
inline constexpr int32_t kOpenColorIoLutEdge = 65;

struct OpenColorIoLut3D {
    int32_t edge = 0;
    // R is the fastest-varying coordinate. RGBA storage maps directly to a
    // Metal RGBA32Float 3D texture; alpha remains one.
    std::vector<float> rgba;
};

struct OpenColorIoLutPair {
    OpenColorIoLut3D input_to_working;
    OpenColorIoLut3D working_to_display;
};

YuvCodeParameters BuildYuvCodeParameters(int bitDepth, bool fullRange);
YuvMatrixParameters BuildYuvMatrixParameters(bool bt2020NonConstant);

double DecodeSonySLog3(double signal);
double EncodeAcesCct(double linearAp1);
double DecodeAcesCct(double acesCct);
double EncodeHlg(double sceneLinear);
double DecodeHlg(double signal);

// Scene-reflection scale that maps Sony's 90% white to BT.2408 HLG
// Reference White at signal 0.75.
double HlgSceneReflectionScale();

// Display rendering stage applied after AP1 has been converted into the
// output primaries. It provides a luminance shoulder and hue-preserving gamut
// compression before the transfer function is encoded. This is deliberately
// not named an ACES Output Transform: CUTMACHINE does not yet ship the full
// ACES 2.0 JMh transform or an OCIO processor.
RgbColor MapLinearOutputToDisplay(const ColorManagementSettings& settings,
                                  RgbColor linearOutput);

// CPU reference for the Metal shader's scene-linear color path. Offline
// export uses this to build a high-precision 3D LUT, while tests use it as the
// parity oracle for known S-Log3/HLG values.
RgbColor TransformColorForOutput(const ColorManagementSettings& settings,
                                 RgbColor signal);

// The project output remains authoritative for export. On-screen editing
// monitors use a display-referred Rec.709 view so an HDR delivery transform
// cannot be interpreted as EDR by the desktop compositor a second time.
ColorManagementSettings ColorManagementForSdrPreview(
    const ColorManagementSettings& settings);

// Resolves the document's readable legacy names through the pinned OCIO
// config and samples the processors into deterministic 65^3 LUTs. The pair is
// used by Metal; the combined .cube is consumed by FFmpeg during export.
bool BuildOpenColorIoLuts(const ColorManagementSettings& settings,
                          OpenColorIoLutPair& output, std::string& error);
bool BuildOpenColorIoCube(const ColorManagementSettings& settings,
                          std::string& output, std::string& error);

// Stable cache key for renderer-owned Metal textures.
std::string OpenColorIoCacheKey(const ColorManagementSettings& settings);
