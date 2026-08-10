#pragma once

#include <cstdint>
#include <limits>
#include <stdexcept>

struct RationalTime {
    int64_t value = 0;
    int32_t rate = 1;

    constexpr RationalTime() = default;
    constexpr RationalTime(int64_t valueIn, int32_t rateIn)
        : value(valueIn), rate(rateIn) {}

    RationalTime add(const RationalTime& other) const {
        CheckRate(rate);
        CheckRate(other.rate);
        const int64_t commonRate = Lcm(rate, other.rate);
        if (commonRate > std::numeric_limits<int32_t>::max()) {
            throw std::overflow_error(
                "RationalTime common rate overflows int32_t");
        }
        const __int128 result =
            static_cast<__int128>(value) * (commonRate / rate) +
            static_cast<__int128>(other.value) * (commonRate / other.rate);
        return {CheckedInt64(result), static_cast<int32_t>(commonRate)};
    }

    RationalTime sub(const RationalTime& other) const {
        CheckRate(rate);
        CheckRate(other.rate);
        const int64_t commonRate = Lcm(rate, other.rate);
        if (commonRate > std::numeric_limits<int32_t>::max()) {
            throw std::overflow_error(
                "RationalTime common rate overflows int32_t");
        }
        const __int128 result =
            static_cast<__int128>(value) * (commonRate / rate) -
            static_cast<__int128>(other.value) * (commonRate / other.rate);
        return {CheckedInt64(result), static_cast<int32_t>(commonRate)};
    }

    // Returns -1, 0 or 1 without converting either operand to floating point.
    constexpr int compare(const RationalTime& other) const {
        if (rate <= 0 || other.rate <= 0) {
            throw std::invalid_argument("RationalTime rate must be positive");
        }
        const __int128 left = static_cast<__int128>(value) * other.rate;
        const __int128 right = static_cast<__int128>(other.value) * rate;
        return left < right ? -1 : (left > right ? 1 : 0);
    }

    // Rescaling is exact. Silent rounding would move edit boundaries.
    RationalTime rescale(int32_t newRate) const {
        CheckRate(rate);
        CheckRate(newRate);
        const __int128 numerator = static_cast<__int128>(value) * newRate;
        if (numerator % rate != 0) {
            throw std::invalid_argument(
                "RationalTime cannot be rescaled exactly");
        }
        return {CheckedInt64(numerator / rate), newRate};
    }

    // Maps a time to the containing frame. This is floor division, including
    // for negative values, and supports rates such as 30000/1001.
    int64_t to_frames(int32_t framesPerSecond) const {
        return to_frames(framesPerSecond, 1);
    }

    int64_t to_frames(int32_t frameRateNumerator,
                      int32_t frameRateDenominator) const {
        CheckRate(rate);
        CheckRate(frameRateNumerator);
        CheckRate(frameRateDenominator);
        const __int128 numerator =
            static_cast<__int128>(value) * frameRateNumerator;
        const __int128 denominator =
            static_cast<__int128>(rate) * frameRateDenominator;
        __int128 quotient = numerator / denominator;
        if (numerator < 0 && numerator % denominator != 0) {
            --quotient;
        }
        return CheckedInt64(quotient);
    }

    constexpr bool operator==(const RationalTime& other) const {
        return compare(other) == 0;
    }
    constexpr bool operator!=(const RationalTime& other) const {
        return !(*this == other);
    }
    constexpr bool operator<(const RationalTime& other) const {
        return compare(other) < 0;
    }
    constexpr bool operator<=(const RationalTime& other) const {
        return compare(other) <= 0;
    }
    constexpr bool operator>(const RationalTime& other) const {
        return compare(other) > 0;
    }
    constexpr bool operator>=(const RationalTime& other) const {
        return compare(other) >= 0;
    }

private:
    static constexpr void CheckRate(int32_t candidate) {
        if (candidate <= 0) {
            throw std::invalid_argument("RationalTime rate must be positive");
        }
    }

    static constexpr int64_t Gcd(int64_t a, int64_t b) {
        while (b != 0) {
            const int64_t remainder = a % b;
            a = b;
            b = remainder;
        }
        return a;
    }

    static constexpr int64_t Lcm(int32_t a, int32_t b) {
        return (static_cast<int64_t>(a) / Gcd(a, b)) * b;
    }

    static constexpr int64_t CheckedInt64(__int128 valueIn) {
        if (valueIn < std::numeric_limits<int64_t>::min() ||
            valueIn > std::numeric_limits<int64_t>::max()) {
            throw std::overflow_error("RationalTime value overflows int64_t");
        }
        return static_cast<int64_t>(valueIn);
    }
};
