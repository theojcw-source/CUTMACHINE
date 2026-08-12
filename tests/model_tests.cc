#include "ColorManagement.h"
#include "Document.h"
#include "Timeline.h"
#include "Ulid.h"

#include <cmath>
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
    document.sequence.tracks = {
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
    Test("reference color transfer functions", [] {
        const auto Near = [](double actual, double expected, double epsilon,
                             const std::string& message) {
            Check(std::abs(actual - expected) <= epsilon,
                  message + ": got " + std::to_string(actual));
        };
        Near(DecodeSonySLog3(95.0 / 1023.0), 0.0, 1e-12,
             "S-Log3 code 95 is scene black");
        Near(DecodeSonySLog3(420.0 / 1023.0), 0.18, 1e-12,
             "S-Log3 code 420 is 18% grey");
        Near(DecodeSonySLog3(598.0 / 1023.0), 0.9, 0.005,
             "S-Log3 code 598 is 90% white");
        for (double value : {-0.01, 0.0, 0.0078125, 0.18, 4.0})
            Near(DecodeAcesCct(EncodeAcesCct(value)), value, 1e-12,
                 "ACEScct round trip");
        Near(EncodeHlg(DecodeHlg(0.75)), 0.75, 1e-9,
             "HLG reference white signal round trip");
        Near(EncodeHlg(0.9 * HlgSceneReflectionScale()), 0.75, 1e-9,
             "90% scene white maps to HLG reference white");
        ColorManagementSettings sony;
        sony.enabled = true;
        sony.input_gamut = "sony_sgamut3_cine";
        sony.input_transfer = "sony_slog3";
        sony.input_ycbcr_matrix = "bt709";
        sony.input_range = "full";
        sony.working_gamut = "acescct";
        sony.output_gamut = "rec2020";
        sony.output_transfer = "hlg";
        const double whiteSignal = 598.0 / 1023.0;
        const RgbColor hlg = TransformColorForOutput(
            sony, {whiteSignal, whiteSignal, whiteSignal});
        Near(hlg.red, 0.75, 0.005,
             "neutral S-Log3 90% white maps to HLG reference white");
        Near(hlg.red, hlg.green, 2e-5,
             "neutral conversion remains neutral in Rec.2020");
        Near(hlg.green, hlg.blue, 2e-5, "neutral conversion has no blue cast");
        const YuvCodeParameters full = BuildYuvCodeParameters(10, true);
        const YuvCodeParameters legal = BuildYuvCodeParameters(10, false);
        Near(full.y_offset, 0.0, 1e-7, "full range starts at code zero");
        Near(legal.y_offset, 64.0 / 1023.0, 1e-7,
             "10-bit legal range starts at code 64");
        Near(legal.y_scale, 1023.0 / 876.0, 1e-7,
             "10-bit legal luma spans 876 codes");
        const YuvMatrixParameters bt709 = BuildYuvMatrixParameters(false);
        const YuvMatrixParameters bt2020 = BuildYuvMatrixParameters(true);
        Near(bt709.red_from_cr, 1.5748, 1e-6,
             "BT.709 uses normalized chroma coefficients once");
        Near(bt2020.red_from_cr, 1.4746, 1e-6,
             "BT.2020 NCL uses its own normalized matrix");
    });

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

    Test("legacy document versions are rejected", [] {
        const std::string json =
            "{\"version\":1,\"sources\":[{\"id\":"
            "\"01K90000000000000000000001\",\"path\":\"legacy.mov\","
            "\"rate\":{\"num\":25,\"den\":1},\"duration\":"
            "{\"value\":100,\"rate\":25}}],\"tracks\":[]}";
        Document document;
        std::string error;
        Check(!Document::LoadFromString(json, document, error) &&
                  error.find("unsupported document version 1") !=
                      std::string::npos,
              "version 1 must be rejected explicitly");
    });

    Test("version 2 root timelines are rejected", [] {
        const std::string json =
            "{\"version\":2,\"sequence\":{\"id\":"
            "\"01K91000000000000000000001\",\"name\":\"Legacy edit\","
            "\"width\":1920,\"height\":1080,\"frame_rate\":{\"num\":25,"
            "\"den\":1}},\"library\":[],\"bins\":[],\"markers\":[{"
            "\"id\":\"01K91000000000000000000002\",\"name\":\"Cut\","
            "\"time\":{\"value\":10,\"rate\":25},\"color\":\"yellow\","
            "\"category\":\"edit\"}],\"sources\":[],\"tracks\":[]}";
        Document document;
        std::string error;
        Check(!Document::LoadFromString(json, document, error) &&
                  error.find("unsupported document version 2") !=
                      std::string::npos,
              "version 2 must be rejected explicitly");
    });

    Test("sequence settings persist and validate", [] {
        Document document = ValidDocument();
        document.sequence.name = "Vertical master";
        document.sequence.width = 1080;
        document.sequence.height = 1920;
        document.sequence.frame_rate = {30000, 1001};
        std::string error;
        Document loaded;
        Check(Document::LoadFromString(document.SaveToString(), loaded, error),
              "sequence settings load: " + error);
        Check(loaded.sequence.id == document.sequence.id &&
                  loaded.sequence.name == "Vertical master" &&
                  loaded.sequence.width == 1080 &&
                  loaded.sequence.height == 1920 &&
                  loaded.sequence.frame_rate.num == 30000 &&
                  loaded.sequence.frame_rate.den == 1001,
              "sequence identity, dimensions and rational cadence round-trip");
        loaded.sequence.width = 0;
        Check(!loaded.Validate(error) &&
                  error.find("sequence") != std::string::npos,
              "invalid sequence dimensions are rejected");
    });

    Test("marker model persists exact time and validates identity", [] {
        Document document = ValidDocument();
        document.sequence.markers = {
            {"01K83000000000000000000001",
             "Act 1",
             {1001, 24000},
             "#ffcc00",
             "chapter"},
            {"01K83000000000000000000002",
             "Needs sound",
             {73, 25},
             "violet",
             "todo"},
        };
        std::string error;
        Document loaded;
        const std::string canonical = document.SaveToString();
        Check(Document::LoadFromString(canonical, loaded, error),
              "markers load from canonical JSON: " + error);
        Check(loaded.SaveToString() == canonical &&
                  loaded.sequence.markers.size() == 2,
              "marker document JSON round-trips byte-identically");
        Check(loaded.FindMarker(document.sequence.markers[0].id) &&
                  loaded.FindMarker(document.sequence.markers[0].id)->time ==
                      RationalTime{1001, 24000},
              "marker lookup retains exact rational time");

        loaded.sequence.markers[0].id = loaded.sequence.tracks[0].id;
        Check(!loaded.Validate(error) &&
                  error.find("duplicate ID") != std::string::npos,
              "marker IDs participate in global identity validation");
        loaded = document;
        loaded.sequence.markers[0].category.clear();
        Check(!loaded.Validate(error) &&
                  error.find("marker") != std::string::npos,
              "empty marker categories are rejected");
    });

    Test("color management settings persist and validate", [] {
        Document document = ValidDocument();
        document.color_management.enabled = true;
        document.color_management.input_gamut = "sony_sgamut3_cine";
        document.color_management.input_transfer = "sony_slog3";
        document.color_management.input_ycbcr_matrix = "bt709";
        document.color_management.input_range = "full";
        document.color_management.working_gamut = "acescct";
        document.color_management.output_gamut = "rec2020";
        document.color_management.output_transfer = "hlg";
        std::string error;
        Check(document.Validate(error),
              "Sony/HLG pipeline must validate: " + error);
        Document loaded;
        Check(Document::LoadFromString(document.SaveToString(), loaded, error),
              "Sony/HLG pipeline must load: " + error);
        Check(loaded.color_management.enabled &&
                  loaded.color_management.input_gamut == "sony_sgamut3_cine" &&
                  loaded.color_management.input_transfer == "sony_slog3" &&
                  loaded.color_management.input_range == "full" &&
                  loaded.color_management.working_gamut == "acescct" &&
                  loaded.color_management.output_gamut == "rec2020" &&
                  loaded.color_management.output_transfer == "hlg",
              "all color pipeline stages must round-trip");

        loaded.color_management.output_gamut = "rec709";
        Check(!loaded.Validate(error) && error.find("HLG") != std::string::npos,
              "HLG with a non-Rec.2020 gamut must be rejected");
        loaded.color_management.output_gamut = "rec2020";
        loaded.color_management.input_transfer = "unknown_log";
        Check(!loaded.Validate(error) &&
                  error.find("color_management") != std::string::npos,
              "unknown transfer functions must be rejected");
    });

    Test("timeline resolution", [] {
        const Document document = ValidDocument();
        Timeline timeline(document);
        const Ulid& primaryTrack = document.sequence.tracks[0].id;
        const Ulid& sparseTrack = document.sequence.tracks[1].id;

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

    Test(
        "cross dissolve persists, validates handles and resolves two layers",
        [] {
            Document document = ValidDocument();
            document.sequence.transitions = {{
                "01K00000000000000000000008",
                document.sequence.tracks[0].id,
                document.sequence.tracks[0].clips[0].id,
                document.sequence.tracks[0].clips[1].id,
                "cross_dissolve",
                {2, 25},
                TransitionAlignment::Center,
            }};
            std::string error;
            Check(document.Validate(error),
                  "transition fixture validates: " + error);
            Document loaded;
            const std::string json = document.SaveToString();
            Check(Document::LoadFromString(json, loaded, error) &&
                      loaded.SaveToString() == json &&
                      loaded.FindTransition("01K00000000000000000000008"),
                  "transition JSON round-trips canonically");

            Timeline timeline(document);
            const Ulid track = document.sequence.tracks[0].id;
            const auto beforeCut = timeline.ResolveTrackLayers(track, {1, 25});
            Check(beforeCut.size() == 2 &&
                      beforeCut[0].frame.source_frame == 101 &&
                      beforeCut[1].frame.source_frame == 9 &&
                      beforeCut[1].opacity == 0.0f,
                  "transition begins with outgoing image and incoming head "
                  "handle");
            const auto atCut = timeline.ResolveTrackLayers(track, {2, 25});
            Check(atCut.size() == 2 && atCut[0].frame.source_frame == 102 &&
                      atCut[1].frame.source_frame == 10 &&
                      std::abs(atCut[1].opacity - 0.5f) < 0.0001f,
                  "cut center resolves both source handles at 50 percent");
            const auto after = timeline.ResolveTrackLayers(track, {3, 25});
            Check(
                after.size() == 1 && after[0].frame.source_frame == 11,
                "transition end is half-open and returns to normal resolution");

            document.sequence.tracks[0].clips[1].source_in = {0, 25};
            Check(!document.Validate(error) &&
                      error.find("media handles") != std::string::npos,
                  "a centered dissolve without an incoming head handle is "
                  "rejected");
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
        document.sequence.tracks = {
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

        const auto before = timeline.ResolveTrack(
            document.sequence.tracks[0].id, {29999, 30000});
        Check(before && before->source_id == document.sources[0].id &&
                  before->source_frame == 24,
              "tick before cut must remain on the last 25 fps frame");
        const auto atCut = timeline.ResolveTrack(document.sequence.tracks[0].id,
                                                 {30000, 30000});
        Check(atCut && atCut->source_id == document.sources[1].id &&
                  atCut->source_frame == 100,
              "cut tick must resolve to the first 30000/1001 clip frame");
    });

    Test("document validation", [] {
        {
            Document value = ValidDocument();
            value.sequence.tracks[0].id = value.sources[0].id;
            ExpectInvalid(value, "duplicate ID", "duplicate IDs");
        }
        {
            Document value = ValidDocument();
            value.sequence.tracks[0].clips[0].source_id =
                "01K00000000000000000000009";
            ExpectInvalid(value, "unknown source_id", "unknown source_id");
        }
        {
            Document value = ValidDocument();
            value.sequence.tracks[0].clips[1].timeline_in = {1, 25};
            ExpectInvalid(value, "overlap", "overlapping clips");
        }
        {
            Document value = ValidDocument();
            std::swap(value.sequence.tracks[0].clips[0],
                      value.sequence.tracks[0].clips[1]);
            ExpectInvalid(value, "not sorted", "unsorted clips");
        }
        {
            Document value = ValidDocument();
            value.sequence.tracks[0].clips[0].duration.value = 0;
            ExpectInvalid(value, "zero or negative duration", "zero duration");
        }
        {
            Document value = ValidDocument();
            value.sequence.tracks[0].clips[0].duration.value = -1;
            ExpectInvalid(value, "zero or negative duration",
                          "negative duration");
        }
        {
            Document value = ValidDocument();
            value.sequence.tracks[0].clips[0].source_in = {199, 25};
            ExpectInvalid(value, "outside source bounds", "source bounds");
        }
        {
            Document value = ValidDocument();
            value.sources[0].rate.num = 0;
            ExpectInvalid(value, "media rate", "zero source media rate");
        }
        {
            Document value = ValidDocument();
            value.sequence.tracks[0].clips[0].timeline_in.rate = 0;
            ExpectInvalid(value, "time rate", "zero RationalTime rate");
        }
    });

    Test("track lock state persists", [] {
        Document document = ValidDocument();
        document.sequence.tracks[0].locked = true;
        document.sequence.tracks[0].sync_lock = false;
        Document loaded;
        std::string error;
        Check(Document::LoadFromString(document.SaveToString(), loaded, error),
              "locked document loads: " + error);
        Check(loaded.sequence.tracks[0].locked,
              "locked state survives canonical JSON");
        Check(!loaded.sequence.tracks[0].sync_lock,
              "sync lock state survives canonical JSON");
    });

    Test("derived proxy path persists without replacing the original", [] {
        Document document = ValidDocument();
        LibraryMedia media;
        media.id = document.sources[0].id;
        media.path = document.sources[0].path;
        media.filename = "A.MP4";
        media.codec = "h264";
        media.width = 3840;
        media.height = 2160;
        media.rate = document.sources[0].rate;
        media.duration = document.sources[0].duration;
        media.orientation = "landscape";
        document.library.push_back(std::move(media));
        const std::string original = document.library[0].path;
        document.library[0].proxy_path =
            ".cutmachine/proxies/01K00000000000000000000001.mov";
        Document loaded;
        std::string error;
        Check(Document::LoadFromString(document.SaveToString(), loaded, error),
              "document with proxy loads: " + error);
        Check(
            loaded.library[0].path == original &&
                loaded.library[0].proxy_path == document.library[0].proxy_path,
            "proxy remains derived metadata beside the original path");
    });

    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }
    std::cout << "All model tests passed\n";
    return 0;
}
