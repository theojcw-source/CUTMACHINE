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
    const Ulid unusedSourceId = "01KA0000000000000000000006";
    document.sources = {
        {sourceId, "tone.mp4", {25, 1}, {25, 25}},
        {unusedSourceId, "tone.mp4", {25, 1}, {25, 25}},
    };
    document.sequence.tracks = {
        {"01KA0000000000000000000002",
         "video",
         0,
         {{"01KA0000000000000000000003",
           sourceId,
           {0, 25},
           {25, 25},
           {0, 25},
           true}}},
    };
    AudioPlayback playback;
    std::string error;
    const bool opened = playback.Open(document, root.string(), error);
    const bool unusedBeforeInsert =
        playback.DecodedSourceCount() == 0 && playback.PlannedClipCount() == 0;
    document.sequence.tracks.push_back({"01KA0000000000000000000004",
                                        "audio",
                                        1,
                                        {{"01KA0000000000000000000005",
                                          sourceId,
                                          {0, 25},
                                          {25, 25},
                                          {0, 25},
                                          true}}});
    playback.RebuildTimeline(document);
    const bool decoded = playback.DecodedSourceCount() == 1;
    const bool planned = playback.PlannedClipCount() == 1;
    document.sequence.tracks[1].muted = true;
    playback.RebuildTimeline(document);
    const bool muteSuppressesTrack = playback.PlannedClipCount() == 0;
    document.sequence.tracks[1].muted = false;
    DocumentTrack secondAudio = document.sequence.tracks[1];
    secondAudio.id = "01KA0000000000000000000007";
    secondAudio.index = 2;
    secondAudio.clips[0].id = "01KA0000000000000000000008";
    document.sequence.tracks.push_back(secondAudio);
    document.sequence.tracks[1].solo = true;
    playback.RebuildTimeline(document);
    const bool soloSuppressesOtherTracks = playback.PlannedClipCount() == 1;
    document.sequence.tracks[1].solo = false;
    playback.RebuildTimeline(document);
    const bool allUnmutedTracksMix = playback.PlannedClipCount() == 2;
    const bool started = opened && playback.PlayFrom({0, 25}, 2, error);
    const bool shuttleSpeedPreserved = playback.ShuttleSpeed() == 2;
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
    if (!opened || !unusedBeforeInsert || !decoded || !planned ||
        !muteSuppressesTrack || !soloSuppressesOtherTracks ||
        !allUnmutedTracksMix || !started || !shuttleSpeedPreserved ||
        !scrubbed || !sameFrameSuppressed || !nextFrameTriggered) {
        std::cerr << "FAIL: audio decode/mix plan: " << error << '\n';
        return 1;
    }
    std::cout << "PASS: audio source decodes and enters the timeline mix\n";
    return 0;
}
