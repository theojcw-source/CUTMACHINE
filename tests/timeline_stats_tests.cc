// QC-2026-09 (A8) -- the density check must follow the composited picture,
// not the number of clips stored on its tracks. This is deliberately a pure
// C++ test: the same result is consumed by CLI, MCP and a future UI.

#include "TimelineStats.h"

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

Document Fixture() {
    Document document;
    document.sequence.tracks = {
        {"01K30000000000000000000001",
         "video",
         0,
         {{"01K30000000000000000000011", {}, {}, {10, 25}, {0, 25}},
          {"01K30000000000000000000012", {}, {}, {10, 25}, {10, 25}},
          {"01K30000000000000000000013", {}, {}, {10, 25}, {20, 25}}}},
        {"01K30000000000000000000002",
         "video",
         1,
         {{"01K30000000000000000000021", {}, {}, {20, 25}, {10, 25}}}},
    };
    return document;
}

}  // namespace

int main() {
    Document document = Fixture();
    TimelineStats stats;
    std::string error;
    Check(CalculateTimelineStats(document, stats, error),
          "overlap fixture calculates: " + error);
    Check(stats.duration == RationalTime{30, 25},
          "duration spans the full timeline");
    Check(stats.visible_shots == 2,
          "clips hidden by V2 do not become visible shots");
    Check(stats.visible_cuts == 1,
          "only the visible transition is a cut");
    Check(stats.first_cut && *stats.first_cut == RationalTime{10, 25},
          "first visible cutaway is at the overlay start");
    Check(stats.plans_per_minute.numerator == 100 &&
              stats.plans_per_minute.denominator == 1,
          "plans per minute is an exact reduced ratio");
    Check(stats.cutaway_plans == 1 && stats.cutaway_share.numerator == 1 &&
              stats.cutaway_share.denominator == 2,
          "cutaway share counts visible plans, not hidden V1 clips");

    const std::string expected =
        "{\"duration\":{\"value\":30,\"rate\":25},"
        "\"visible_shots\":2,\"visible_cuts\":1,"
        "\"plans_per_minute\":{\"numerator\":100,\"denominator\":1},"
        "\"cutaway_plans\":1,\"cutaway_share\":{\"numerator\":1,"
        "\"denominator\":2},\"first_cut\":{\"value\":10,"
        "\"rate\":25}}";
    Check(SerializeTimelineStats(stats) == expected,
          "timeline stats JSON is canonical");

    document.sequence.tracks[1].visible = false;
    Check(CalculateTimelineStats(document, stats, error),
          "hidden overlay calculates: " + error);
    Check(stats.visible_shots == 3 && stats.visible_cuts == 2 &&
              stats.cutaway_plans == 0,
          "hidden tracks do not create visible plans or cutaways");
    Check(!stats.first_cut,
          "primary-track edits are not reported as a first cutaway");

    Document invalid = document;
    invalid.sequence.tracks[0].clips[0].duration.rate = 0;
    Check(!CalculateTimelineStats(invalid, stats, error) &&
              error == "clip '01K30000000000000000000011' has a non-positive "
                       "time rate",
          "invalid time rates are refused with a stable error");

    return failures == 0 ? 0 : 1;
}
