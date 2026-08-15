#include "Keyframes.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace {

bool TimeLess(const Keyframe& keyframe, const RationalTime& time) {
    return keyframe.time < time;
}

bool TimeLessKeyframe(const Keyframe& left, const Keyframe& right) {
    return left.time < right.time;
}

// Returns the exact (position - left) / (right - left) rational converted to
// long double only after cancellation into a dimensionless fraction.
long double InterpolationFraction(const RationalTime& position,
                                  const RationalTime& left,
                                  const RationalTime& right) {
    const __int128 fromLeft =
        static_cast<__int128>(position.value) * left.rate -
        static_cast<__int128>(left.value) * position.rate;
    const __int128 span = static_cast<__int128>(right.value) * left.rate -
                          static_cast<__int128>(left.value) * right.rate;
    const __int128 numerator = fromLeft * right.rate;
    const __int128 denominator = span * position.rate;
    return static_cast<long double>(numerator) /
           static_cast<long double>(denominator);
}

}  // namespace

const char* KeyframeErrorName(KeyframeError error) {
    switch (error) {
        case KeyframeError::None:
            return "none";
        case KeyframeError::InvalidId:
            return "invalid_id";
        case KeyframeError::InvalidTime:
            return "invalid_time";
        case KeyframeError::InvalidInterpolation:
            return "invalid_interpolation";
        case KeyframeError::NonFiniteValue:
            return "non_finite_value";
        case KeyframeError::DuplicateId:
            return "duplicate_id";
        case KeyframeError::DuplicateTime:
            return "duplicate_time";
        case KeyframeError::NotFound:
            return "not_found";
        case KeyframeError::EmptyTrack:
            return "empty_track";
        case KeyframeError::NumericOverflow:
            return "numeric_overflow";
    }
    return "unknown";
}

KeyframeResult KeyframeTrack::Validate(const Keyframe& keyframe) {
    if (!IsValidUlid(keyframe.id)) {
        return {KeyframeError::InvalidId};
    }
    if (keyframe.time.rate <= 0) {
        return {KeyframeError::InvalidTime};
    }
    if (keyframe.interpolation != KeyframeInterpolation::Hold &&
        keyframe.interpolation != KeyframeInterpolation::Linear) {
        return {KeyframeError::InvalidInterpolation};
    }
    if (!std::isfinite(keyframe.value)) {
        return {KeyframeError::NonFiniteValue};
    }
    return {};
}

KeyframeResult KeyframeTrack::Add(Keyframe keyframe) {
    const KeyframeResult validation = Validate(keyframe);
    if (!validation) {
        return validation;
    }
    if (std::any_of(keyframes_.begin(), keyframes_.end(),
                    [&](const Keyframe& existing) {
                        return existing.id == keyframe.id;
                    })) {
        return {KeyframeError::DuplicateId};
    }

    const auto insertion = std::lower_bound(
        keyframes_.begin(), keyframes_.end(), keyframe, TimeLessKeyframe);
    if (insertion != keyframes_.end() && insertion->time == keyframe.time) {
        return {KeyframeError::DuplicateTime};
    }
    keyframes_.insert(insertion, std::move(keyframe));
    return {};
}

KeyframeResult KeyframeTrack::Remove(const Ulid& id) {
    const auto found = std::find_if(
        keyframes_.begin(), keyframes_.end(),
        [&](const Keyframe& keyframe) { return keyframe.id == id; });
    if (found == keyframes_.end()) {
        return {KeyframeError::NotFound};
    }
    keyframes_.erase(found);
    return {};
}

KeyframeResult KeyframeTrack::Update(const Ulid& id, RationalTime time,
                                     double value,
                                     KeyframeInterpolation interpolation) {
    const auto found = std::find_if(
        keyframes_.begin(), keyframes_.end(),
        [&](const Keyframe& keyframe) { return keyframe.id == id; });
    if (found == keyframes_.end()) {
        return {KeyframeError::NotFound};
    }

    Keyframe replacement{id, time, value, interpolation};
    const KeyframeResult validation = Validate(replacement);
    if (!validation) {
        return validation;
    }
    if (std::any_of(keyframes_.begin(), keyframes_.end(),
                    [&](const Keyframe& keyframe) {
                        return keyframe.id != id && keyframe.time == time;
                    })) {
        return {KeyframeError::DuplicateTime};
    }

    // Validation precedes mutation, making a rejected update atomic.
    keyframes_.erase(found);
    const auto insertion = std::lower_bound(
        keyframes_.begin(), keyframes_.end(), replacement, TimeLessKeyframe);
    keyframes_.insert(insertion, std::move(replacement));
    return {};
}

KeyframeEvaluation KeyframeTrack::Evaluate(RationalTime time) const {
    if (time.rate <= 0) {
        return {KeyframeError::InvalidTime, 0.0};
    }
    if (keyframes_.empty()) {
        return {KeyframeError::EmptyTrack, 0.0};
    }

    const auto right =
        std::lower_bound(keyframes_.begin(), keyframes_.end(), time, TimeLess);
    if (right == keyframes_.begin()) {
        return {KeyframeError::None, right->value};
    }
    if (right == keyframes_.end()) {
        return {KeyframeError::None, keyframes_.back().value};
    }
    if (right->time == time) {
        return {KeyframeError::None, right->value};
    }

    const Keyframe& left = *(right - 1);
    if (left.interpolation == KeyframeInterpolation::Hold) {
        return {KeyframeError::None, left.value};
    }
    const long double fraction =
        InterpolationFraction(time, left.time, right->time);
    const long double interpolated =
        static_cast<long double>(left.value) +
        (static_cast<long double>(right->value) - left.value) * fraction;
    const double value = static_cast<double>(interpolated);
    if (!std::isfinite(value)) {
        return {KeyframeError::NumericOverflow, 0.0};
    }
    return {KeyframeError::None, value};
}

std::string KeyframeTrack::CanonicalValueString(double value) {
    if (!std::isfinite(value)) {
        throw std::invalid_argument("keyframe value must be finite");
    }
    if (value == 0.0) {
        return "0";
    }

    char buffer[64];
    const auto result = std::to_chars(
        buffer, buffer + sizeof(buffer), value, std::chars_format::general,
        std::numeric_limits<double>::max_digits10);
    if (result.ec != std::errc{}) {
        throw std::runtime_error("cannot format canonical keyframe value");
    }
    return std::string(buffer, result.ptr);
}
