#pragma once

// F2.2 -- ROADMAP.md Inspector. UI-facing metadata and pure conversion
// helpers for the eight F1.3 color.* grading knobs (ColorEffects.h's
// EffectRegistry). Plain C++, no AppKit -- following UiTheme.h's and
// PanelLayout.h's precedent -- so the float<->EffectParamValue boundary a
// slider UI needs (a drag position becoming a committed exact fraction) is
// directly unit-testable (tests/inspector_grade_controls_tests.cc) without
// a macOS toolchain.
//
// This header only *describes* the eight knobs for a slider UI (label,
// range, quantization step) and converts between a slider's float position
// and an exact EffectParamValue. It never mutates a document or clip, and
// never touches EditLog/Operations: the AppKit Inspector view
// (InspectorView.h/.mm, unverified in this sandbox -- see the F2.2 report)
// calls WithGradeControlValue to build the *new* effects vector for a
// clip, then submits it through SetClipEffectsOperation via
// EditLog::Apply -- the same path CLI/MCP already use for this operation
// (PHILOSOPHY.md principle 3, "aucune surface n'est privilégiée").
//
// Curves, wheels and LUTs are deliberately not represented here: F1.3
// (ColorEffects.h) only implements the eight scalar "amount"/"kelvin"
// knobs below, and ROADMAP.md/ColorEffects.h both call curves/wheels/LUTs
// out as unimplemented future work. Building sliders for a ninth knob type
// the engine cannot render yet would put the UI ahead of the engine,
// exactly what PHILOSOPHY.md principle 3 rules out.

#include "ColorEffects.h"
#include "Document.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace ui::inspector {

// One grading knob's UI presentation: the registry entry it reads/writes
// (`type`, matched against ColorEffectRegistry() at call time -- never
// duplicated here) plus the range and quantization a slider needs that the
// registry itself has no reason to carry (ColorEffects.h is a render-time
// concern; range/precision are a UI concern).
struct GradeControlSpec {
    const char* type;   // Matches ColorEffectRegistryEntry::type exactly.
    const char* label;  // French, matches the rest of the app's UI.
    float min_value;
    float max_value;
    // A committed value is quantized to the nearest 1/precision_den before
    // becoming an EffectParamValue, so num/den is always an exact fraction
    // -- never a lossy, unpredictable round-trip of the slider's raw float
    // (PHILOSOPHY.md principle 4: exact conversions, refused/quantized
    // deliberately rather than silently drifting). Temperature already
    // commits whole Kelvin -- see ColorEffects.cc's "kelvin" default and
    // its test ("integral values (kelvin) cross without a denominator") --
    // so its precision is 1, matching that convention exactly.
    int32_t precision_den;
};

// Display order matches ColorEffectKind (ColorEffects.h), which itself
// matches BuildRegistry()'s declaration order in ColorEffects.cc. Ranges
// below are read off shader.metal's own comments for each knob (e.g.
// applyContrast: "+/-1 doubles/removes the slope"; applyTemperature:
// "practical +/-3000K range", read here as a wider 2000-12000K span a
// slider can comfortably cover end to end).
inline const std::vector<GradeControlSpec>& GradeControls() {
    // clang-format off
    static const std::vector<GradeControlSpec> kControls{{
        {"color.exposure",    "Exposition",    -2.0f,     2.0f, 1000},
        {"color.contrast",    "Contraste",     -1.0f,     1.0f, 1000},
        {"color.saturation",  "Saturation",    -1.0f,     1.0f, 1000},
        {"color.vibrance",    "Vibrance",      -1.0f,     1.0f, 1000},
        {"color.temperature", "Température", 2000.0f, 12000.0f,    1},
        {"color.tint",        "Teinte",        -1.0f,     1.0f, 1000},
        {"color.highlights",  "Hautes lum.",   -1.0f,     1.0f, 1000},
        {"color.shadows",     "Ombres",        -1.0f,     1.0f, 1000},
    }};
    // clang-format on
    return kControls;
}

inline const GradeControlSpec* FindGradeControlSpec(const std::string& type) {
    for (const GradeControlSpec& spec : GradeControls())
        if (type == spec.type) return &spec;
    return nullptr;
}

