#include "Cli.h"
#include "Document.h"
#include "Export.h"
#include "ProjectStorage.h"
#include "Ulid.h"

extern "C" {
#include <libavformat/avformat.h>
}

#include <cstdlib>
#include <filesystem>
#include <fstream>
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

std::string Quote(const std::string& value) {
    std::string result = "'";
    for (char character : value) {
        if (character == '\'')
            result += "'\\''";
        else
            result += character;
    }
    return result + "'";
}

size_t Count(const std::string& text, const std::string& needle) {
    size_t count = 0;
    size_t offset = 0;
    while ((offset = text.find(needle, offset)) != std::string::npos) {
        ++count;
        offset += needle.size();
    }
    return count;
}

Document Fixture(const std::string& mediaPath) {
    Document document;
    const Ulid sourceId = "01K40000000000000000000001";
    LibraryMedia media;
    media.id = sourceId;
    media.path = mediaPath;
    media.filename = "source.mp4";
    media.codec = "h264";
    media.width = 320;
    media.height = 180;
    media.pixel_format = "yuv420p";
    media.color_range = "tv";
    media.color_space = "bt709";
    media.color_transfer = "bt709";
    media.color_primaries = "bt709";
    media.rate = {25, 1};
    media.duration = {25, 25};
    media.orientation = "landscape";
    media.has_audio = true;
    media.audio_rate = 48000;
    media.audio_channels = 2;
    document.library = {media};
    document.sources = {{sourceId, mediaPath, {25, 1}, {25, 25}}};
    document.sequence.tracks = {
        {"01K40000000000000000000002",
         "video",
         0,
         {{"01K40000000000000000000003",
           sourceId,
           {0, 25},
           {10, 25},
           {5, 25},
           false}}},
        {"01K40000000000000000000004",
         "video",
         1,
         {{"01K40000000000000000000005",
           sourceId,
           {10, 25},
           {5, 25},
           {10, 25},
           false}}},
        {"01K40000000000000000000006",
         "audio",
         2,
         {{"01K40000000000000000000007",
           sourceId,
           {0, 25},
           {10, 25},
           {5, 25},
           true}}},
    };
    return document;
}

}  // namespace

