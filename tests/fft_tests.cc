#include "Fft.h"

#include <cmath>
#include <complex>
#include <iostream>
#include <vector>

namespace {

bool NearlyEqual(double a, double b, double tolerance) {
    return std::abs(a - b) <= tolerance;
}

bool TestImpulseIsFlatSpectrum() {
    constexpr size_t kSize = 64;
    std::vector<std::complex<double>> data(kSize, {0.0, 0.0});
    data[0] = {1.0, 0.0};
    fft::Forward(data);
    for (const std::complex<double>& value : data) {
        if (!NearlyEqual(std::abs(value), 1.0, 1e-9)) return false;
    }
    return true;
}

bool TestSinusoidHasExpectedPeakBin() {
    constexpr size_t kSize = 256;
    constexpr size_t kBin = 20;
    std::vector<std::complex<double>> data(kSize);
    for (size_t n = 0; n < kSize; ++n) {
        const double angle =
            2.0 * fft::kPi * static_cast<double>(kBin) * n / kSize;
        data[n] = {std::cos(angle), 0.0};
    }
    fft::Forward(data);
    size_t peakBin = 0;
    double peakMagnitude = 0.0;
    for (size_t k = 0; k < kSize / 2; ++k) {
        const double magnitude = std::abs(data[k]);
        if (magnitude > peakMagnitude) {
            peakMagnitude = magnitude;
            peakBin = k;
        }
    }
    // A real cosine at bin 20 puts half its energy at bin 20 and the mirror
    // half at N-20; both should read ~N/2, everywhere else near zero.
    return peakBin == kBin &&
           NearlyEqual(peakMagnitude, static_cast<double>(kSize) / 2.0, 1e-6);
}

bool TestParsevalEnergyIsConserved() {
    constexpr size_t kSize = 128;
    std::vector<std::complex<double>> data(kSize);
    double timeEnergy = 0.0;
    unsigned seed = 12345;
    for (size_t n = 0; n < kSize; ++n) {
        // A small deterministic LCG in place of <random> keeps this test
        // reproducible without relying on platform-specific distributions.
        seed = seed * 1103515245u + 12345u;
        const double sample =
            (static_cast<double>((seed >> 8) & 0xFFFF) / 65535.0) - 0.5;
        data[n] = {sample, 0.0};
        timeEnergy += sample * sample;
    }
    fft::Forward(data);
    double frequencyEnergy = 0.0;
    for (const std::complex<double>& value : data)
        frequencyEnergy += std::norm(value);
    frequencyEnergy /= static_cast<double>(kSize);
    return NearlyEqual(timeEnergy, frequencyEnergy, 1e-6);
}

bool TestRejectsNonPowerOfTwo() {
    std::vector<std::complex<double>> data(100, {0.0, 0.0});
    try {
        fft::Forward(data);
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

}  // namespace

int main() {
    struct Case {
        const char* name;
        bool (*run)();
    };
    const Case cases[] = {
        {"impulse produces a flat spectrum", TestImpulseIsFlatSpectrum},
        {"sinusoid peaks at its own bin", TestSinusoidHasExpectedPeakBin},
        {"energy is conserved (Parseval)", TestParsevalEnergyIsConserved},
        {"non-power-of-two length is rejected", TestRejectsNonPowerOfTwo},
    };
    bool ok = true;
    for (const Case& testCase : cases) {
        if (testCase.run()) {
            std::cout << "PASS: " << testCase.name << '\n';
        } else {
            std::cerr << "FAIL: " << testCase.name << '\n';
            ok = false;
        }
    }
    return ok ? 0 : 1;
}