// Clamps to [min_value, max_value]. Every conversion below goes through
// this first so an out-of-range slider position (or a caller passing a
// stray float) never becomes a stored value the knob's own range wouldn't
// already cover.
inline float ClampToGradeControlRange(const GradeControlSpec& spec,
                                      float value) {
    return std::clamp(value, spec.min_value, spec.max_value);
}

// The exact float->EffectParamValue boundary for one knob, mirroring
// ColorEffects.h's EffectParamValueToFloat on the way in: rounds to the
// nearest 1/precision_den so the stored fraction is exact by construction,
// not a truncation of whatever bit pattern the slider produced. Reading it
// straight back through EffectParamValueToFloat reproduces the same
// quantized value on every read -- no further drift on repeated
// load/save, which is what makes this the correct side of the exact<->
// float boundary (PHILOSOPHY.md principle 4) rather than the wrong one.
inline EffectParamValue QuantizeGradeControlValue(const GradeControlSpec& spec,
                                                  float sliderValue) {
    const float clamped = ClampToGradeControlRange(spec, sliderValue);
    const int32_t den = spec.precision_den > 0 ? spec.precision_den : 1;
    const double scaled = static_cast<double>(clamped) * den;
    const int32_t num = static_cast<int32_t>(std::lround(scaled));
    return EffectParamValue{num, den};
}

// ALPHA-2026-08 -- Inspector percentages cross into the canonical model at a
// fixed 1/1000 boundary. Keeping this pure lets the portable UI policy tests
// cover the same value AppKit submits through SetClipOpacityOperation.
inline EffectParamValue QuantizeClipOpacity(float sliderValue) {
    const float clamped = std::clamp(sliderValue, 0.0f, 1.0f);
    return {static_cast<int32_t>(
                std::lround(static_cast<double>(clamped) * 1000.0)),
            1000};
}

inline float CurrentClipOpacity(const EffectParamValue& opacity) {
    return opacity.den > 0 ? static_cast<float>(opacity.num) /
                                 static_cast<float>(opacity.den)
                           : 1.0f;
}

// Reads a knob's current value out of a clip's effect stack, honoring the
// registry's default (ColorEffects.h) when the clip carries no entry for
// this type -- the same rule ResolveColorGrade applies at render time,
// kept consistent here so a freshly opened Inspector's slider position
// always matches what the renderer is already drawing.
inline float CurrentGradeControlValue(const std::vector<ClipEffect>& effects,
                                      const GradeControlSpec& spec) {
    const ColorEffectRegistryEntry* entry =
        FindColorEffectRegistryEntry(spec.type);
    const float fallback = entry ? entry->default_value : 0.0f;
    for (const ClipEffect& effect : effects) {
        if (effect.type != spec.type) continue;
        if (!entry) return fallback;
        const auto param = effect.params.find(entry->param_name);
        if (param == effect.params.end()) return fallback;
        return EffectParamValueToFloat(param->second);
    }
    return fallback;
}

// Pure function: returns a *new* effects vector with `spec`'s knob set to
// `sliderValue` (quantized per QuantizeGradeControlValue), leaving every
// other effect and the stack's order untouched. Updates the first existing
// entry of `spec.type` in place if present, otherwise appends a new
// ClipEffect. Never mutates `effects` and never touches EditLog/Operations
// -- the caller (an AppKit control's action) wraps the result in
// SetClipEffectsOperation and submits it through EditLog::Apply, exactly
// like every other edit in this codebase (PHILOSOPHY.md principle 2/3).
inline std::vector<ClipEffect> WithGradeControlValue(
    const std::vector<ClipEffect>& effects, const GradeControlSpec& spec,
    float sliderValue) {
    const ColorEffectRegistryEntry* entry =
        FindColorEffectRegistryEntry(spec.type);
    const std::string paramName = entry ? entry->param_name : "amount";
    const EffectParamValue quantized =
        QuantizeGradeControlValue(spec, sliderValue);

    std::vector<ClipEffect> result = effects;
    for (ClipEffect& effect : result) {
        if (effect.type != spec.type) continue;
        effect.params[paramName] = quantized;
        return result;
    }
    ClipEffect added;
    added.type = spec.type;
    added.params[paramName] = quantized;
    result.push_back(std::move(added));
    return result;
}

}  // namespace ui::inspector
