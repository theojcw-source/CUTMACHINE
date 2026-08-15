#include "MediaTaskManager.h"
#include "Ulid.h"
#include "Waveform.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace {

std::string Quote(const std::filesystem::path& path) {
    std::string result = "'";
    for (char character : path.string())
        result += character == '\'' ? "'\\''" : std::string(1, character);
    return result + "'";
}

}  // namespace

int main() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / (GenerateUlid() + "-waveform");
    const std::filesystem::path source = root / "source.m4a";
    const std::filesystem::path cache = root / "cache" / "source.waveform";
    std::filesystem::create_directories(root);
    const std::string generate =
        Quote(FFMPEG_EXECUTABLE) +
        " -hide_banner -loglevel error -f lavfi -i "
        "'sine=frequency=440:sample_rate=48000:duration=1' -c:a aac -y " +
        Quote(source);
    if (std::system(generate.c_str()) != 0) {
        std::cerr << "FAIL: unable to generate waveform fixture\n";
        return 1;
    }

    MediaTaskManager manager;
    WaveformSettings settings;
    settings.ffmpeg_path = FFMPEG_EXECUTABLE;
    manager.Enqueue(MediaTaskKind::Waveform, "waveform fixture",
                    [&](MediaTaskContext& context, std::string& error) {
                        return GenerateAudioWaveform(source.string(),
                                                     cache.string(), {1, 1},
                                                     settings, context, error);
                    });
    if (!manager.WaitForIdle(30000)) {
        std::cerr << "FAIL: waveform task timed out\n";
        return 1;
    }
    AudioWaveform waveform;
    std::string error;
    const bool loaded = LoadAudioWaveform(cache.string(), waveform, error);
    const auto tasks = manager.Snapshot();
    const float maximum =
        waveform.peaks.empty()
            ? 0.0f
            : *std::max_element(waveform.peaks.begin(), waveform.peaks.end());
    const bool ok =
        tasks.size() == 1 && tasks[0].state == MediaTaskState::Succeeded &&
        loaded && waveform.peaks_per_second == 100 &&
        waveform.peaks.size() >= 95 && waveform.peaks.size() <= 105 &&
        maximum > 0.01f && maximum <= 1.0f;
    std::filesystem::remove_all(root);
    if (!ok) {
        std::cerr << "FAIL: invalid waveform cache: " << error << '\n';
        return 1;
    }
    std::cout << "PASS: audio waveform generated and loaded\n";
    return 0;
}
