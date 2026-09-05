// QC-2026-09 (A6) -- selection stays pure and exact; a small generated media
// fixture proves the delivered JPEG crosses the document colour pipeline.

#include "TimelineSheets.h"

#include <cstdlib>
#include <filesystem>
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

std::string Quote(const std::filesystem::path& path) {
    std::string result = "'";
    for (char character : path.string())
        result += character == '\'' ? "'\\''" : std::string(1, character);
    return result + "'";
}

Document Fixture() {
    Document document;
    document.sequence.frame_rate = {25, 1};
    document.sources = {
        {"01K30000000000000000000001", "a.mp4", {25, 1}, {1000, 25}},
        {"01K30000000000000000000002", "b.mp4", {25, 1}, {1000, 25}},
        {"01K30000000000000000000003", "c.mp4", {25, 1}, {1000, 25}},
        {"01K30000000000000000000004", "x.mp4", {25, 1}, {1000, 25}},
    };
    document.sequence.tracks = {
        {"01K30000000000000000000010",
         "video",
         0,
         {{"01K30000000000000000000011",
           "01K30000000000000000000001",
           {100, 25},
           {10, 25},
           {0, 25}},
          {"01K30000000000000000000012",
           "01K30000000000000000000002",
           {200, 25},
           {10, 25},
           {10, 25}},
          {"01K30000000000000000000013",
           "01K30000000000000000000003",
           {400, 25},
           {10, 25},
           {20, 25}}}},
        {"01K30000000000000000000020",
         "video",
         1,
         {{"01K30000000000000000000021",
           "01K30000000000000000000004",
           {300, 25},
           {10, 25},
           {10, 25}}}},
    };
    return document;
}

}  // namespace

int main() {
    TimelineSheetSettings settings;
    TimelineSheetPlan plan;
    std::string error;
    const Document document = Fixture();

    Check(BuildTimelineSheetPlan(document, TimelineSheetKind::Contact, settings,
                                 plan, error),
          "contact plan builds: " + error);
    Check(plan.total_candidates == 3 && plan.frames.size() == 3,
          "contact sheet contains only the three composited shots");
    Check(plan.frames.size() == 3 &&
              plan.frames[0].timeline_position == RationalTime{5, 25} &&
              plan.frames[0].source_position == RationalTime{105, 25} &&
              plan.frames[1].clip_id == "01K30000000000000000000021" &&
              plan.frames[1].source_position == RationalTime{305, 25} &&
              plan.frames[2].source_position == RationalTime{405, 25},
          "contact cells are exact middle frames of visible segments");

    settings.maximum_images = 2;
    Check(BuildTimelineSheetPlan(document, TimelineSheetKind::Contact, settings,
                                 plan, error) &&
              plan.frames.size() == 2 &&
              plan.frames.front().clip_id == "01K30000000000000000000011" &&
              plan.frames.back().clip_id == "01K30000000000000000000013",
          "bounded contact sheets sample the whole timeline");

    settings.maximum_images = 24;
    Check(BuildTimelineSheetPlan(document, TimelineSheetKind::Cuts, settings,
                                 plan, error),
          "cut plan builds: " + error);
    Check(plan.total_candidates == 2 && plan.frames.size() == 4,
          "cut sheet keeps two complete visible-cut pairs");
    Check(plan.frames.size() == 4 && plan.frames[0].role == "before" &&
              plan.frames[0].timeline_position == RationalTime{9, 25} &&
              plan.frames[0].source_position == RationalTime{109, 25} &&
              plan.frames[1].role == "after" &&
              plan.frames[1].timeline_position == RationalTime{10, 25} &&
              plan.frames[1].source_position == RationalTime{300, 25} &&
              plan.frames[2].source_position == RationalTime{309, 25} &&
              plan.frames[3].source_position == RationalTime{400, 25},
          "each cut uses the exact sequence frames bracketing it");
    Check(
        SerializeTimelineSheetPlan(plan).find(
            "\"cut_position\":{\"value\":10,\"rate\":25}") != std::string::npos,
        "sheet metadata keeps exact cut positions");
    settings.maximum_images = 1;
    Check(!BuildTimelineSheetPlan(document, TimelineSheetKind::Cuts, settings,
                                  plan, error),
          "a cut sheet never exceeds its bound with an incomplete pair");
    settings.maximum_images = 24;

    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "cutmachine-sheet-tests";
    std::filesystem::create_directories(root);
    const std::filesystem::path source = root / "grey.mp4";
    const std::string generate =
        Quote(FFMPEG_EXECUTABLE) +
        " -hide_banner -loglevel error -f lavfi -i "
        "'testsrc2=size=160x90:rate=25:duration=1' -c:v libx264 "
        "-pix_fmt yuv420p -y " +
        Quote(source);
    Check(std::system(generate.c_str()) == 0, "colour fixture is generated");

    Document renderDocument;
    LibraryMedia media;
    media.id = "01K31000000000000000000001";
    media.path = source.filename().string();
    media.filename = source.filename().string();
    media.rate = {25, 1};
    media.duration = {25, 25};
    renderDocument.library.push_back(media);
    renderDocument.sources.push_back(
        {media.id, media.path, media.rate, media.duration});
    renderDocument.sequence.tracks = {
        {"01K31000000000000000000002",
         "video",
         0,
         {{"01K31000000000000000000003",
           media.id,
           {0, 25},
           {25, 25},
           {0, 25}}}},
    };
    settings.ffmpeg_path = FFMPEG_EXECUTABLE;
    settings.cell_dimension = 128;
    Check(BuildTimelineSheetPlan(renderDocument, TimelineSheetKind::Contact,
                                 settings, plan, error),
          "render plan builds: " + error);
    std::string unmanaged;
    Check(RenderTimelineSheet(renderDocument, root, plan, settings, unmanaged,
                              error),
          "unmanaged sheet renders: " + error);
    renderDocument.color_management.enabled = true;
    renderDocument.color_management.input_gamut = "sony_sgamut3_cine";
    renderDocument.color_management.input_transfer = "sony_slog3";
    renderDocument.color_management.input_ycbcr_matrix = "bt709";
    renderDocument.color_management.input_range = "full";
    std::string managed;
    Check(RenderTimelineSheet(renderDocument, root, plan, settings, managed,
                              error),
          "colour-managed sheet renders: " + error);
    Check(managed.size() > 512 && managed != unmanaged,
          "document colour management changes the delivered JPEG bytes");
    Check(managed.size() >= 3 &&
              static_cast<unsigned char>(managed[0]) == 0xff &&
              static_cast<unsigned char>(managed[1]) == 0xd8 &&
              static_cast<unsigned char>(managed[2]) == 0xff,
          "the managed sheet is a JPEG image");

    std::filesystem::remove_all(root);
    if (failures == 0) std::cout << "timeline sheet tests passed\n";
    return failures == 0 ? 0 : 1;
}
