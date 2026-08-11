#include "Ingest.h"
#include "MediaTaskManager.h"
#include "Proxy.h"
#include "Ulid.h"

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
        std::filesystem::temp_directory_path() / (GenerateUlid() + "-proxy");
    const std::filesystem::path source = root / "source.mp4";
    const std::filesystem::path proxy = root / "cache" / "proxy.mov";
    std::filesystem::create_directories(root);
    const std::string generate =
        Quote(FFMPEG_EXECUTABLE) +
        " -hide_banner -loglevel error -f lavfi -i "
        "'testsrc2=size=320x180:rate=25:duration=0.4' -f lavfi -i "
        "'sine=frequency=440:sample_rate=48000:duration=0.4' "
        "-c:v libx264 -pix_fmt yuv420p -c:a aac -shortest -y " +
        Quote(source);
    if (std::system(generate.c_str()) != 0) {
        std::cerr << "FAIL: unable to generate proxy fixture\n";
        return 1;
    }

    MediaTaskManager manager;
    ProxySettings settings;
    settings.max_width = 160;
    settings.ffmpeg_path = FFMPEG_EXECUTABLE;
    manager.Enqueue(MediaTaskKind::Proxy, "proxy fixture",
                    [&](MediaTaskContext& context, std::string& error) {
                        return GenerateMediaProxy(source.string(),
                                                  proxy.string(), {10, 25},
                                                  settings, context, error);
                    });
    if (!manager.WaitForIdle(30000)) {
        std::cerr << "FAIL: proxy task timed out\n";
        return 1;
    }
    const auto tasks = manager.Snapshot();
    LibraryMedia metadata;
    std::string error;
    const bool probed = ProbeMediaMetadata(proxy.string(), metadata, error);
    const bool ok = tasks.size() == 1 &&
                    tasks[0].state == MediaTaskState::Succeeded && probed &&
                    metadata.width == 160 && metadata.height == 90 &&
                    !metadata.has_audio;
    std::filesystem::remove_all(root);
    if (!ok) {
        std::cerr << "FAIL: proxy output invalid: " << error << '\n';
        return 1;
    }
    std::cout << "PASS: silent ProRes proxy generated through media tasks\n";
    return 0;
}
