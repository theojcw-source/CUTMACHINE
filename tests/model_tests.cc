#include "Document.h"
#include "Timeline.h"
#include "Ulid.h"

#include <cstdio>
#include <filesystem>
#include <functional>
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
    try {
        function();
        if (failures == 0) {
            std::cout << "PASS: " << name << '\n';
        }
    } catch (const std::exception& exception) {
        ++failures;
        std::cerr << "FAIL: " << name << ": " << exception.what() << '\n';
    }
}

Document ValidDocument() {
    Document document;
    document.sources = {
        {"01K00000000000000000000001", "A.MP4", {25, 1}, {200, 25}},
        {"01K00000000000000000000002", "B.MP4", {25, 1}, {200, 25}},
    };
    document.tracks = {
        {"01K00000000000000000000003",
         "video",
         0,
         {
             {"01K00000000000000000000004",
              "01K00000000000000000000001",
              {100, 25},
              {2, 25},
              {0, 25}},
             {"01K00000000000000000000005",
              "01K00000000000000000000002",
              {10, 25},
              {2, 25},
              {2, 25}},
         }},
        {"01K00000000000000000000006",
         "video",
         1,
         {
             {"01K00000000000000000000007",
              "01K00000000000000000000001",
              {0, 25},
              {1, 25},
              {1, 25}},
         }},
    };
    return document;
}

void ExpectInvalid(Document document, const std::string& expected,
                   const std::string& label) {
    std::string error;
    Document loaded;
    Check(!Document::LoadFromString(document.SaveToString(), loaded, error),
          label + " must be rejected while loading");
    Check(
        error.find(expected) != std::string::npos,
        label + " error must contain '" + expected + "', got '" + error + "'");
}

}  // namespace

