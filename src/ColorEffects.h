#pragma once

#include "Document.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// Render-time interpretation of a clip's color.* effect stack (see
// Document.h's ClipEffect/EffectParamValue and ROADMAP.md ticket F1.3). This
// is the EffectRegistry: a small, fixed table mapping each known
// "color.<name>" type string to a GPU-side dispatch id and its one numeric
// knob. Adding a ninth knob later means adding one row to the registry
// (ColorEffects.cc) and one case to shader.metal's applyColorGrade switch --
// nothing here or in Document.h changes shape, and nothing upstream of this
// file (Operations.h, SetClipEffectsOperation, EditLog) is touched.
//
// This file only reads DocumentClip::effects; it has no write path and
// mutates nothing. Curves, wheels and LUTs (ROADMAP.md F1.3, explicitly
// "future work") are out of scope: an effect whose type is not one of the
// eight rows below is a silent no-op here, not an error, so a document
// carrying one (written by a later ticket, or opened from a newer build)
// still renders every effect this build understands instead of failing to
// render the clip at all. Document::Validate is the only place effect
// *acceptance* is decided (see ClipEffect::Validate in Document.cc, which
// enforces the "color." prefix generically); this registry only decides
// which accepted types this build knows how to draw.

enum class ColorEffectKind : int32_t {
    Exposure = 0,
    Contrast = 1,
    Saturation = 2,
    Vibrance = 3,
    Temperature = 4,
    Tint = 5,
    Highlights = 6,
    Shadows = 7,
};

struct ColorEffectRegistryEntry {
    const char* type;      // Matches ClipEffect::type, e.g. "color.exposure".
    ColorEffectKind kind;  // GPU-side dispatch id (shader.metal switch).
    const char*
        param_name;       // The one numeric knob, read from ClipEffect::params.
    float default_value;  // Used when the named param is absent.
};

// The registry: every color.* type this build knows how to render, in no
// particular order (lookup is by type string, not position).
const std::vector<ColorEffectRegistryEntry>& ColorEffectRegistry();

// Looks up one effect type ("color.exposure", ...). Returns nullptr for a
// type outside the table above -- callers must treat that as a no-op, not
// an error (see the file comment).
const ColorEffectRegistryEntry* FindColorEffectRegistryEntry(
    const std::string& type);

// The single explicit boundary where an exact EffectParamValue{num, den}
// becomes a float, mirroring how README.md documents TimelineViewport as
// the sole time-to-pixel boundary. Every layer upstream of this call --
// Document, EditLog, SetClipEffectsOperation -- keeps the value as an exact
// fraction (PHILOSOPHY.md principle 4); only this render-time resolve step
// (and, past it, the Metal kernel) ever sees a float. Returns 0 for a
// non-positive denominator rather than dividing by it: Document::Validate
// already rejects that shape for any document that reaches the renderer,
// so this is a defensive fallback for callers (tests, future tooling) that
// hand this function an unvalidated value directly.
float EffectParamValueToFloat(const EffectParamValue& value);

// One registry entry after crossing the float boundary above, ready to cross
// into Renderer.mm/shader.metal.
struct ResolvedColorEffect {
    int32_t kind = static_cast<int32_t>(ColorEffectKind::Exposure);
    float amount = 0.0f;
};

// A clip's resolved grade sits in a fixed-size slot because the Metal
// fragment shader consumes it as a plain array in a constant buffer, not a
// dynamically sized allocation. Stacks deeper than this are dropped, in
// document order, rather than reordered or silently merged; sixteen stages
// is generous for a grading stack limited to eight knob families with no
// duplication rule enforced.
inline constexpr int32_t kMaxColorEffectsPerClip = 16;

struct ResolvedColorGrade {
    std::array<ResolvedColorEffect, kMaxColorEffectsPerClip> entries{};
    int32_t count = 0;
};

// Pure, read-only interpretation of one clip's effect stack: looks up each
// entry's type in the registry (skipping anything unregistered, see the
// file comment), converts its one param to a float, and preserves stack
// order. Never mutates `effects` and never touches Operations.h/EditLog --
// this runs at render time only, the same boundary TransformColorForOutput
// in ColorManagement.h occupies for the display/export transform.
ResolvedColorGrade ResolveColorGrade(const std::vector<ClipEffect>& effects);
