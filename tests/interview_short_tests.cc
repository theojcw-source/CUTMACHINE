#include "InterviewShort.h"
#include "Json.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
void Check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}
}  // namespace

int main() {
    Document document;
    const Ulid sourceId = "01K50000000000000000000001";
    document.sources = {{sourceId, "interview.mov", {25, 1}, {250, 25}}};
    document.sequence.tracks = {
        {"01K50000000000000000000002",
         "audio",
         0,
         {{"01K50000000000000000000003",
           sourceId,
           {50, 25},
           {100, 25},
           {25, 25}}}},
    };
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        ("cutmachine-interview-short-" + GenerateUlid());
    std::filesystem::create_directories(directory);
    {
        std::ofstream cache(directory / (sourceId + ".json"));
        cache << "{\"version\":1,\"media_id\":\"" << sourceId
              << "\",\"whisper_model\":\"test.bin\","
                 "\"source_rate\":{\"num\":25,\"den\":1},"
                 "\"words\":[{\"text\":\"Une\",\"start\":{\"value\":"
                 "50,\"rate\":25},\"end\":{\"value\":60,\"rate\":25}},{"
                 "\"text\":\"accroche.\",\"start\":{\"value\":60,"
                 "\"rate\":25},\"end\":{\"value\":75,\"rate\":25}}]}";
    }
    std::string json;
    std::string error;
    Check(DescribeTimelineTranscriptForAgent(document, directory, json, error),
          "describe transcript: " + error);
    mcp_json::Value root;
    Check(mcp_json::Value::Parse(json, root, error),
          "agent transcript JSON parses: " + error);
    const mcp_json::Value* spans = root.Find("spans");
    Check(spans && spans->IsArray() && spans->AsArray().size() == 1,
          "words are grouped into one semantic span");
    const mcp_json::Value& span = spans->AsArray().front();
    int64_t sourceIn = -1;
    Check(span.Find("source_in") && span.Find("source_in")->Find("value") &&
              span.Find("source_in")->Find("value")->AsInt64(sourceIn) &&
              sourceIn == 50 && span.Find("text") &&
              span.Find("text")->AsString() == "Une accroche.",
          "span exposes source-exact boundaries and readable text");

    // ---- span ids resolve to exact ranges; the caller never adds times ----
    std::vector<TimelineTranscriptSpan> built;
    Check(BuildTimelineTranscriptSpans(document, directory, built, error),
          "spans build: " + error);
    Check(built.size() == 1 && built[0].span_id == "S1",
          "spans are numbered in timeline order");

    // B6 -- both clips overlap the word "milieu", but the incoming clip
    // plays four of its six frames while the outgoing clip plays only two.
    // It must be emitted once on the incoming side and marked as cut.
    Document splitDocument = document;
    splitDocument.sequence.tracks[0].clips = {
        {"01K50000000000000000000003", sourceId, {0, 25}, {6, 25}, {0, 25}},
        {"01K50000000000000000000004", sourceId, {6, 25}, {8, 25}, {6, 25}},
    };
    {
        std::ofstream cache(directory / (sourceId + ".json"), std::ios::trunc);
        cache << "{\"version\":1,\"media_id\":\"" << sourceId
              << "\",\"whisper_model\":\"test.bin\","
                 "\"source_rate\":{\"num\":25,\"den\":1},\"words\":["
                 "{\"text\":\"Avant\",\"start\":{\"value\":0,\"rate\":25},"
                 "\"end\":{\"value\":4,\"rate\":25}},"
                 "{\"text\":\"milieu\",\"start\":{\"value\":4,\"rate\":25},"
                 "\"end\":{\"value\":10,\"rate\":25}},"
                 "{\"text\":\"Après\",\"start\":{\"value\":10,\"rate\":25},"
                 "\"end\":{\"value\":14,\"rate\":25}},"
                 "{\"text\":\"Hors\",\"start\":{\"value\":20,\"rate\":25},"
                 "\"end\":{\"value\":22,\"rate\":25}}]}";
    }
    std::vector<TimelineTranscriptSpan> splitSpans;
    Check(BuildTimelineTranscriptSpans(splitDocument, directory, splitSpans,
                                       error),
          "a transcript split through a word builds: " + error);
    std::string splitText;
    size_t middleCount = 0;
    bool middleStraddles = false;
    for (const TimelineTranscriptSpan& item : splitSpans) {
        splitText += item.text + " ";
        if (item.text.find("milieu") != std::string::npos) {
            ++middleCount;
            middleStraddles = item.straddles_cut;
            Check(item.source_in >= RationalTime{6, 25},
                  "the straddled word is attributed to its greatest overlap");
        }
    }
    Check(middleCount == 1 && middleStraddles,
          "a word cut between two clips appears once with straddles_cut");
    Check(splitText.find("Hors") == std::string::npos,
          "a word outside every played source range is omitted");
    const std::string splitJson =
        SerializeTimelineTranscriptSpans(splitDocument, splitSpans);
    std::vector<TimelineTranscriptSpan> splitReparsed;
    Check(ParseTimelineTranscriptSpans(splitJson, splitReparsed, error) &&
              std::any_of(splitReparsed.begin(), splitReparsed.end(),
                          [](const TimelineTranscriptSpan& item) {
                              return item.straddles_cut;
                          }),
          "straddles_cut survives the transcript view round trip");

    // Two contiguous spans of one source, and a third after a gap: the
    // shape that separates a legitimate merge from one that would swallow
    // a silence nobody asked to cut.
    const std::vector<TimelineTranscriptSpan> run = {
        {"S1", sourceId, {50, 25}, {25, 25}, {25, 25}, "Une accroche."},
        {"S2",
         sourceId,
         {75, 25},
         {30, 25},
         {50, 25},
         "Puis la suite.",
         false,
         true,
         true},
        {"S3", sourceId, {200, 25}, {20, 25}, {80, 25}, "Et la chute."},
        {"S4",
         "01K50000000000000000000009",
         {0, 25},
         {10, 25},
         {100, 25},
         "Autre source."},
    };
    Ulid resolvedSource;
    RationalTime resolvedIn;
    RationalTime resolvedDuration;

    Check(ResolveTranscriptSpanRange(run, "S1", "", resolvedSource, resolvedIn,
                                     resolvedDuration, error),
          "a single span resolves: " + error);
    Check(resolvedSource == sourceId && resolvedIn.compare({50, 25}) == 0 &&
              resolvedDuration.compare({25, 25}) == 0,
          "one span resolves to its own exact range");

    Check(ResolveTranscriptSpanRange(run, "S1", "S2", resolvedSource,
                                     resolvedIn, resolvedDuration, error),
          "a contiguous run resolves: " + error);
    Check(resolvedIn.compare({50, 25}) == 0 &&
              resolvedDuration.compare({55, 25}) == 0,
          "a contiguous run merges into one range spanning both spans");

    Check(!ResolveTranscriptSpanRange(run, "S2", "S3", resolvedSource,
                                      resolvedIn, resolvedDuration, error),
          "a run across a real editorial gap is refused");
    Check(error.find("breath") != std::string::npos,
          "the refusal names the reason and suggests separate segments");

    // A cue breaks on a pause between words, and that pause belongs to
    // neither cue. Refusing those made ordinary sentences unselectable: on a
    // real interview, three of eight editorial beats were rejected over gaps
    // of one and two frames. Anything under the cue segmenter's own maximum
    // gap is one continuous utterance and merges, breath included.
    const std::vector<TimelineTranscriptSpan> breathing = {
        {"S1", sourceId, {50, 25}, {25, 25}, {25, 25}, "Une accroche"},
        // Two frames of silence, well under kSubtitleCueMaximumGap.
        {"S2", sourceId, {77, 25}, {30, 25}, {52, 25}, "puis la suite."},
    };
    Check(ResolveTranscriptSpanRange(breathing, "S1", "S2", resolvedSource,
                                     resolvedIn, resolvedDuration, error),
          "a two-frame breath between cues still merges: " + error);
    Check(resolvedIn.compare({50, 25}) == 0 &&
              resolvedDuration.compare({57, 25}) == 0,
          "and the merged range keeps the breath rather than dropping it");

    // The boundary itself: exactly the cue segmenter's maximum gap is a real
    // break, not a breath, so it is refused.
    const std::vector<TimelineTranscriptSpan> atLimit = {
        {"S1", sourceId, {0, 25}, {25, 25}, {0, 25}, "Avant"},
        {"S2", sourceId, {25, 25}, {25, 25}, {25, 25}, "après"},
    };
    std::vector<TimelineTranscriptSpan> justOver = atLimit;
    // 0.7 s at 25 fps is 17.5 frames, so 18 frames is over the line.
    justOver[1].source_in = {25 + 18, 25};
    Check(!ResolveTranscriptSpanRange(justOver, "S1", "S2", resolvedSource,
                                      resolvedIn, resolvedDuration, error),
          "a gap past the cue segmenter's own limit is refused");

    Check(!ResolveTranscriptSpanRange(run, "S3", "S4", resolvedSource,
                                      resolvedIn, resolvedDuration, error),
          "a run across two sources is refused");
    Check(!ResolveTranscriptSpanRange(run, "S2", "S1", resolvedSource,
                                      resolvedIn, resolvedDuration, error),
          "a backwards run is refused rather than silently reordered");
    Check(!ResolveTranscriptSpanRange(run, "S9", "", resolvedSource, resolvedIn,
                                      resolvedDuration, error),
          "an unknown span id is refused");

    // The rendered view and the structured spans must stay the same thing:
    // a backend that can only hand out JSON still answers span lookups.
    std::vector<TimelineTranscriptSpan> reparsed;
    Check(ParseTimelineTranscriptSpans(
              SerializeTimelineTranscriptSpans(document, run), reparsed, error),
          "the rendered view parses back to spans: " + error);
    Check(reparsed.size() == run.size() && reparsed[1].span_id == "S2" &&
              reparsed[1].source_in.compare(run[1].source_in) == 0 &&
              reparsed[1].duration.compare(run[1].duration) == 0 &&
              reparsed[1].likely_hallucinated && reparsed[1].likely_incomplete,
          "a span survives the view round trip with its exact times and "
          "transcript guard");

    std::filesystem::remove_all(directory);
    std::cout << "interview short tests passed\n";
    return 0;
}
