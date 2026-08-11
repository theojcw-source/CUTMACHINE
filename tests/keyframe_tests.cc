#include "Keyframes.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <string>

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

const Ulid kFirst = "01K00000000000000000000001";
const Ulid kSecond = "01K00000000000000000000002";
const Ulid kThird = "01K00000000000000000000003";

}  // namespace

int main() {
    KeyframeTrack track;
    Check(static_cast<bool>(track.Add({kSecond, {48, 48}, 10.0,
                                       KeyframeInterpolation::Linear})),
          "add second keyframe");
    Check(static_cast<bool>(track.Add(
              {kFirst, {0, 24}, 0.0, KeyframeInterpolation::Linear})),
          "add earlier keyframe out of order");
    Check(track.keyframes()[0].id == kFirst &&
              track.keyframes()[1].id == kSecond,
          "track order is exact time order, independent of insertion order");

    const KeyframeEvaluation midpoint = track.Evaluate({12, 24});
    Check(midpoint && midpoint.value == 5.0,
          "linear interpolation works across different rates");
    Check(track.Evaluate({-1, 24}).value == 0.0 &&
              track.Evaluate({96, 48}).value == 10.0,
          "evaluation clamps outside the keyed range");

    Check(static_cast<bool>(track.Update(kFirst, {0, 30000}, 3.0,
                                         KeyframeInterpolation::Hold)),
          "update mutable fields");
    Check(track.Evaluate({1001, 30000}).value == 3.0,
          "hold interpolation keeps the left value");
    Check(track.Evaluate({48, 48}).value == 10.0,
          "an exact keyframe uses its own value after a hold segment");

    const KeyframeResult duplicateTime =
        track.Add({kThird, {1, 1}, 20.0, KeyframeInterpolation::Linear});
    Check(duplicateTime.error == KeyframeError::DuplicateTime,
          "equivalent rational times collide exactly");
    Check(static_cast<bool>(track.Add(
              {kThird, {2, 1}, 20.0, KeyframeInterpolation::Linear})),
          "add a distinct third keyframe");
    Check(track.Add({kThird, {3, 1}, 30.0, KeyframeInterpolation::Linear})
                  .error == KeyframeError::DuplicateId,
          "stable IDs must be unique");

    const std::size_t sizeBeforeRejectedUpdate = track.size();
    Check(track.Update(kThird, {1, 1}, 99.0,
                       KeyframeInterpolation::Linear)
                  .error == KeyframeError::DuplicateTime &&
              track.size() == sizeBeforeRejectedUpdate &&
              track.keyframes().back().time == RationalTime{2, 1},
          "rejected updates leave the track unchanged");
    Check(track.Update(kThird, {3, 1},
                       std::numeric_limits<double>::infinity(),
                       KeyframeInterpolation::Linear)
                  .error == KeyframeError::NonFiniteValue,
          "non-finite parameter values are rejected");
    Check(track.Evaluate({0, 0}).error == KeyframeError::InvalidTime,
          "invalid query rates return a named error");

    Check(static_cast<bool>(track.Remove(kSecond)),
          "remove keyframe by stable ID");
    Check(track.Remove(kSecond).error == KeyframeError::NotFound,
          "removing a missing ID returns a named error");
    Check(std::string(KeyframeErrorName(KeyframeError::DuplicateTime)) ==
              "duplicate_time",
          "errors have stable machine-readable names");

    Check(KeyframeTrack::CanonicalValueString(-0.0) == "0",
          "negative zero has one canonical spelling");
    Check(KeyframeTrack::CanonicalValueString(0.1) ==
              "0.10000000000000001",
          "canonical values use deterministic round-trippable precision");

    KeyframeTrack empty;
    Check(empty.Evaluate({0, 24}).error == KeyframeError::EmptyTrack,
          "empty evaluation returns a named error");
    Check(empty.Add({"bad", {0, 24}, 1.0, KeyframeInterpolation::Linear})
                  .error == KeyframeError::InvalidId,
          "invalid persistent IDs are rejected");

    if (failures == 0) {
        std::cout << "PASS: keyframe core\n";
    }
    return failures == 0 ? 0 : 1;
}
