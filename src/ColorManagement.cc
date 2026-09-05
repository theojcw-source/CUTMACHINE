#include "ColorManagement.h"

#include "Document.h"

#include <OpenColorIO/OpenColorIO.h>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>

namespace OCIO = OCIO_NAMESPACE;

YuvCodeParameters BuildYuvCodeParameters(int bitDepth, bool fullRange) {
    if (bitDepth < 8 || bitDepth > 16)
        throw std::invalid_argument("YUV bit depth must be between 8 and 16");
    const uint32_t maximumCode =
        bitDepth == 16 ? 65535u : ((1u << bitDepth) - 1u);
    YuvCodeParameters result;
    result.sample_scale = bitDepth > 8 ? 65535.0f / maximumCode : 1.0f;
    if (fullRange) {
        result.chroma_offset =
            static_cast<float>(1u << (bitDepth - 1)) / maximumCode;
        return result;
    }
    const uint32_t shift = static_cast<uint32_t>(bitDepth - 8);
    result.y_offset = static_cast<float>(16u << shift) / maximumCode;
    result.y_scale = static_cast<float>(maximumCode) / (219u << shift);
    result.chroma_offset = static_cast<float>(128u << shift) / maximumCode;
    result.chroma_scale = static_cast<float>(maximumCode) / (224u << shift);
    return result;
}

YuvMatrixParameters BuildYuvMatrixParameters(bool bt2020NonConstant) {
    if (bt2020NonConstant) return {1.4746f, -0.164553f, -0.571353f, 1.8814f};
    return {};
}

double DecodeSonySLog3(double signal) {
    constexpr double breakpoint = 171.2102946929 / 1023.0;
    if (signal >= breakpoint)
        return std::pow(10.0, (signal * 1023.0 - 420.0) / 261.5) * 0.19 - 0.01;
    return (signal * 1023.0 - 95.0) * 0.01125 / (171.2102946929 - 95.0);
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
    return positive >= 0.081 ? std::pow((positive + 0.099) / 1.099, 1.0 / 0.45)
                             : positive / 4.5;
}

