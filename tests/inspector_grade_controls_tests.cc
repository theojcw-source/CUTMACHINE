#include "InspectorGradeControls.h"

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

}  // namespace

int main() {
    using ui::inspector::ClampToGradeControlRange;
    using ui::inspector::CurrentGradeControlValue;
    using ui::inspector::FindGradeControlSpec;
    using ui::inspector::GradeControls;
    using ui::inspector::QuantizeGradeControlValue;
    using ui::inspector::WithGradeControlValue;

    // --- Spec table shape -----------------------------------------------
    Check(GradeControls().size() == 8,
          "eight grading knobs, matching ColorEffectRegistry() exactly");
    for (const auto& spec : GradeControls()) {
        Check(FindColorEffectRegistryEntry(spec.type) != nullptr,
              std::string("every GradeControlSpec type resolves in "
                          "ColorEffectRegistry(): ") +
                  spec.type);
        Check(FindGradeControlSpec(spec.type) == &spec,
              std::string("lookup returns the exact spec for ") + spec.type);
        Check(spec.min_value < spec.max_value,
              std::string("range is non-degenerate for ") + spec.type);
        Check(spec.precision_den > 0,
              std::string("precision_den is positive for ") + spec.type);
    }
    Check(FindGradeControlSpec("color.curve") == nullptr &&
              FindGradeControlSpec("") == nullptr,
          "unregistered / future knob types (curves, wheels) miss cleanly "
          "-- F1.3 does not implement them yet, so the Inspector must not "
          "either");

    // --- Clamping ----------------------------------------------------------
    {
        const ui::inspector::GradeControlSpec* contrast =
            FindGradeControlSpec("color.contrast");
        Check(contrast != nullptr, "color.contrast is registered");
        Check(ClampToGradeControlRange(*contrast, 5.0f) == 1.0f,
              "clamps above max_value");
        Check(ClampToGradeControlRange(*contrast, -5.0f) == -1.0f,
              "clamps below min_value");
        Check(ClampToGradeControlRange(*contrast, 0.25f) == 0.25f,
              "in-range values pass through unchanged");
    }

    // --- The exact float -> EffectParamValue boundary ----------------------
    {
        const ui::inspector::GradeControlSpec* exposure =
            FindGradeControlSpec("color.exposure");
        const EffectParamValue value =
            QuantizeGradeControlValue(*exposure, 0.35f);
        Check(value.den == 1000, "exposure quantizes to its declared 1/1000");
        Check(value.num == 350, "0.35 becomes an exact 350/1000");
        Check(EffectParamValueToFloat(value) == 0.35f,
              "reading the committed value back reproduces the exact same "
              "float the slider produced -- no drift across the boundary");
    }
    {
        // Temperature commits whole Kelvin (den == 1), matching
        // ColorEffects.cc's own "kelvin" convention exactly.
        const ui::inspector::GradeControlSpec* temperature =
            FindGradeControlSpec("color.temperature");
        const EffectParamValue value =
            QuantizeGradeControlValue(*temperature, 5600.4f);
        Check(value.den == 1, "temperature commits without a denominator");
        Check(value.num == 5600, "rounds to the nearest whole Kelvin");
    }
    {
        // A value dragged back to a previous position quantizes to the
        // identical fraction both times -- required for undo/redo and for
        // canonical-JSON determinism (PHILOSOPHY.md principle 6): the same
        // slider gesture must produce byte-identical committed state.
        const ui::inspector::GradeControlSpec* saturation =
            FindGradeControlSpec("color.saturation");
        const EffectParamValue first =
            QuantizeGradeControlValue(*saturation, -0.6f);
        const EffectParamValue second =
            QuantizeGradeControlValue(*saturation, -0.6f);
        Check(first.num == second.num && first.den == second.den,
              "quantizing the same float twice is deterministic");
    }
    {
        // An out-of-range slider position (defensive: a caller bug, or a
        // control that briefly overshoots during a drag) still commits an
        // exact, in-range fraction rather than an out-of-range one.
        const ui::inspector::GradeControlSpec* vibrance =
            FindGradeControlSpec("color.vibrance");
        const EffectParamValue value =
            QuantizeGradeControlValue(*vibrance, 50.0f);
        Check(EffectParamValueToFloat(value) == vibrance->max_value,
              "an out-of-range slider value is clamped before quantizing");
    }

    // --- Reading the current value, including registry defaults -----------
    {
        const ui::inspector::GradeControlSpec* contrast =
            FindGradeControlSpec("color.contrast");
        Check(CurrentGradeControlValue({}, *contrast) == 0.0f,
              "no effects at all falls back to the registry default");
        const std::vector<ClipEffect> effects = {
            {GenerateUlid(), "color.contrast", {{"amount", {-250, 1000}}}},
        };
        Check(CurrentGradeControlValue(effects, *contrast) == -0.25f,
              "an existing effect's value crosses the exact->float "
              "boundary the same way ColorEffects.h does");
        const std::vector<ClipEffect> missingParam = {
            {GenerateUlid(), "color.contrast", {}},
        };
        Check(CurrentGradeControlValue(missingParam, *contrast) == 0.0f,
              "an effect present but missing its param falls back to the "
              "registry default, same rule as ResolveColorGrade");
    }

    // --- WithGradeControlValue: the pure effects-vector builder ------------
    {
        const ui::inspector::GradeControlSpec* exposure =
            FindGradeControlSpec("color.exposure");
        const std::vector<ClipEffect> updated =
            WithGradeControlValue({}, *exposure, 0.5f);
        Check(updated.size() == 1 && updated[0].type == "color.exposure",
              "a knob absent from an empty stack is appended");
        Check(EffectParamValueToFloat(updated[0].params.at("amount")) == 0.5f,
              "the appended effect carries the quantized committed value");
    }
    {
        // Updating an existing knob edits in place: same id, same position,
        // every other effect untouched -- SetClipEffectsOperation replaces
        // the whole vector, so preserving identity/order here matters for
        // anything else (future tooling, a second Inspector row) that
        // might key off effect id or stack position.
        const Ulid exposureId = GenerateUlid();
        const Ulid contrastId = GenerateUlid();
        const std::vector<ClipEffect> effects = {
            {exposureId, "color.exposure", {{"amount", {100, 1000}}}},
            {contrastId, "color.contrast", {{"amount", {-200, 1000}}}},
        };
        const ui::inspector::GradeControlSpec* exposure =
            FindGradeControlSpec("color.exposure");
        const std::vector<ClipEffect> updated =
            WithGradeControlValue(effects, *exposure, -0.8f);
        Check(updated.size() == 2, "updating in place does not add a stage");
        Check(updated[0].id == exposureId && updated[1].id == contrastId,
              "identity and stack order are preserved across an update");
        Check(EffectParamValueToFloat(updated[0].params.at("amount")) ==
                  -0.8f,
              "the targeted knob's value changed");
        Check(EffectParamValueToFloat(updated[1].params.at("amount")) ==
                  -0.2f,
              "every other knob's value is untouched");
    }
    {
        // The input vector itself is never mutated -- WithGradeControlValue
        // is a pure function, matching ResolveColorGrade's own contract.
        const std::vector<ClipEffect> original = {
            {GenerateUlid(), "color.tint", {{"amount", {0, 1000}}}},
        };
        const std::vector<ClipEffect> before = original;
        const ui::inspector::GradeControlSpec* tint =
            FindGradeControlSpec("color.tint");
        (void)WithGradeControlValue(original, *tint, 0.9f);
        Check(original.size() == before.size() &&
                  original[0].params.at("amount").num ==
                      before[0].params.at("amount").num,
              "the caller's effects vector is left untouched");
    }

    if (failures == 0) {
        std::cout << "PASS: inspector grade controls\n";
    } else {
        std::cout << "FAIL: " << failures << " check(s) failed\n";
    }
    return failures == 0 ? 0 : 1;
}
