#include "ColorManagement.h"

#include "Document.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

YuvCodeParameters BuildYuvCodeParameters(int bitDepth, bool fullRange) {
    if (bitDepth < 8 || bitDepth > 16)
        throw std::invalid_argument("YUV bit depth must be between 8 and 16");
    const uint32_t maximumCode =
        bitDepth == 16 ? 65535u : ((1u << bitDepth) - 1u);
    YuvCodeParameters result;
    result.sample_scale =
        bitDepth > 8 ? 65535.0f / maximumCode : 1.0f;
    if (fullRange) {
        result.chroma_offset =
            static_cast<float>(1u << (bitDepth - 1)) / maximumCode;
        return result;
    }
    const uint32_t shift = static_cast<uint32_t>(bitDepth - 8);
    result.y_offset = static_cast<float>(16u << shift) / maximumCode;
    result.y_scale = static_cast<float>(maximumCode) / (219u << shift);
    result.chroma_offset =
        static_cast<float>(128u << shift) / maximumCode;
    result.chroma_scale =
        static_cast<float>(maximumCode) / (224u << shift);
    return result;
}

YuvMatrixParameters BuildYuvMatrixParameters(bool bt2020NonConstant) {
    if (bt2020NonConstant)
        return {1.4746f, -0.164553f, -0.571353f, 1.8814f};
    return {};
}

double DecodeSonySLog3(double signal) {
    constexpr double breakpoint = 171.2102946929 / 1023.0;
    if (signal >= breakpoint)
        return std::pow(10.0, (signal * 1023.0 - 420.0) / 261.5) *
                   0.19 -
               0.01;
    return (signal * 1023.0 - 95.0) * 0.01125 /
           (171.2102946929 - 95.0);
}

double EncodeAcesCct(double linearAp1) {
    if (linearAp1 <= 0.0078125)
        return linearAp1 * 10.5402377416545 + 0.0729055341958355;
    return (std::log2(linearAp1) + 9.72) / 17.52;
}

double DecodeAcesCct(double acesCct) {
    constexpr double breakpoint = 0.155251141552511;
    if (acesCct <= breakpoint)
        return (acesCct - 0.0729055341958355) / 10.5402377416545;
    return std::exp2(acesCct * 17.52 - 9.72);
}

double EncodeHlg(double sceneLinear) {
    constexpr double a = 0.17883277;
    constexpr double b = 0.28466892;
    constexpr double c = 0.55991073;
    if (sceneLinear <= 0.0) return 0.0;
    if (sceneLinear <= 1.0 / 12.0) return std::sqrt(3.0 * sceneLinear);
    return a * std::log(12.0 * sceneLinear - b) + c;
}

double DecodeHlg(double signal) {
    constexpr double a = 0.17883277;
    constexpr double b = 0.28466892;
    constexpr double c = 0.55991073;
    if (signal <= 0.5) return signal * signal / 3.0;
    return (std::exp((signal - c) / a) + b) / 12.0;
}

double HlgSceneReflectionScale() { return DecodeHlg(0.75) / 0.9; }

