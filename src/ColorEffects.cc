#include "ColorEffects.h"

namespace {

const std::vector<ColorEffectRegistryEntry>& BuildRegistry() {
    // clang-format off
    static const std::vector<ColorEffectRegistryEntry> registry = {
        // type                kind                              param      default
        {"color.exposure",     ColorEffectKind::Exposure,      "amount",  0.0f},
        {"color.contrast",     ColorEffectKind::Contrast,      "amount",  0.0f},
        {"color.saturation",   ColorEffectKind::Saturation,    "amount",  0.0f},
        {"color.vibrance",     ColorEffectKind::Vibrance,      "amount",  0.0f},
        // Neutral daylight; see shader.metal's applyTemperature for the sign
        // convention (higher Kelvin reads as "the source light was cooler",
        // so the compensation warms the image -- same convention as most
        // camera/raw-converter temperature sliders, including
        // CITemperatureAndTint's inputNeutral).
        {"color.temperature",  ColorEffectKind::Temperature,   "kelvin",  6500.0f},
        {"color.tint",         ColorEffectKind::Tint,          "amount",  0.0f},
        {"color.highlights",   ColorEffectKind::Highlights,    "amount",  0.0f},
        {"color.shadows",      ColorEffectKind::Shadows,       "amount",  0.0f},
    };
    // clang-format on
    return registry;
}

}  // namespace

const std::vector<ColorEffectRegistryEntry>& ColorEffectRegistry() {
    return BuildRegistry();
}

const ColorEffectRegistryEntry* FindColorEffectRegistryEntry(
    const std::string& type) {
    for (const ColorEffectRegistryEntry& entry : ColorEffectRegistry()) {
        if (type == entry.type) return &entry;
    }
    return nullptr;
}

float EffectParamValueToFloat(const EffectParamValue& value) {
    if (value.den <= 0) return 0.0f;
    return static_cast<float>(value.num) / static_cast<float>(value.den);
}

ResolvedColorGrade ResolveColorGrade(const std::vector<ClipEffect>& effects) {
    ResolvedColorGrade grade;
    for (const ClipEffect& effect : effects) {
        if (grade.count >= kMaxColorEffectsPerClip) break;
        const ColorEffectRegistryEntry* entry =
            FindColorEffectRegistryEntry(effect.type);
        if (!entry) continue;  // Unregistered "color.*" type: silent no-op.
        float amount = entry->default_value;
        const auto param = effect.params.find(entry->param_name);
        if (param != effect.params.end())
            amount = EffectParamValueToFloat(param->second);
        grade.entries[static_cast<size_t>(grade.count)] = {
            static_cast<int32_t>(entry->kind), amount};
        ++grade.count;
    }
    return grade;
}
