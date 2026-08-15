#include "ColorEffects.h"

#include <iostream>
#include <string>

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

const Ulid kFirst = "01K90000000000000000000001";
const Ulid kSecond = "01K90000000000000000000002";
const Ulid kThird = "01K90000000000000000000003";

}  // namespace

int main() {
    // --- Registry shape -----------------------------------------------
    Check(ColorEffectRegistry().size() == 8,
          "registry starts with exactly the eight F1.3 knobs");
    for (const ColorEffectRegistryEntry& entry : ColorEffectRegistry()) {
        const std::string type = entry.type;
        Check(type.rfind("color.", 0) == 0,
              "every registered type carries the color. prefix: " + type);
        Check(FindColorEffectRegistryEntry(type) == &entry,
              "lookup returns the exact registered entry for " + type);
    }
    Check(FindColorEffectRegistryEntry("color.exposure") != nullptr &&
              FindColorEffectRegistryEntry("color.contrast") != nullptr &&
              FindColorEffectRegistryEntry("color.saturation") != nullptr &&
              FindColorEffectRegistryEntry("color.vibrance") != nullptr &&
              FindColorEffectRegistryEntry("color.temperature") != nullptr &&
              FindColorEffectRegistryEntry("color.tint") != nullptr &&
              FindColorEffectRegistryEntry("color.highlights") != nullptr &&
              FindColorEffectRegistryEntry("color.shadows") != nullptr,
          "all eight ROADMAP.md F1.3 knob names resolve");
    Check(FindColorEffectRegistryEntry("color.curve") == nullptr &&
              FindColorEffectRegistryEntry("brightness") == nullptr &&
              FindColorEffectRegistryEntry("") == nullptr,
          "types outside the registry (future knobs, garbage) miss cleanly");

    // --- The exact -> float boundary -----------------------------------
    Check(EffectParamValueToFloat({35, 100}) == 0.35f,
          "num/den crosses to float exactly for a clean fraction");
    Check(EffectParamValueToFloat({-5, 100}) == -0.05f,
          "negative numerators cross correctly");
    Check(EffectParamValueToFloat({5600, 1}) == 5600.0f,
          "integral values (kelvin) cross without a denominator");
    Check(EffectParamValueToFloat({1, 0}) == 0.0f,
          "a non-positive denominator resolves to 0 rather than dividing by "
          "it (Document::Validate rejects this shape before it ever "
          "reaches a validated document, this is a defensive fallback)");
    Check(EffectParamValueToFloat({1, -3}) == 0.0f,
          "a negative denominator resolves to 0 the same way");

    // --- ResolveColorGrade: order, defaults, unknown-type skipping ------
    {
        const std::vector<ClipEffect> effects = {
            {kFirst, "color.exposure", {{"amount", {35, 100}}}},
            {kSecond, "color.temperature", {{"kelvin", {5600, 1}}}},
        };
        const ResolvedColorGrade grade = ResolveColorGrade(effects);
        Check(grade.count == 2, "both known effects resolve");
        Check(grade.entries[0].kind ==
                      static_cast<int32_t>(ColorEffectKind::Exposure) &&
                  grade.entries[0].amount == 0.35f,
              "first entry resolves exposure in document order");
        Check(grade.entries[1].kind ==
                      static_cast<int32_t>(ColorEffectKind::Temperature) &&
                  grade.entries[1].amount == 5600.0f,
              "second entry resolves temperature, stack order preserved");
    }
    {
        // A param map missing its expected key falls back to the registry
        // default rather than treating the effect as absent.
        const std::vector<ClipEffect> effects = {
            {kFirst, "color.contrast", {}},
        };
        const ResolvedColorGrade grade = ResolveColorGrade(effects);
        Check(grade.count == 1 && grade.entries[0].amount == 0.0f,
              "a missing 'amount' param falls back to the registry default");
    }
    {
        // Types outside the registry (future curves/wheels/LUT, or a typo)
        // are silent no-ops, not errors, and do not consume a slot.
        const std::vector<ClipEffect> effects = {
            {kFirst, "color.exposure", {{"amount", {10, 100}}}},
            {kSecond, "color.curve", {{"points", {1, 1}}}},
            {kThird, "color.contrast", {{"amount", {20, 100}}}},
        };
        const ResolvedColorGrade grade = ResolveColorGrade(effects);
        Check(grade.count == 2,
              "an unregistered color.* type is skipped, not counted");
        Check(grade.entries[0].kind ==
                      static_cast<int32_t>(ColorEffectKind::Exposure) &&
                  grade.entries[1].kind ==
                      static_cast<int32_t>(ColorEffectKind::Contrast),
              "surrounding known effects keep their relative order across "
              "a skipped unknown one");
    }
    {
        // Depth beyond kMaxColorEffectsPerClip is dropped, not reordered.
        std::vector<ClipEffect> effects;
        for (int32_t i = 0; i < kMaxColorEffectsPerClip + 5; ++i) {
            effects.push_back(
                {GenerateUlid(), "color.exposure", {{"amount", {i, 1}}}});
        }
        const ResolvedColorGrade grade = ResolveColorGrade(effects);
        Check(grade.count == kMaxColorEffectsPerClip,
              "a stack deeper than kMaxColorEffectsPerClip is truncated");
        Check(grade.entries[0].amount == 0.0f &&
                  grade.entries[kMaxColorEffectsPerClip - 1].amount ==
                      static_cast<float>(kMaxColorEffectsPerClip - 1),
              "truncation keeps the leading entries in document order");
    }
    {
        const ResolvedColorGrade grade = ResolveColorGrade({});
        Check(grade.count == 0, "an empty effect stack resolves to no-op");
    }

    if (failures == 0) {
        std::cout << "PASS: color effects registry\n";
    }
    return failures == 0 ? 0 : 1;
}
