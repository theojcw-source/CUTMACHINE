// Pure-C++ test for the Media panel's tab content view-model (ROADMAP.md
// F2.3). MediaPanelModel.h has no AppKit dependency -- it links only
// against cutmachine_model (Document.h/Operations.h/Ulid.h) -- so it builds
// and runs on a plain Linux host, the same way tests/project_bin_tests.cc
// does.

#include "MediaPanelModel.h"

#include <iostream>
#include <set>
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
    function();
    if (before == failures) std::cout << "PASS: " << name << '\n';
}

LibraryMedia MakeMedia(std::string filename, bool hasAudio, Ulid binId = {}) {
    LibraryMedia media;
    media.id = GenerateUlid();
    media.filename = std::move(filename);
    media.codec = "h264";
    media.has_audio = hasAudio;
    media.bin_id = std::move(binId);
    return media;
}

}  // namespace

int main() {
    using namespace ui::media_panel;

    Test("AllTabs lists exactly Media, Audio, Captions in that order", [] {
        const auto& tabs = AllTabs();
        Check(tabs.size() == 3, "expected exactly 3 tabs");
        Check(tabs[0] == Tab::Media, "Media should be first");
        Check(tabs[1] == Tab::Audio, "Audio should be second");
        Check(tabs[2] == Tab::Captions, "Captions should be third");
    });

    Test("TabTitle is non-empty and distinct for every tab", [] {
        std::set<std::string> titles;
        for (Tab tab : AllTabs()) {
            const std::string title = TabTitle(tab);
            Check(!title.empty(), "tab title must not be empty");
            titles.insert(title);
        }
        Check(titles.size() == AllTabs().size(),
              "tab titles must be pairwise distinct");
    });

    Test("CarriesAudio mirrors LibraryMedia::has_audio exactly", [] {
        Check(CarriesAudio(MakeMedia("a.mp4", true)),
              "has_audio=true must read as audio-capable");
        Check(!CarriesAudio(MakeMedia("b.mp4", false)),
              "has_audio=false must not read as audio-capable");
    });

    Test("FilterAudioSources drops silent media and keeps audio-capable ones",
         [] {
             std::vector<LibraryMedia> library{
                 MakeMedia("interview.mp4", true),
                 MakeMedia("broll_silent.mp4", false),
                 MakeMedia("music_bed.mp4", true),
             };
             const auto result =
                 FilterAudioSources(library, /*anyBin=*/true,
                                    /*wantRoot=*/false, /*wantBinId=*/{}, "");
             Check(result.size() == 2, "expected 2 audio-capable entries");
             for (const LibraryMedia* media : result)
                 Check(media->has_audio, "every result must carry audio");
         });

    Test("FilterAudioSources honors bin membership", [] {
        const Ulid binA = GenerateUlid();
        const Ulid binB = GenerateUlid();
        std::vector<LibraryMedia> library{
            MakeMedia("in_a.mp4", true, binA),
            MakeMedia("in_b.mp4", true, binB),
            MakeMedia("in_root.mp4", true, Ulid{}),
        };
        const auto inA = FilterAudioSources(library, false, false, binA, "");
        Check(inA.size() == 1 && inA.front()->filename == "in_a.mp4",
              "bin filter should keep only that bin's audio-capable media");
        const auto inRoot = FilterAudioSources(library, false, true, {}, "");
        Check(inRoot.size() == 1 && inRoot.front()->filename == "in_root.mp4",
              "root filter should keep only unbinned audio-capable media");
        const auto everywhere =
            FilterAudioSources(library, true, false, {}, "");
        Check(everywhere.size() == 3, "anyBin=true should ignore bin_id");
    });

    Test("FilterAudioSources search is a case-insensitive substring match", [] {
        std::vector<LibraryMedia> library{
            MakeMedia("Interview_Take1.mp4", true),
            MakeMedia("broll.mp4", true),
        };
        const auto result =
            FilterAudioSources(library, true, false, {}, "interview");
        Check(result.size() == 1, "expected exactly one match");
        Check(result.front()->filename == "Interview_Take1.mp4",
              "the matching filename should survive the filter");
    });

    Test("FilterAudioSources returns results sorted by filename", [] {
        std::vector<LibraryMedia> library{
            MakeMedia("zebra.mp4", true),
            MakeMedia("apple.mp4", true),
            MakeMedia("mango.mp4", true),
        };
        const auto result = FilterAudioSources(library, true, false, {}, "");
        Check(result.size() == 3, "expected all three entries");
        if (result.size() == 3) {
            Check(result[0]->filename == "apple.mp4",
                  "apple should sort first");
            Check(result[1]->filename == "mango.mp4",
                  "mango should sort second");
            Check(result[2]->filename == "zebra.mp4",
                  "zebra should sort third");
        }
    });

    Test("DirectChildBins exposes only one folder level", [] {
        DocumentBin rootB;
        rootB.name = "B rushes";
        DocumentBin rootA;
        rootA.name = "A interviews";
        DocumentBin nested;
        nested.name = "Nested";
        nested.parent_id = rootA.id;
        const std::vector<DocumentBin> bins{rootB, nested, rootA};

        const auto root = DirectChildBins(bins, {}, "");
        Check(root.size() == 2, "root should contain its two direct bins");
        if (root.size() == 2) {
            Check(root[0]->id == rootA.id, "root bins should sort by name");
            Check(root[1]->id == rootB.id, "root bins should sort by name");
        }
        const auto children = DirectChildBins(bins, rootA.id, "nest");
        Check(children.size() == 1 && children.front()->id == nested.id,
              "a bin should expose its matching direct child");

        const auto tree = BinNavigationTree(bins);
        Check(tree.size() == 3, "navigation should contain every bin");
        if (tree.size() == 3) {
            Check(tree[0].bin->id == rootA.id && tree[0].depth == 0,
                  "first root folder should start at depth zero");
            Check(tree[1].bin->id == nested.id && tree[1].depth == 1,
                  "a child should immediately follow its parent");
            Check(tree[2].bin->id == rootB.id && tree[2].depth == 0,
                  "the next root folder should return to depth zero");
        }
    });

    Test("DescribeCaptionStyle is deterministic and includes every field", [] {
        CaptionStyle style;
        style.font_family = "Helvetica";
        style.font_size = 36;
        style.color = "#00ff00";
        style.position = "top";
        const std::string first = DescribeCaptionStyle(style);
        const std::string second = DescribeCaptionStyle(style);
        Check(first == second, "must be a pure, deterministic function");
        Check(first.find("Helvetica") != std::string::npos,
              "description must mention the font family");
        Check(first.find("36") != std::string::npos,
              "description must mention the font size");
        Check(first.find("top") != std::string::npos,
              "description must mention the position");
        Check(first.find("#00ff00") != std::string::npos,
              "description must mention the color");
    });

    Test("SummarizeCaptionStyles preserves caption_styles order", [] {
        DocumentSequence sequence;
        CaptionStyle first;
        first.font_family = "system";
        CaptionStyle second;
        second.font_family = "Georgia";
        sequence.caption_styles = {first, second};
        const auto summary = SummarizeCaptionStyles(sequence);
        Check(summary.size() == 2, "expected one summary per style");
        if (summary.size() == 2) {
            Check(summary[0].style_id == first.id,
                  "first summary must match the first style");
            Check(summary[1].style_id == second.id,
                  "second summary must match the second style");
        }
    });

    Test("SummarizeCaptionStyles counts clips per caption group correctly", [] {
        DocumentSequence sequence;
        CaptionStyle styleA;
        CaptionStyle styleB;
        sequence.caption_styles = {styleA, styleB};

        DocumentTrack track;
        track.kind = "video";
        DocumentClip clipUsingA1;
        clipUsingA1.caption_group_id = styleA.id;
        DocumentClip clipUsingA2;
        clipUsingA2.caption_group_id = styleA.id;
        DocumentClip clipUsingNone;
        DocumentClip clipUsingB;
        clipUsingB.caption_group_id = styleB.id;
        track.clips = {clipUsingA1, clipUsingA2, clipUsingNone, clipUsingB};
        sequence.tracks = {track};

        const auto summary = SummarizeCaptionStyles(sequence);
        Check(summary.size() == 2, "expected two style summaries");
        if (summary.size() == 2) {
            Check(summary[0].clip_count == 2,
                  "style A should be used by exactly 2 clips");
            Check(summary[1].clip_count == 1,
                  "style B should be used by exactly 1 clip");
        }
    });

    Test("SummarizeCaptionStyles returns zero counts for an unused style", [] {
        DocumentSequence sequence;
        CaptionStyle unused;
        sequence.caption_styles = {unused};
        const auto summary = SummarizeCaptionStyles(sequence);
        Check(summary.size() == 1, "expected one summary");
        if (summary.size() == 1)
            Check(summary.front().clip_count == 0,
                  "an unreferenced style must report zero clips");
    });

    Test("JoinClipToCaptionStyle builds the exact join operation", [] {
        const Ulid clipId = GenerateUlid();
        const Ulid styleId = GenerateUlid();
        const SetClipCaptionOperation op =
            JoinClipToCaptionStyle(clipId, styleId, "Bonjour");
        Check(op.clip_id == clipId, "clip_id must round-trip");
        Check(op.caption_group_id == styleId,
              "caption_group_id must be the target style");
        Check(op.caption_text == "Bonjour", "caption_text must round-trip");
    });

    Test("ClearClipCaption clears both group and text", [] {
        const Ulid clipId = GenerateUlid();
        const SetClipCaptionOperation op = ClearClipCaption(clipId);
        Check(op.clip_id == clipId, "clip_id must round-trip");
        Check(op.caption_group_id.empty(),
              "clearing must leave caption_group_id empty");
        Check(op.caption_text.empty(),
              "clearing must leave caption_text empty");
    });

    return failures == 0 ? 0 : 1;
}
