#include "MediaTaskManager.h"
#include "Thumbnail.h"
#include "Ulid.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>

extern "C" {
#include <libavformat/avformat.h>
}

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
                                       (GenerateUlid() + "-thumbnail");
    const std::filesystem::path source = root / "portrait.mp4";
    const std::filesystem::path thumbnail = root / "cache" / "preview.png";
    std::filesystem::create_directories(root);
    const std::string generate = Quote(FFMPEG_EXECUTABLE) +
                                 " -hide_banner -loglevel error -f lavfi -i "
                                 "'testsrc2=size=180x320:rate=25:duration=0.5' "
                                 "-c:v libx264 -pix_fmt yuv420p -y " +
                                 Quote(source);
    if (std::system(generate.c_str()) != 0) {
        std::cerr << "FAIL: unable to generate thumbnail fixture\n";
        return 1;
    }

    MediaTaskManager manager;
    ThumbnailSettings settings;
    settings.ffmpeg_path = FFMPEG_EXECUTABLE;
    manager.Enqueue(MediaTaskKind::Thumbnail, "thumbnail fixture",
                    [&](MediaTaskContext& context, std::string& error) {
                        return GenerateMediaThumbnail(
                            source.string(), thumbnail.string(), {1, 2},
                            settings, context, error);
                    });
    if (!manager.WaitForIdle(30000)) {
        std::cerr << "FAIL: thumbnail task timed out\n";
        return 1;
    }
    std::string error;
    AVFormatContext* format = nullptr;
    bool probed = avformat_open_input(&format, thumbnail.c_str(), nullptr,
                                      nullptr) >= 0 &&
                  avformat_find_stream_info(format, nullptr) >= 0;
    int width = 0;
    int height = 0;
    if (probed) {
        for (unsigned index = 0; index < format->nb_streams; ++index) {
            const AVCodecParameters* parameters =
                format->streams[index]->codecpar;
            if (parameters->codec_type == AVMEDIA_TYPE_VIDEO) {
                width = parameters->width;
                height = parameters->height;
                break;
            }
        }
    }
    if (format) avformat_close_input(&format);
    const auto tasks = manager.Snapshot();
    const bool ok = tasks.size() == 1 &&
                    tasks[0].state == MediaTaskState::Succeeded && probed &&
                    width == 320 && height == 180;
    std::filesystem::remove_all(root);
    if (!ok) {
        std::cerr << "FAIL: invalid thumbnail: " << error << '\n';
        return 1;
    }
    std::cout << "PASS: normalized video thumbnail generated\n";
    return 0;
}
