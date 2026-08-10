#include "AudioPlayback.h"
#include "TimelineView.h"
#include "Ulid.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

std::string Quote(const std::filesystem::path& path) {
    std::string result = "'";
    for (char character : path.string())
        result += character == '\'' ? "'\\''" : std::string(1, character);
    return result + "'";
}

}  // namespace

int main() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
                                       (GenerateUlid() + "-audio-playback");
    const std::filesystem::path media = root / "tone.mp4";
    std::filesystem::create_directories(root);
    const std::string generate =
        Quote(FFMPEG_EXECUTABLE) +
        " -hide_banner -loglevel error "
        "-f lavfi -i 'color=c=black:s=32x32:r=25:d=1' "
        "-f lavfi -i 'sine=frequency=440:sample_rate=48000:duration=1' "
        "-c:v mpeg4 -c:a aac -shortest " +
        Quote(media);
    if (std::system(generate.c_str()) != 0) {
        std::cerr << "FAIL: unable to generate audio fixture\n";
        return 1;
    }

    Document document;
    const Ulid sourceId = "01KA0000000000000000000001";
    document.sources = {{sourceId, "tone.mp4", {25, 1}, {25, 25}}};
    document.tracks = {
        {"01KA0000000000000000000002",
         "video",
         0,
         {{"01KA0000000000000000000003",
           sourceId,
           {0, 25},
           {25, 25},
           {0, 25},
           false}}},
        {"01KA0000000000000000000004",
         "audio",
         1,
         {{"01KA0000000000000000000005",
           sourceId,
           {0, 25},
           {25, 25},
           {0, 25},
           true}}},
    };
    AudioPlayback playback;
    std::string error;
    const bool opened = playback.Open(document, root.string(), error);
    const bool decoded = playback.DecodedSourceCount() == 1;
    const bool planned = playback.PlannedClipCount() == 1;
    const bool started = opened && playback.PlayFrom({0, 25}, 1, error);
    playback.Stop();
    const RationalTime firstMousePosition = QuantizePlayheadPosition(
        {121, 250}, PlayheadResolution::Frame, {25, 1});
    const RationalTime sameFrameMousePosition = QuantizePlayheadPosition(
        {124, 250}, PlayheadResolution::Frame, {25, 1});
    const RationalTime nextFrameMousePosition = QuantizePlayheadPosition(
        {126, 250}, PlayheadResolution::Frame, {25, 1});
    const bool scrubbed = opened && playback.ScrubAt(firstMousePosition, error);
    const bool sameFrameSuppressed =
        firstMousePosition == sameFrameMousePosition &&
        playback.ScrubAt(sameFrameMousePosition, error) &&
        playback.ScrubTriggerCount() == 1;
    const bool nextFrameTriggered =
        nextFrameMousePosition != firstMousePosition &&
        playback.ScrubAt(nextFrameMousePosition, error) &&
        playback.ScrubTriggerCount() == 2;
    playback.Stop();
    std::filesystem::remove_all(root);
    if (!opened || !decoded || !planned || !started || !scrubbed ||
        !sameFrameSuppressed || !nextFrameTriggered) {
        std::cerr << "FAIL: audio decode/mix plan: " << error << '\n';
        return 1;
    }
    std::cout << "PASS: audio source decodes and enters the timeline mix\n";
    return 0;
}
