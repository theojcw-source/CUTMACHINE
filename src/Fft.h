#pragma once

// A minimal iterative radix-2 Cooley-Tukey FFT (Cooley & Tukey, 1965),
// in-place, on a power-of-two length buffer.
//
// CUTMACHINE has no other DSP or numerics dependency, and BeatDetection.cc
// is the only caller: it always transforms one fixed, small power-of-two
// window (see BeatDetectionSettings::fft_size). Vetting and pinning an
// external FFT library through CMake FetchContent — a new build dependency,
// a new license to track, a new thing to go stale — is more machinery than
// this single, well-understood transform justifies. The algorithm below is
// textbook material with no relation to any third-party implementation.

#include <cmath>
#include <complex>
#include <cstddef>
#include <stdexcept>
#include <vector>

namespace fft {

constexpr double kPi = 3.14159265358979323846;

inline bool IsPowerOfTwo(size_t n) { return n != 0 && (n & (n - 1)) == 0; }

// Forward in-place FFT. `data.size()` must be a power of two.
inline void Forward(std::vector<std::complex<double>>& data) {
    const size_t n = data.size();
    if (!IsPowerOfTwo(n)) {
        throw std::invalid_argument(
            "fft::Forward requires a power-of-two length");
    }
    if (n <= 1) return;

    // Bit-reversal permutation.
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(data[i], data[j]);
    }

    // Iterative Cooley-Tukey butterflies, one pass per doubling stage.
    for (size_t length = 2; length <= n; length <<= 1) {
        const double angle = -2.0 * kPi / static_cast<double>(length);
        const std::complex<double> root(std::cos(angle), std::sin(angle));
        for (size_t start = 0; start < n; start += length) {
            std::complex<double> twiddle(1.0, 0.0);
            for (size_t k = 0; k < length / 2; ++k) {
                const std::complex<double> even = data[start + k];
                const std::complex<double> odd =
                    data[start + k + length / 2] * twiddle;
                data[start + k] = even + odd;
                data[start + k + length / 2] = even - odd;
                twiddle *= root;
            }
        }
    }
}

}  // namespace fft
