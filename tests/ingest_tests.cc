#include "Cli.h"
#include "Document.h"
#include "Ingest.h"
#include "Ulid.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
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
    for (char character : path.string()) {
        if (character == '\'') result += "'\\''";
        else result += character;
    }
    return result + "'";
}

std::string Read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream output;
    output << input.rdbuf();
    return output.str();
}

}  // namespace

int main() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / (GenerateUlid() + "-ingest");
    const std::filesystem::path mediaDirectory = root / "media";
    const std::filesystem::path documentPath = root / "document.json";
    const std::filesystem::path rawPath = root / "raw.mp4";
    const std::filesystem::path videoPath = mediaDirectory / "rotated.mp4";
    std::filesystem::create_directories(mediaDirectory);

    Document empty;
    std::string error;
    Check(empty.Save(documentPath.string(), error),
          "empty document saves: " + error);

    const std::string generate =
        Quote(FFMPEG_EXECUTABLE) + " -hide_banner -loglevel error "
        "-f lavfi -i 'color=c=black:s=64x32:r=30000/1001:d=1.001' "
        "-f lavfi -i 'sine=frequency=1000:sample_rate=48000:duration=1.001' "
        "-c:v mpeg4 -c:a aac -shortest " + Quote(rawPath);
    Check(std::system(generate.c_str()) == 0,
          "FFmpeg must generate the media fixture");
    const std::string rotate =
        Quote(FFMPEG_EXECUTABLE) +
        " -hide_banner -loglevel error -display_rotation:v:0 90 -i " +
        Quote(rawPath) + " -c copy " + Quote(videoPath);
    Check(std::system(rotate.c_str()) == 0,
          "FFmpeg must attach a display matrix");

    {
        std::ofstream text(mediaDirectory / "notes.txt");
        text << "not media\n";
    }
    {
        std::ofstream corrupt(mediaDirectory / "corrupt.mp4",
                              std::ios::binary);
        corrupt << "broken mp4";
    }

    std::string output;
    Check(IngestCommand(documentPath.string(), mediaDirectory.string(), false,
                        output) == 0,
          "mixed folder ingest succeeds: " + output);
    Check(output.find("\"added\":1") != std::string::npos &&
              output.find("\"skipped\":2") != std::string::npos,
          "one media is added and two unreadable files are skipped");
    Check(output.find("notes.txt") != std::string::npos &&
              output.find("corrupt.mp4") != std::string::npos,
          "both unreadable files have reported reasons");

    Document ingested;
    Check(Document::Load(documentPath.string(), ingested, error),
          "ingested document loads: " + error);
    Check(ingested.library.size() == 1, "library contains one media");
    if (ingested.library.size() == 1) {
        const LibraryMedia& media = ingested.library[0];
        Check(media.rate.num == 30000 && media.rate.den == 1001,
              "avg_frame_rate remains the exact 30000/1001 rational");
        Check(media.width == 64 && media.height == 32,
              "stored dimensions remain the coded dimensions");
        Check(media.orientation == "portrait",
              "display-matrix rotation controls orientation");
        Check(media.has_audio && media.audio_rate == 48000 &&
                  media.audio_channels == 1,
              "audio header metadata is extracted");
    }

    const std::string beforeSecondIngest = Read(documentPath);
    Check(IngestCommand(documentPath.string(), mediaDirectory.string(), false,
                        output) == 0,
          "second ingest succeeds: " + output);
    Check(output.find("\"added\":0") != std::string::npos &&
              Read(documentPath) == beforeSecondIngest,
          "second ingest is byte-idempotent");

    std::string description;
    Check(DescribeCommand(documentPath.string(), description) == 0,
          "describe succeeds after ingest");
    Check(description.find("{\"timeline\":") == 0 &&
              description.find("\"library\":[") != std::string::npos &&
              description.find("\"alias\":\"M1\"") != std::string::npos &&
              description.find("\"in_use\":false") != std::string::npos,
          "describe separates timeline and library with media aliases");

    const std::string beforeFailedScan = Read(documentPath);
    Check(IngestCommand(documentPath.string(), (root / "missing").string(),
                        false, output) == 1 &&
              Read(documentPath) == beforeFailedScan,
          "global scan failure leaves the document byte-identical");

    std::filesystem::remove_all(root);
    if (failures) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }
    std::cout << "All ingest tests passed\n";
    return 0;
}