namespace {

RgbColor Multiply(RgbColor value, const double matrix[3][3]) {
    return {
        value.red * matrix[0][0] + value.green * matrix[0][1] +
            value.blue * matrix[0][2],
        value.red * matrix[1][0] + value.green * matrix[1][1] +
            value.blue * matrix[1][2],
        value.red * matrix[2][0] + value.green * matrix[2][1] +
            value.blue * matrix[2][2],
    };
}

double DecodeRec709(double signal) {
    const double positive = std::max(signal, 0.0);
    return positive >= 0.081
               ? std::pow((positive + 0.099) / 1.099, 1.0 / 0.45)
               : positive / 4.5;
}

double EncodeRec709(double linear) {
    linear = std::max(linear, 0.0);
    return std::clamp(linear >= 0.018
                          ? 1.099 * std::pow(linear, 0.45) - 0.099
                          : 4.5 * linear,
                      0.0, 1.0);
}

RgbColor DecodeTransfer(RgbColor value, const std::string& transfer) {
    auto decode = [&](double channel) {
        if (transfer == "linear") return channel;
        if (transfer == "sony_slog3") return DecodeSonySLog3(channel);
        return DecodeRec709(channel);
    };
    return {decode(value.red), decode(value.green), decode(value.blue)};
}

RgbColor SourceToAp1(RgbColor rgb, const std::string& gamut) {
    RgbColor ap0;
    if (gamut == "sony_sgamut3_cine") {
        static constexpr double matrix[3][3] = {
            {0.6387886672, 0.2723514337, 0.0888598991},
            {-0.0039159060, 1.0880732309, -0.0841573249},
            {-0.0299072021, -0.0264325799, 1.0563397820},
        };
        ap0 = Multiply(rgb, matrix);
    } else if (gamut == "sony_sgamut3") {
        static constexpr double matrix[3][3] = {
            {0.7529825954, 0.1433702162, 0.1036471884},
            {0.0217076974, 1.0153188355, -0.0370265329},
            {-0.0094160527, 0.0033704179, 1.0060456349},
        };
        ap0 = Multiply(rgb, matrix);
    } else {
        static constexpr double rec709ToXyz[3][3] = {
            {0.412391, 0.357584, 0.180481},
            {0.212639, 0.715169, 0.072192},
            {0.019331, 0.119195, 0.950532},
        };
        static constexpr double rec2020ToXyz[3][3] = {
            {0.636958, 0.144617, 0.168881},
            {0.262700, 0.677998, 0.059302},
            {0.000000, 0.028073, 1.060985},
        };
        static constexpr double d65ToD60[3][3] = {
            {1.013030, 0.006105, -0.014971},
            {0.007698, 0.998165, -0.005032},
            {-0.002841, 0.004685, 0.924507},
        };
        static constexpr double xyzToAp0[3][3] = {
            {1.049811, 0.000000, -0.000097},
            {-0.495903, 1.373313, 0.098240},
            {0.000000, 0.000000, 0.991252},
        };
        const RgbColor xyz =
            Multiply(rgb, gamut == "rec2020" ? rec2020ToXyz : rec709ToXyz);
        ap0 = Multiply(Multiply(xyz, d65ToD60), xyzToAp0);
    }
    static constexpr double ap0ToAp1[3][3] = {
        {1.4514393161, -0.2365107469, -0.2149285693},
        {-0.0765537734, 1.1762296998, -0.0996759264},
        {0.0083161484, -0.0060324498, 0.9977163014},
    };
    return Multiply(ap0, ap0ToAp1);
}

RgbColor Ap1ToOutput(RgbColor ap1, const std::string& gamut) {
    static constexpr double ap1ToXyzD60[3][3] = {
        {0.6624541811, 0.1340042065, 0.1561876870},
        {0.2722287168, 0.6740817658, 0.0536895174},
        {-0.0055746495, 0.0040607335, 1.0103391003},
    };
    static constexpr double d60ToD65[3][3] = {
        {0.9872240087, -0.0061132286, 0.0159532883},
        {-0.0075983718, 1.0018614847, 0.0053300358},
        {0.0030725771, -0.0050959615, 1.0816806031},
    };
    static constexpr double xyzToRec2020[3][3] = {
        {1.716651, -0.355671, -0.253366},
        {-0.666684, 1.616481, 0.015769},
        {0.017640, -0.042771, 0.942103},
    };
    static constexpr double xyzToRec709[3][3] = {
        {3.240970, -1.537383, -0.498611},
        {-0.969244, 1.875968, 0.041555},
        {0.055630, -0.203977, 1.056972},
    };
    const RgbColor xyz = Multiply(Multiply(ap1, ap1ToXyzD60), d60ToD65);
    return Multiply(xyz, gamut == "rec2020" ? xyzToRec2020 : xyzToRec709);
}

}  // namespace

RgbColor TransformColorForOutput(const ColorManagementSettings& settings,
                                 RgbColor signal) {
    if (!settings.enabled)
        return {std::clamp(signal.red, 0.0, 1.0),
                std::clamp(signal.green, 0.0, 1.0),
                std::clamp(signal.blue, 0.0, 1.0)};
    RgbColor output = Ap1ToOutput(
        SourceToAp1(DecodeTransfer(signal, settings.input_transfer),
                    settings.input_gamut),
        settings.output_gamut);
    if (settings.output_transfer == "hlg") {
        const double scale = HlgSceneReflectionScale();
        return {std::clamp(EncodeHlg(std::max(0.0, output.red) * scale), 0.0,
                           1.0),
                std::clamp(EncodeHlg(std::max(0.0, output.green) * scale), 0.0,
                           1.0),
                std::clamp(EncodeHlg(std::max(0.0, output.blue) * scale), 0.0,
                           1.0)};
    }
    return {EncodeRec709(output.red), EncodeRec709(output.green),
            EncodeRec709(output.blue)};
}
