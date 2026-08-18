// Pure-C++ test for the Transport panel's scrub-bar fraction<->time
// boundary (ROADMAP.md F2.5). TransportBar.h has no AppKit dependency, so
// this builds and runs on a plain Linux host, the same way
// tests/timeline_view_tests.cc does for TimelineViewport.

#include "TransportBar.h"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

template <typename Function>
void Test(const std::string& name, Function function) {
    const int before = failures;
    try {
        function();
        if (before == failures) std::cout << "PASS: " << name << '\n';
    } catch (const std::exception& exception) {
        ++failures;
        std::cerr << "FAIL: " << name << ": " << exception.what() << '\n';
    }
}

}  // namespace

int main() {
    Test("J/L shuttle ramps and reverses deterministically", [] {
        Check(NextShuttleSpeed(0, 1) == 1, "first L press -> +1x");
        Check(NextShuttleSpeed(1, 1) == 2, "second L press -> +2x");
        Check(NextShuttleSpeed(2, 1) == 4, "third L press -> +4x");
        Check(NextShuttleSpeed(4, 1) == 4, "L remains capped at +4x");
        Check(NextShuttleSpeed(4, -1) == -1, "J reverses direction at -1x");
        Check(NextShuttleSpeed(-1, -1) == -2, "second J press -> -2x");
        Check(NextShuttleSpeed(-2, -1) == -4, "third J press -> -4x");
        Check(NextShuttleSpeed(-4, 0) == 0, "K stops shuttle playback");
    });

    Test("TimeToFraction: start and end of the range map to 0 and 1", [] {
        const ScrubBarRange range{RationalTime{100, 25}};  // 4 seconds @25fps
        Check(range.TimeToFraction({0, 25}) == 0.0, "time zero -> 0.0");
        Check(range.TimeToFraction({100, 25}) == 1.0, "duration -> 1.0");
    });

    Test("TimeToFraction: midpoint is 0.5 regardless of the time's own rate",
         [] {
             const ScrubBarRange range{RationalTime{100, 25}};
             const double atNativeRate = range.TimeToFraction({50, 25});
             const double atOtherRate = range.TimeToFraction({1, 1});  // 1s
             Check(std::abs(atNativeRate - 0.5) < 1e-12,
                   "50/25 (2s of 4s) should be fraction 0.5");
             Check(std::abs(atOtherRate - 0.25) < 1e-12,
                   "1s of 4s should be fraction 0.25 regardless of rate");
         });

    Test(
        "TimeToFraction clamps out-of-range positions instead of "
        "extrapolating",
        [] {
            const ScrubBarRange range{RationalTime{100, 25}};
            Check(range.TimeToFraction({-25, 25}) == 0.0,
                  "negative time clamps to 0.0");
            Check(range.TimeToFraction({200, 25}) == 1.0,
                  "past-duration time clamps to 1.0");
        });

    Test("TimeToFraction on a zero-duration sequence is always 0", [] {
        const ScrubBarRange range{RationalTime{0, 25}};
        Check(range.TimeToFraction({0, 25}) == 0.0, "empty sequence -> 0.0");
    });

    Test("FractionToTime: 0 and 1 land exactly on the endpoints", [] {
        const ScrubBarRange range{RationalTime{100, 25}};
        const RationalTime start = range.FractionToTime(0.0, 25);
        const RationalTime end = range.FractionToTime(1.0, 25);
        Check(start == RationalTime(0, 25), "fraction 0.0 -> time zero");
        Check(end == RationalTime(100, 25), "fraction 1.0 -> duration");
    });

    Test("FractionToTime clamps fractions outside [0, 1]", [] {
        const ScrubBarRange range{RationalTime{100, 25}};
        Check(range.FractionToTime(-0.5, 25) == RationalTime(0, 25),
              "negative fraction clamps to time zero");
        Check(range.FractionToTime(1.5, 25) == RationalTime(100, 25),
              "fraction past 1.0 clamps to duration");
    });

    Test("FractionToTime rounds to the nearest tick at the requested rate", [] {
        const ScrubBarRange range{RationalTime{3, 1}};  // 3 seconds
        const RationalTime quarter = range.FractionToTime(0.5, 2);
        // 1.5s at rate 2 is exactly value 3.
        Check(quarter == RationalTime(3, 2),
              "half of 3s at rate 2 should be exactly 1.5s");
    });

    Test(
        "round-tripping TimeToFraction -> FractionToTime is stable at the "
        "duration's own rate",
        [] {
            const ScrubBarRange range{RationalTime{48000, 48000}};  // 1s
            for (int64_t sample : {0LL, 12000LL, 24000LL, 36000LL, 48000LL}) {
                const RationalTime original{sample, 48000};
                const double fraction = range.TimeToFraction(original);
                const RationalTime roundTripped =
                    range.FractionToTime(fraction, 48000);
                Check(roundTripped == original,
                      "round trip through a fraction must land back on the "
                      "same sample-accurate time");
            }
        });

    Test("FractionToTime rejects a non-finite fraction", [] {
        const ScrubBarRange range{RationalTime{100, 25}};
        bool threw = false;
        try {
            range.FractionToTime(std::nan(""), 25);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        Check(threw, "NaN fraction must be rejected, not silently clamped");
    });

    Test("both directions reject a non-positive rate", [] {
        const ScrubBarRange range{RationalTime{100, 25}};
        bool threw = false;
        try {
            range.FractionToTime(0.5, 0);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        Check(threw, "rate <= 0 in FractionToTime must throw");

        threw = false;
        try {
            range.TimeToFraction({10, 0});
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        Check(threw, "a time with rate <= 0 in TimeToFraction must throw");
    });

    Test("a zero-rate duration is rejected by both directions", [] {
        const ScrubBarRange range{RationalTime{0, 0}};
        bool threw = false;
        try {
            range.TimeToFraction({0, 25});
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        Check(threw, "TimeToFraction must reject an invalid duration rate");

        threw = false;
        try {
            range.FractionToTime(0.5, 25);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        Check(threw, "FractionToTime must reject an invalid duration rate");
    });

    return failures == 0 ? 0 : 1;
}