double EncodeRec709(double linear) {
    linear = std::max(linear, 0.0);
    return std::clamp(
        linear >= 0.018 ? 1.099 * std::pow(linear, 0.45) - 0.099 : 4.5 * linear,
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

RgbColor MapLinearOutputToDisplay(const ColorManagementSettings& settings,
                                  RgbColor linearOutput) {
    const bool hlg = settings.output_transfer == "hlg";
    const double maximum = hlg ? 1.0 / HlgSceneReflectionScale() : 1.0;
    const double knee = hlg ? 0.9 : 0.72;
    const RgbColor weights = settings.output_gamut == "rec2020"
                                 ? RgbColor{0.2627, 0.6780, 0.0593}
                                 : RgbColor{0.2126, 0.7152, 0.0722};
    const double luminance = linearOutput.red * weights.red +
                             linearOutput.green * weights.green +
                             linearOutput.blue * weights.blue;
    if (luminance > knee) {
        const double shoulder =
            maximum -
            (maximum - knee) * std::exp(-(luminance - knee) / (maximum - knee));
        const double scale = shoulder / luminance;
        linearOutput.red *= scale;
        linearOutput.green *= scale;
        linearOutput.blue *= scale;
    }

    // Compress chroma around luminance instead of clipping channels
    // independently. The ratio describes how far a colour is from neutral as
    // a fraction of the available RGB cube in that direction. A smooth knee
    // begins before the boundary so saturated highlights retain hue detail.
    const double mappedLuminance = linearOutput.red * weights.red +
                                   linearOutput.green * weights.green +
                                   linearOutput.blue * weights.blue;
    const double positiveRoom = std::max(maximum - mappedLuminance, 1e-9);
    const double negativeRoom = std::max(mappedLuminance, 1e-9);
    const auto excursion = [&](double channel) {
        const double chroma = channel - mappedLuminance;
        return chroma >= 0.0 ? chroma / positiveRoom : -chroma / negativeRoom;
    };
    const double ratio =
        std::max({excursion(linearOutput.red), excursion(linearOutput.green),
                  excursion(linearOutput.blue)});
    constexpr double kChromaKnee = 0.75;
    constexpr double kChromaLimit = 0.98;
    if (ratio > kChromaKnee) {
        const double compressed =
            kChromaKnee + (kChromaLimit - kChromaKnee) *
                              (1.0 - std::exp(-(ratio - kChromaKnee) /
                                              (kChromaLimit - kChromaKnee)));
        const double scale = compressed / ratio;
        linearOutput.red =
            mappedLuminance + (linearOutput.red - mappedLuminance) * scale;
        linearOutput.green =
            mappedLuminance + (linearOutput.green - mappedLuminance) * scale;
        linearOutput.blue =
            mappedLuminance + (linearOutput.blue - mappedLuminance) * scale;
    }
    return {std::clamp(linearOutput.red, 0.0, maximum),
            std::clamp(linearOutput.green, 0.0, maximum),
            std::clamp(linearOutput.blue, 0.0, maximum)};
}

RgbColor TransformColorForOutput(const ColorManagementSettings& settings,
                                 RgbColor signal) {
    if (!settings.enabled)
        return {std::clamp(signal.red, 0.0, 1.0),
                std::clamp(signal.green, 0.0, 1.0),
                std::clamp(signal.blue, 0.0, 1.0)};
    RgbColor output =
        Ap1ToOutput(SourceToAp1(DecodeTransfer(signal, settings.input_transfer),
                                settings.input_gamut),
                    settings.output_gamut);
    output = MapLinearOutputToDisplay(settings, output);
    if (settings.output_transfer == "hlg") {
        const double scale = HlgSceneReflectionScale();
        return {
            std::clamp(EncodeHlg(std::max(0.0, output.red) * scale), 0.0, 1.0),
            std::clamp(EncodeHlg(std::max(0.0, output.green) * scale), 0.0,
                       1.0),
            std::clamp(EncodeHlg(std::max(0.0, output.blue) * scale), 0.0,
                       1.0)};
    }
    return {EncodeRec709(output.red), EncodeRec709(output.green),
            EncodeRec709(output.blue)};
}

ColorManagementSettings ColorManagementForSdrPreview(
    const ColorManagementSettings& settings) {
    ColorManagementSettings preview = settings;
    preview.output_gamut = "rec709";
    preview.output_transfer = "rec709";
    return preview;
}

namespace {

const char* OpenColorIoSourceSpace(const ColorManagementSettings& settings) {
    if (settings.input_transfer == "sony_slog3" &&
        settings.input_gamut == "sony_sgamut3_cine")
        return "S-Log3 S-Gamut3.Cine";
    if (settings.input_transfer == "sony_slog3" &&
        settings.input_gamut == "sony_sgamut3")
        return "S-Log3 S-Gamut3";
    if (settings.input_transfer == "rec709" && settings.input_gamut == "rec709")
        return "Camera Rec.709";
    if (settings.input_transfer == "linear" && settings.input_gamut == "rec709")
        return "Linear Rec.709 (sRGB)";
    if (settings.input_transfer == "linear" &&
        settings.input_gamut == "rec2020")
        return "Linear Rec.2020";
    throw std::runtime_error(
        "unsupported OCIO input gamut/transfer combination");
}

const char* OpenColorIoWorkingSpace(const ColorManagementSettings& settings) {
    if (settings.working_gamut == "acescct") return "ACEScg";
    if (settings.working_gamut == "rec709") return "Linear Rec.709 (sRGB)";
    if (settings.working_gamut == "rec2020") return "Linear Rec.2020";
    throw std::runtime_error("unsupported OCIO working gamut");
}

struct OpenColorIoDisplayView {
    const char* display;
    const char* view;
};

OpenColorIoDisplayView ResolveOpenColorIoDisplayView(
    const ColorManagementSettings& settings) {
    if (settings.output_transfer == "hlg") {
        if (settings.output_gamut != "rec2020")
            throw std::runtime_error("HLG output requires Rec.2020");
        return {"Rec.2100-HLG - Display",
                "ACES 1.1 - HDR Video (1000 nits & Rec.2020 lim)"};
    }
    if (settings.output_gamut == "rec2020")
        return {"Rec.1886 Rec.2020 - Display", "ACES 1.0 - SDR Video"};
    return {"Rec.1886 Rec.709 - Display", "ACES 1.0 - SDR Video"};
}

OCIO::ConstConfigRcPtr OpenColorIoConfig() {
    static const OCIO::ConstConfigRcPtr config =
        OCIO::Config::CreateFromBuiltinConfig(kOpenColorIoConfig);
    return config;
}

OCIO::Lut1DTransformRcPtr Rec709DecodeTransform() {
    constexpr unsigned long kLength = 4096;
    OCIO::Lut1DTransformRcPtr transform =
        OCIO::Lut1DTransform::Create(kLength, false);
    transform->setInterpolation(OCIO::INTERP_LINEAR);
    for (unsigned long index = 0; index < kLength; ++index) {
        const float signal = static_cast<float>(index) / (kLength - 1);
        const float linear = static_cast<float>(DecodeRec709(signal));
        transform->setValue(index, linear, linear, linear);
    }
    return transform;
}

bool NeedsRec2020Rec709InputTransform(const ColorManagementSettings& settings) {
    return settings.input_gamut == "rec2020" &&
           settings.input_transfer == "rec709";
}

OCIO::ConstProcessorRcPtr OpenColorIoInputProcessor(
    const ColorManagementSettings& settings) {
    const OCIO::ConstConfigRcPtr config = OpenColorIoConfig();
    if (!NeedsRec2020Rec709InputTransform(settings)) {
        return config->getProcessor(OpenColorIoSourceSpace(settings),
                                    OpenColorIoWorkingSpace(settings));
    }
    // COLOR-2026-08 -- the ACES studio config has linear Rec.2020 but no
    // camera Rec.2020 alias. Preserve the legacy document combination by
    // decoding its Rec.709 transfer before the named gamut conversion.
    OCIO::GroupTransformRcPtr group = OCIO::GroupTransform::Create();
    group->appendTransform(Rec709DecodeTransform());
    OCIO::ColorSpaceTransformRcPtr gamut = OCIO::ColorSpaceTransform::Create();
    gamut->setSrc("Linear Rec.2020");
    gamut->setDst(OpenColorIoWorkingSpace(settings));
    group->appendTransform(gamut);
    return config->getProcessor(group);
}

void SampleOpenColorIoProcessor(const OCIO::ConstProcessorRcPtr& processor,
                                OpenColorIoLut3D& output) {
    output.edge = kOpenColorIoLutEdge;
    const size_t texelCount =
        static_cast<size_t>(output.edge) * output.edge * output.edge;
    output.rgba.resize(texelCount * 4);
    const OCIO::ConstCPUProcessorRcPtr cpu =
        processor->getDefaultCPUProcessor();
    for (int32_t blue = 0; blue < output.edge; ++blue) {
        for (int32_t green = 0; green < output.edge; ++green) {
            for (int32_t red = 0; red < output.edge; ++red) {
                float pixel[4] = {static_cast<float>(red) / (output.edge - 1),
                                  static_cast<float>(green) / (output.edge - 1),
                                  static_cast<float>(blue) / (output.edge - 1),
                                  1.0f};
                cpu->applyRGBA(pixel);
                const size_t offset =
                    (static_cast<size_t>(blue * output.edge * output.edge +
                                         green * output.edge + red)) *
                    4;
                std::copy(pixel, pixel + 4, output.rgba.begin() + offset);
            }
        }
    }
}

OCIO::ConstProcessorRcPtr OpenColorIoOutputProcessor(
    const ColorManagementSettings& settings) {
    const OpenColorIoDisplayView displayView =
        ResolveOpenColorIoDisplayView(settings);
    const char* lutInputSpace = settings.working_gamut == "acescct"
                                    ? "ACEScct"
                                    : OpenColorIoWorkingSpace(settings);
    return OpenColorIoConfig()->getProcessor(lutInputSpace, displayView.display,
                                             displayView.view,
                                             OCIO::TRANSFORM_DIR_FORWARD);
}

OCIO::ConstProcessorRcPtr OpenColorIoCombinedProcessor(
    const ColorManagementSettings& settings) {
    const OpenColorIoDisplayView displayView =
        ResolveOpenColorIoDisplayView(settings);
    if (NeedsRec2020Rec709InputTransform(settings)) {
        OCIO::GroupTransformRcPtr group = OCIO::GroupTransform::Create();
        group->appendTransform(Rec709DecodeTransform());
        OCIO::DisplayViewTransformRcPtr display =
            OCIO::DisplayViewTransform::Create();
        display->setSrc("Linear Rec.2020");
        display->setDisplay(displayView.display);
        display->setView(displayView.view);
        group->appendTransform(display);
        return OpenColorIoConfig()->getProcessor(group);
    }
    return OpenColorIoConfig()->getProcessor(
        OpenColorIoSourceSpace(settings), displayView.display, displayView.view,
        OCIO::TRANSFORM_DIR_FORWARD);
}

}  // namespace

bool BuildOpenColorIoLuts(const ColorManagementSettings& settings,
                          OpenColorIoLutPair& output, std::string& error) {
    output = {};
    error.clear();
    try {
        SampleOpenColorIoProcessor(OpenColorIoInputProcessor(settings),
                                   output.input_to_working);
        SampleOpenColorIoProcessor(OpenColorIoOutputProcessor(settings),
                                   output.working_to_display);
        return true;
    } catch (const OCIO::Exception& exception) {
        error = exception.what();
    } catch (const std::exception& exception) {
        error = exception.what();
    }
    output = {};
    return false;
}

bool BuildOpenColorIoCube(const ColorManagementSettings& settings,
                          std::string& output, std::string& error) {
    output.clear();
    error.clear();
    try {
        OpenColorIoLut3D lut;
        SampleOpenColorIoProcessor(OpenColorIoCombinedProcessor(settings), lut);
        std::ostringstream stream;
        stream.imbue(std::locale::classic());
        stream << "TITLE \"CUTMACHINE OpenColorIO display transform\"\n"
               << "LUT_3D_SIZE " << lut.edge << '\n'
               << "DOMAIN_MIN 0.0 0.0 0.0\n"
               << "DOMAIN_MAX 1.0 1.0 1.0\n"
               << std::fixed << std::setprecision(10);
        for (size_t offset = 0; offset < lut.rgba.size(); offset += 4)
            stream << lut.rgba[offset] << ' ' << lut.rgba[offset + 1] << ' '
                   << lut.rgba[offset + 2] << '\n';
        output = stream.str();
        return true;
    } catch (const OCIO::Exception& exception) {
        error = exception.what();
    } catch (const std::exception& exception) {
        error = exception.what();
    }
    return false;
}

std::string OpenColorIoCacheKey(const ColorManagementSettings& settings) {
    std::ostringstream output;
    output << kOpenColorIoConfig << '|' << settings.input_gamut << '|'
           << settings.input_transfer << '|' << settings.working_gamut << '|'
           << settings.output_gamut << '|' << settings.output_transfer;
    return output.str();
}
