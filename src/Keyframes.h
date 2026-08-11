#pragma once

#include "RationalTime.h"
#include "Ulid.h"

#include <cstddef>
#include <string>
#include <vector>

enum class KeyframeInterpolation {
    Hold,
    Linear,
};

struct Keyframe {
    Ulid id;
    RationalTime time;
    double value = 0.0;

    // Interpolation applies from this keyframe to the following one.
    KeyframeInterpolation interpolation = KeyframeInterpolation::Linear;
};

enum class KeyframeError {
    None,
    InvalidId,
    InvalidTime,
    InvalidInterpolation,
    NonFiniteValue,
    DuplicateId,
    DuplicateTime,
    NotFound,
    EmptyTrack,
    NumericOverflow,
};

const char* KeyframeErrorName(KeyframeError error);

struct KeyframeResult {
    KeyframeError error = KeyframeError::None;

    explicit operator bool() const { return error == KeyframeError::None; }
};

struct KeyframeEvaluation {
    KeyframeError error = KeyframeError::None;
    double value = 0.0;

    explicit operator bool() const { return error == KeyframeError::None; }
};

// A deterministic, single-numeric-parameter keyframe track.
//
// Time ordering and comparisons are exact. Linear interpolation converts only
// the resulting dimensionless rational fraction to floating point; it never
// converts a RationalTime position to seconds. Values are finite doubles by
// design, because image/audio parameter evaluation is ultimately floating
// point. CanonicalValueString provides a locale-independent, round-trippable
// spelling for future document serialization.
class KeyframeTrack {
public:
    const std::vector<Keyframe>& keyframes() const { return keyframes_; }
    std::size_t size() const { return keyframes_.size(); }
    bool empty() const { return keyframes_.empty(); }

    // IDs are supplied by the caller so a serialized operation can replay the
    // same identity. Two keyframes may not occupy the same exact rational time.
    KeyframeResult Add(Keyframe keyframe);
    KeyframeResult Remove(const Ulid& id);

    // Identity is immutable; an update replaces all mutable fields atomically.
    KeyframeResult Update(const Ulid& id, RationalTime time, double value,
                          KeyframeInterpolation interpolation);

    // Before/after the keyed range, evaluation clamps to the endpoint value.
    KeyframeEvaluation Evaluate(RationalTime time) const;

    static KeyframeResult Validate(const Keyframe& keyframe);
    static std::string CanonicalValueString(double value);

private:
    std::vector<Keyframe> keyframes_;
};