int main() {
    Test("RationalTime arithmetic across rates", [] {
        const RationalTime a{1, 24};
        const RationalTime b{1, 48};
        const RationalTime sum = a.add(b);
        Check(sum.value == 3 && sum.rate == 48, "1/24 + 1/48 must be 3/48");
        Check(a.compare(RationalTime{2, 48}) == 0,
              "equivalent rates compare equal");
        Check(sum.sub(a) == b, "sub must preserve the exact rational result");
        Check(a.rescale(48).value == 2, "exact rescale must produce 2/48");
        Check(RationalTime{1001, 30000}.to_frames(30000, 1001) == 1,
              "30000/1001 frame mapping must remain integral");
        const Ulid generated = GenerateUlid();
        Check(IsValidUlid(generated), "generated IDs must be valid ULIDs");
        Check(generated != GenerateUlid(),
              "successive generated ULIDs must differ");
    });

    Test("load/save/load canonical round trip", [] {
        const Document original = ValidDocument();
        std::string error;
        Check(original.Validate(error), "fixture must validate: " + error);
        const std::filesystem::path first =
            std::filesystem::temp_directory_path() / (GenerateUlid() + ".json");
        const std::filesystem::path second =
            std::filesystem::temp_directory_path() / (GenerateUlid() + ".json");
        Check(original.Save(first.string(), error), "first save: " + error);
        Document loaded;
        Check(Document::Load(first.string(), loaded, error), "load: " + error);
        Check(loaded.Save(second.string(), error), "second save: " + error);
        Document loadedAgain;
        Check(Document::Load(second.string(), loadedAgain, error),
              "second load: " + error);
        Check(loaded.SaveToString() == loadedAgain.SaveToString(),
              "canonical JSON must be byte-identical after second load");
        std::filesystem::remove(first);
        std::filesystem::remove(second);
    });

    Test("version 1 documents migrate to the version 2 library", [] {
        const std::string json =
            "{\"version\":1,\"sources\":[{\"id\":"
            "\"01K90000000000000000000001\",\"path\":\"legacy.mov\","
            "\"rate\":{\"num\":25,\"den\":1},\"duration\":"
            "{\"value\":100,\"rate\":25}}],\"tracks\":[]}";
        Document document;
        std::string error;
        Check(Document::LoadFromString(json, document, error),
              "version 1 must load: " + error);
        Check(document.version == 2, "version 1 must migrate in memory");
        Check(document.library.size() == 1,
              "the legacy source must remain visible in the library");
        Check(document.library[0].id == document.sources[0].id &&
                  !document.library[0].metadata_complete,
              "migration must preserve identity without inventing metadata");
    });

    Test("timeline resolution", [] {
        const Document document = ValidDocument();
        Timeline timeline(document);
        const Ulid& primaryTrack = document.tracks[0].id;
        const Ulid& sparseTrack = document.tracks[1].id;

        const auto first = timeline.ResolveTrack(primaryTrack, {0, 25});
        Check(first && first->source_id == document.sources[0].id &&
                  first->source_frame == 100,
              "first timeline frame must map to first clip source frame 100");

        const auto cut = timeline.ResolveTrack(primaryTrack, {2, 25});
        Check(cut && cut->source_id == document.sources[1].id &&
                  cut->source_frame == 10,
              "half-open cut boundary must map to second clip");

        Check(!timeline.ResolveTrack(sparseTrack, {0, 25}),
              "position before sparse clip must resolve to a hole");

        const auto last = timeline.ResolveTrack(primaryTrack, {3, 25});
        Check(last && last->source_id == document.sources[1].id &&
                  last->source_frame == 11,
              "last timeline frame must map to second clip source frame 11");
        Check(!timeline.ResolveTrack(primaryTrack, {4, 25}),
              "timeline end is exclusive");

        const auto allTracks = timeline.Resolve({0, 25});
        Check(
            allTracks.size() == 2 && allTracks[0].frame && !allTracks[1].frame,
            "Resolve must return one populated-or-hole result per track");
    });

    Test("mixed-rate cut resolution at the common timebase", [] {
        Document document;
        document.sources = {
            {"01K10000000000000000000001", "25.MP4", {25, 1}, {250, 25}},
            {"01K10000000000000000000002",
             "2997.MP4",
             {30000, 1001},
             {300300, 30000}},
        };
        document.tracks = {
            {"01K10000000000000000000003",
             "video",
             0,
             {
                 {"01K10000000000000000000004",
                  "01K10000000000000000000001",
                  {0, 25},
                  {25, 25},
                  {0, 25}},
                 {"01K10000000000000000000005",
                  "01K10000000000000000000002",
                  {100100, 30000},
                  {1001, 30000},
                  {30000, 30000}},
             }},
        };
        std::string error;
        Check(document.Validate(error),
              "mixed-rate fixture must validate: " + error);
        Timeline timeline(document);
        Check(timeline.Duration().rate == 30000,
              "timeline timebase must be the exact LCM 30000");

        const auto before =
            timeline.ResolveTrack(document.tracks[0].id, {29999, 30000});
        Check(before && before->source_id == document.sources[0].id &&
                  before->source_frame == 24,
              "tick before cut must remain on the last 25 fps frame");
        const auto atCut =
            timeline.ResolveTrack(document.tracks[0].id, {30000, 30000});
        Check(atCut && atCut->source_id == document.sources[1].id &&
                  atCut->source_frame == 100,
              "cut tick must resolve to the first 30000/1001 clip frame");
    });

    Test("document validation", [] {
        {
            Document value = ValidDocument();
            value.tracks[0].id = value.sources[0].id;
            ExpectInvalid(value, "duplicate ID", "duplicate IDs");
        }
        {
            Document value = ValidDocument();
            value.tracks[0].clips[0].source_id = "01K00000000000000000000009";
            ExpectInvalid(value, "unknown source_id", "unknown source_id");
        }
        {
            Document value = ValidDocument();
            value.tracks[0].clips[1].timeline_in = {1, 25};
            ExpectInvalid(value, "overlap", "overlapping clips");
        }
        {
            Document value = ValidDocument();
            std::swap(value.tracks[0].clips[0], value.tracks[0].clips[1]);
            ExpectInvalid(value, "not sorted", "unsorted clips");
        }
        {
            Document value = ValidDocument();
            value.tracks[0].clips[0].duration.value = 0;
            ExpectInvalid(value, "zero or negative duration", "zero duration");
        }
        {
            Document value = ValidDocument();
            value.tracks[0].clips[0].duration.value = -1;
            ExpectInvalid(value, "zero or negative duration",
                          "negative duration");
        }
        {
            Document value = ValidDocument();
            value.tracks[0].clips[0].source_in = {199, 25};
            ExpectInvalid(value, "outside source bounds", "source bounds");
        }
        {
            Document value = ValidDocument();
            value.sources[0].rate.num = 0;
            ExpectInvalid(value, "media rate", "zero source media rate");
        }
        {
            Document value = ValidDocument();
            value.tracks[0].clips[0].timeline_in.rate = 0;
            ExpectInvalid(value, "time rate", "zero RationalTime rate");
        }
    });

    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }
    std::cout << "All model tests passed\n";
    return 0;
}