int main() {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        (GenerateUlid() + "-export-tests");
    std::filesystem::create_directories(directory);
    const std::filesystem::path media = directory / "source.mp4";
    const std::filesystem::path project = directory / "project.json";
    const std::filesystem::path destination = directory / "result.mp4";

    const std::string generate =
        Quote(FFMPEG_EXECUTABLE) +
        " -hide_banner -loglevel error -f lavfi -i "
        "'testsrc2=size=320x180:rate=25:duration=1' -f lavfi -i "
        "'sine=frequency=440:sample_rate=48000:duration=1' -c:v libx264 "
        "-pix_fmt yuv420p -c:a aac -ac 2 -shortest -y " +
        Quote(media.string());
    Check(std::system(generate.c_str()) == 0, "source fixture is generated");

    Document document = Fixture(media.string());
    document.sequence.tracks[0].clips[0].opacity = {1, 2};
    std::string error;
    Check(document.Save(project.string(), error), "project saves: " + error);

    ExportSettings settings =
        Exporter::HevcMain10SoftwarePreset(destination.string());
    settings.width = 320;
    settings.height = 180;
    settings.frame_rate = {25, 1};
    settings.ffmpeg_path = FFMPEG_EXECUTABLE;
    ExportPlan plan;
    Check(
        Exporter::BuildPlan(document, project.string(), settings, plan, error),
        "export plan builds: " + error);
    Check(plan.total_frames == 15,
          "timeline duration is converted to an exact frame count");
    Check(plan.filter_graph.find("overlay=") != std::string::npos &&
              plan.filter_graph.find("amix=inputs=1") != std::string::npos,
          "plan contains video composition and audio mixing");
    Check(plan.filter_graph.find("setpts=PTS-STARTPTS+(5/25)/TB") !=
              std::string::npos,
          "clip placement remains rational in the filter graph");
    Check(plan.filter_graph.find("colorchannelmixer=aa=(1/2)") !=
              std::string::npos,
          "opaque export composites the clip's exact opacity");

    Document outputStateDocument = Fixture(media.string());
    outputStateDocument.sequence.tracks[1].visible = false;
    outputStateDocument.sequence.tracks[2].muted = true;
    ExportPlan outputStatePlan;
    Check(Exporter::BuildPlan(outputStateDocument, project.string(), settings,
                              outputStatePlan, error) &&
              Count(outputStatePlan.filter_graph, "overlay=") == 1 &&
              outputStatePlan.filter_graph.find("amix=") == std::string::npos,
          "export honors video visibility and audio mute state");

    Document transitionDocument = Fixture(media.string());
    DocumentClip incoming{"01K40000000000000000000008",
                          transitionDocument.sources[0].id,
                          {10, 25},
                          {5, 25},
                          {15, 25},
                          false};
    transitionDocument.sequence.tracks[0].clips.push_back(incoming);
    transitionDocument.sequence.transitions = {{
        "01K40000000000000000000009",
        transitionDocument.sequence.tracks[0].id,
        transitionDocument.sequence.tracks[0].clips[0].id,
        incoming.id,
        "cross_dissolve",
        {4, 25},
        TransitionAlignment::Center,
    }};
    ExportPlan transitionPlan;
    Check(Exporter::BuildPlan(transitionDocument, project.string(), settings,
                              transitionPlan, error) &&
              transitionPlan.filter_graph.find(
                  "fade=t=in:st=0:d=(4/25):alpha=1") != std::string::npos &&
              transitionPlan.filter_graph.find(
                  "setpts=PTS-STARTPTS+(13/25)/TB") != std::string::npos,
          "export extends video handles and applies the same cross dissolve");

    Document legacyFlagOnly = Fixture(media.string());
    legacyFlagOnly.sequence.tracks.pop_back();
    legacyFlagOnly.sequence.tracks[0].clips[0].include_audio = true;
    ExportPlan silentPlan;
    Check(Exporter::BuildPlan(legacyFlagOnly, project.string(), settings,
                              silentPlan, error) &&
              silentPlan.filter_graph.find("amix=") == std::string::npos,
          "video tracks never contribute embedded audio to export");

    Check(Exporter::Presets().size() == 4,
          "the export UI is backed by the core preset catalog");
    const ExportSettings master = Exporter::SettingsForPreset(
        ExportPresetId::HevcMaster, destination.string(), 3840, 2160,
        {30000, 1001});
    Check(master.width == 3840 && master.height == 2160 &&
              master.frame_rate.num == 30000 && master.frame_rate.den == 1001 &&
              master.encoder == ExportEncoder::HevcSoftware,
          "master preset follows source geometry and selects x265");
    const ExportSettings web = Exporter::SettingsForPreset(
        ExportPresetId::HevcWeb1080, destination.string(), 3840, 2160, {25, 1});
    Check(web.width == 1920 && web.height == 1080 &&
              web.video_bitrate == 20000000,
          "web preset fixes its delivery resolution and bitrate");

    int progressCalls = 0;
    std::atomic_bool cancel{false};
    const bool rendered = Exporter::Run(
        plan,
        [&](const ExportProgress& progress) {
            ++progressCalls;
            Check(progress.fraction >= 0.0 && progress.fraction <= 1.0,
                  "progress stays normalized");
        },
        &cancel, error);
    Check(rendered, "HEVC render succeeds: " + error);
    Check(progressCalls > 0, "render reports progress");
    Check(std::filesystem::is_regular_file(destination),
          "render is atomically committed to its destination");
    Check(!std::filesystem::exists(plan.temporary_output_path),
          "temporary output is removed after commit");

    AVFormatContext* format = nullptr;
    Check(avformat_open_input(&format, destination.c_str(), nullptr, nullptr) >=
              0,
          "exported container opens");
    if (format) {
        Check(avformat_find_stream_info(format, nullptr) >= 0,
              "exported streams can be probed");
        bool foundHevc = false;
        bool foundAudio = false;
        for (unsigned int index = 0; index < format->nb_streams; ++index) {
            const AVCodecParameters* parameters =
                format->streams[index]->codecpar;
            foundHevc = foundHevc || parameters->codec_id == AV_CODEC_ID_HEVC;
            foundAudio =
                foundAudio || parameters->codec_type == AVMEDIA_TYPE_AUDIO;
        }
        Check(foundHevc, "export contains an HEVC video stream");
        Check(foundAudio, "export contains an audio stream");
        avformat_close_input(&format);
    }

    ExportSettings invalid = settings;
    invalid.width = 319;
    Check(!Exporter::BuildPlan(document, project.string(), invalid, plan,
                               error) &&
              error.find("dimensions") != std::string::npos,
          "odd HEVC dimensions are rejected before rendering");
    document.color_management.enabled = true;
    document.color_management.input_gamut = "sony_sgamut3_cine";
    document.color_management.input_transfer = "sony_slog3";
    document.color_management.input_ycbcr_matrix = "bt709";
    document.color_management.input_range = "full";
    document.color_management.output_gamut = "rec2020";
    document.color_management.output_transfer = "hlg";
    ExportSettings colorSettings = settings;
    colorSettings.output_path = (directory / "hlg.mp4").string();
    ExportPlan colorPlan;
    Check(Exporter::BuildPlan(document, project.string(), colorSettings,
                              colorPlan, error),
          "S-Log3 to HLG export plan builds: " + error);
    Check(!colorPlan.color_lut.empty() &&
              colorPlan.filter_graph.find("lut3d=") != std::string::npos,
          "color-managed export carries its display transform LUT");
    Check(Exporter::Run(colorPlan, {}, &cancel, error),
          "S-Log3 to HLG render succeeds: " + error);
    Check(!std::filesystem::exists(colorPlan.temporary_lut_path),
          "temporary color transform is removed after rendering");
    AVFormatContext* hlgFormat = nullptr;
    Check(avformat_open_input(&hlgFormat, colorSettings.output_path.c_str(),
                              nullptr, nullptr) >= 0 &&
              avformat_find_stream_info(hlgFormat, nullptr) >= 0,
          "HLG export can be probed");
    if (hlgFormat) {
        const AVCodecParameters* video = nullptr;
        for (unsigned int index = 0; index < hlgFormat->nb_streams; ++index)
            if (hlgFormat->streams[index]->codecpar->codec_type ==
                AVMEDIA_TYPE_VIDEO)
                video = hlgFormat->streams[index]->codecpar;
        Check(video && video->color_primaries == AVCOL_PRI_BT2020 &&
                  video->color_trc == AVCOL_TRC_ARIB_STD_B67 &&
                  video->color_space == AVCOL_SPC_BT2020_NCL,
              "HEVC stream is tagged Rec.2020 HLG");
        avformat_close_input(&hlgFormat);
    }
    std::string commandOutput;
    std::string headlessProjectPath;
    Check(CreatePortableProject(
              (directory / "Headless.cutmachine-project").string(),
              Project::FromDocument(document, "Headless export"),
              headlessProjectPath, error),
          "headless export project package saves: " + error);
    Check(ExportCommand(headlessProjectPath, invalid, {}, nullptr,
                        commandOutput) == 1 &&
              commandOutput.find("\"error\":\"InvalidExport\"") !=
                  std::string::npos,
          "headless export returns a structured validation error");

    std::filesystem::remove_all(directory);
    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }
    std::cout << "All export tests passed\n";
    return 0;
}
