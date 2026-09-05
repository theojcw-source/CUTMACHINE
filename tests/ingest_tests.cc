#include "Cli.h"
#include "Document.h"
#include "Ingest.h"
#include "MediaSource.h"
#include "Operations.h"
#include "ProjectStorage.h"
#include "Ulid.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
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
        if (character == '\'')
            result += "'\\''";
        else
            result += character;
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
    const std::filesystem::path rawPath = root / "raw.mp4";
    const std::filesystem::path variableRatePath = root / "variable-rate.mp4";
    const std::filesystem::path alphaPath = root / "transparent.mov";
    const std::filesystem::path audioPath = root / "voice.wav";
    const std::filesystem::path videoPath = mediaDirectory / "rotated.mp4";
    std::filesystem::create_directories(mediaDirectory);

    std::string error;
    std::string projectPath;
    Check(CreatePortableProject((root / "Ingest.cutmachine-project").string(),
                                Project("Ingest"), projectPath, error),
          "empty project package saves: " + error);
    const std::filesystem::path documentPath = projectPath;

    const std::string generate =
        Quote(FFMPEG_EXECUTABLE) +
        " -hide_banner -loglevel error "
        "-f lavfi -i 'color=c=black:s=64x32:r=30000/1001:d=1.001' "
        "-f lavfi -i 'sine=frequency=1000:sample_rate=48000:duration=1.001' "
        "-c:v mpeg4 -c:a aac -shortest " +
        Quote(rawPath);
    Check(std::system(generate.c_str()) == 0,
          "FFmpeg must generate the media fixture");
    const std::string rotate =
        Quote(FFMPEG_EXECUTABLE) +
        " -hide_banner -loglevel error -display_rotation:v:0 90 -i " +
        Quote(rawPath) + " -c copy " + Quote(videoPath);
    Check(std::system(rotate.c_str()) == 0,
          "FFmpeg must attach a display matrix");
    const std::string generateVariableRate =
        Quote(FFMPEG_EXECUTABLE) +
        " -hide_banner -loglevel error "
        "-f lavfi -i 'testsrc2=size=64x32:rate=30:duration=1' "
        "-vf \"select='not(eq(n,15))'\" -fps_mode vfr -c:v mpeg4 " +
        Quote(variableRatePath);
    Check(std::system(generateVariableRate.c_str()) == 0,
          "FFmpeg must generate the variable-rate media fixture");
    const std::string generateAlpha =
        Quote(FFMPEG_EXECUTABLE) +
        " -hide_banner -loglevel error -f lavfi "
        "-i 'color=c=red@0.5:s=16x16:r=25:d=0.2,format=rgba' "
        "-c:v qtrle -pix_fmt argb " +
        Quote(alphaPath);
    Check(std::system(generateAlpha.c_str()) == 0,
          "FFmpeg must generate the transparent video fixture");
    const std::string generateAudio =
        Quote(FFMPEG_EXECUTABLE) +
        " -hide_banner -loglevel error "
        "-f lavfi -i 'sine=frequency=440:sample_rate=48000:duration=1' "
        "-c:a pcm_s16le " +
        Quote(audioPath);
    Check(std::system(generateAudio.c_str()) == 0,
          "FFmpeg must generate the audio-only fixture");
    MediaSource alphaSource;
    Check(alphaSource.Open(alphaPath.string()),
          "transparent video decode source opens");
    const AVFrame* alphaFrame = nullptr;
    int64_t alphaPts = 0;
    Check(alphaSource.DecodeFrame(0, alphaFrame, alphaPts) && alphaFrame,
          "transparent video frame decodes");
    if (alphaFrame) {
        const AVPixFmtDescriptor* pixel =
            av_pix_fmt_desc_get(static_cast<AVPixelFormat>(alphaFrame->format));
        Check(pixel && pixel->nb_components == 4 &&
                  (pixel->flags & AV_PIX_FMT_FLAG_ALPHA) &&
                  pixel->comp[0].plane != pixel->comp[1].plane &&
                  pixel->comp[0].plane != pixel->comp[2].plane &&
                  pixel->comp[3].plane != pixel->comp[0].plane,
              "packed source alpha is normalized to four Metal-ready planes");
        if (pixel && pixel->nb_components == 4) {
            const int alphaPlane = pixel->comp[3].plane;
            const uint16_t alpha = reinterpret_cast<const uint16_t*>(
                alphaFrame->data[alphaPlane])[0];
            Check(alpha > 30000 && alpha < 36000,
                  "the normalized alpha plane preserves 50 percent coverage");
        }
    }
    LibraryMedia variableRateMetadata;
    Check(ProbeMediaMetadata(variableRatePath.string(), variableRateMetadata,
                             error),
          "variable-rate metadata probe succeeds: " + error);
    MediaSource variableRateSource;
    Check(variableRateSource.Open(variableRatePath.string()),
          "variable-rate decode source opens");
    Check(variableRateMetadata.rate.num ==
                  variableRateSource.FrameRateNumerator() &&
              variableRateMetadata.rate.den ==
                  variableRateSource.FrameRateDenominator(),
          "ingest and decode retain the same exact variable frame rate");

    {
        std::ofstream text(mediaDirectory / "notes.txt");
        text << "not media\n";
    }
    {
        std::ofstream corrupt(mediaDirectory / "corrupt.mp4", std::ios::binary);
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

    Project ingestedProject;
    Check(LoadStoredProject(documentPath.string(), ingestedProject, error),
          "ingested project loads: " + error);
    Document ingested = ingestedProject.MakeActiveDocument();
    Check(ingested.library.size() == 1, "library contains one media");
    if (ingested.library.size() == 1) {
        const LibraryMedia& media = ingested.library[0];
        Check(media.rate.num == 30000 && media.rate.den == 1001,
              "avg_frame_rate remains the exact 30000/1001 rational");
        Check(media.width == 64 && media.height == 32,
              "stored dimensions remain the coded dimensions");
        Check(!media.pixel_format.empty() && !media.color_range.empty() &&
                  !media.color_space.empty() && !media.color_transfer.empty() &&
                  !media.color_primaries.empty(),
              "pixel format and color signalling flow through ingest");
        Check(media.orientation == "portrait",
              "display-matrix rotation controls orientation");
        Check(std::abs(media.rotation_degrees) == 90,
              "display-matrix rotation is retained for presentation");
        Check(media.has_audio && media.audio_rate == 48000 &&
                  media.audio_channels == 1,
              "audio header metadata is extracted");
        // QC-2026-09 A3 -- FFmpeg's own volumedetect reads this fixture's
        // AAC audio at -21.1 dBFS, which is 88000 on the kAudioLevelScale
        // grid. The band below is that figure with room for the mono downmix
        // and for a different encoder build, and is still far from anything
        // else the measurement could mean. What the ingest has to get right
        // is the order of magnitude, because that is what decides whether
        // Whisper runs.
        Check(media.audio_level_measured,
              "ingest measures the mean audio level as a document fact");
        Check(media.audio_level > 50000 && media.audio_level < 150000,
              "the measured level lands where FFmpeg's own reading of the "
              "same audio does: " +
                  std::to_string(media.audio_level));
        Check(media.audio_level >= kSilentMediaAudioLevel,
              "and is not mistaken for a mute cutaway");
        const DocumentSource* source = ingested.FindSource(media.id);
        Check(source && source->rate.num == media.rate.num &&
                  source->rate.den == media.rate.den &&
                  source->duration == media.duration,
              "ingest creates a source with the same stable media ULID");
        ingested.sequence.tracks.push_back(
            {"01K82000000000000000000001", "video", 0, {}});
        Operation insert = InsertClipOperation{ingested.sequence.tracks[0].id,
                                               media.id,
                                               {0, media.duration.rate},
                                               media.duration,
                                               {0, media.duration.rate},
                                               {},
                                               {}};
        Operation inverse = RemoveClipOperation{};
        EditError editError = EditError::None;
        std::string editMessage;
        Check(ApplyOperation(ingested, insert, inverse, editError, editMessage),
              "an ingested media can be inserted directly: " + editMessage);
    }

    // B11 -- a precise file path is an ingest root too, and a sound file is
    // first-class media. Its sample clock becomes the exact source timebase;
    // no synthetic black video is needed.
    Check(IngestCommand(documentPath.string(), audioPath.string(), false,
                        output) == 0,
          "a single audio file ingests directly: " + output);
    Check(output.find("\"added\":1") != std::string::npos &&
              output.find("no video stream") == std::string::npos,
          "audio-only ingest succeeds instead of reporting a video refusal");
    Project audioProject;
    Check(LoadStoredProject(documentPath.string(), audioProject, error),
          "project with audio-only media loads: " + error);
    const auto audioMedia =
        std::find_if(audioProject.rushes.begin(), audioProject.rushes.end(),
                     [&](const LibraryMedia& media) {
                         return media.filename == audioPath.filename().string();
                     });
    Check(audioMedia != audioProject.rushes.end(),
          "audio-only media is present in the library");
    if (audioMedia != audioProject.rushes.end()) {
        Check(!audioMedia->has_video && audioMedia->has_audio,
              "audio-only capability is explicit in the library");
        Check(audioMedia->width == 0 && audioMedia->height == 0 &&
                  audioMedia->orientation == "audio",
              "audio-only media carries no fabricated picture metadata");
        Check(audioMedia->rate.num == 48000 && audioMedia->rate.den == 1 &&
                  audioMedia->duration == RationalTime{48000, 48000},
              "audio samples provide the exact rate and duration");
        const std::string canonical =
            audioProject.MakeActiveDocument().SaveToString();
        Check(canonical.find("\"has_video\":false") != std::string::npos,
              "canonical library JSON exposes has_video false");

        Document audioDocument = audioProject.MakeActiveDocument();
        audioDocument.sequence.tracks.push_back(
            {"01K83000000000000000000001", "audio", 0, {}});
        Operation audioInsert =
            InsertClipOperation{audioDocument.sequence.tracks.back().id,
                                audioMedia->id,
                                {0, 48000},
                                audioMedia->duration,
                                {0, 48000},
                                {},
                                {}};
        Operation audioInverse = RemoveClipOperation{};
        EditError audioEditError = EditError::None;
        std::string audioEditMessage;
        Check(
            ApplyOperation(audioDocument, audioInsert, audioInverse,
                           audioEditError, audioEditMessage),
            "audio-only media inserts on an audio track: " + audioEditMessage);
        audioDocument.sequence.tracks.push_back(
            {"01K83000000000000000000002", "video", 1, {}});
        Operation videoInsert =
            InsertClipOperation{audioDocument.sequence.tracks.back().id,
                                audioMedia->id,
                                {0, 48000},
                                audioMedia->duration,
                                {0, 48000},
                                {},
                                {}};
        Check(!ApplyOperation(audioDocument, videoInsert, audioInverse,
                              audioEditError, audioEditMessage) &&
                  audioEditError == EditError::InvalidOperation,
              "audio-only media is refused explicitly on a video track");

        std::string qualityOutput;
        Check(AnalyzeShotQualityCommand(documentPath.string(), audioMedia->id,
                                        qualityOutput) == 1 &&
                  qualityOutput.find("\"error\":\"InvalidOperation\"") !=
                      std::string::npos &&
                  qualityOutput.find("audio-only") != std::string::npos,
              "shot-quality refuses audio-only media before decoding");
    }

    // QC-2026-09 A3 -- the case the ticket is about: a mute cutaway. 29 of one
    // project's 71 rushes were this, and each went to Whisper at eleven times
    // its own runtime before anyone noticed.
    {
        const std::filesystem::path silentPath = root / "silent.mp4";
        const std::string generateSilent =
            Quote(FFMPEG_EXECUTABLE) +
            " -hide_banner -loglevel error "
            "-f lavfi -i 'color=c=black:s=64x32:r=25:d=1' "
            "-f lavfi -i 'anullsrc=sample_rate=48000:channel_layout=mono' "
            "-c:v mpeg4 -c:a aac -shortest " +
            Quote(silentPath);
        Check(std::system(generateSilent.c_str()) == 0,
              "FFmpeg must generate the mute fixture");
        int64_t level = -1;
        std::string levelReason;
        Check(MeasureMediaAudioLevel(silentPath.string(), FFMPEG_EXECUTABLE,
                                     level, levelReason),
              "a mute media still measures: " + levelReason);
        Check(level < kSilentMediaAudioLevel,
              "digital silence falls under the threshold that skips Whisper: " +
                  std::to_string(level));

        // A media with no audio stream at all is a refusal with a reason, not
        // a zero a caller would read as "measured, and silent".
        int64_t missing = -1;
        Check(!MeasureMediaAudioLevel(variableRatePath.string(),
                                      FFMPEG_EXECUTABLE, missing, levelReason),
              "a media without an audio stream reports that it cannot be "
              "measured");
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
    Check(description.find("{\"sequence\":") == 0 &&
              description.find("\"timeline\":") != std::string::npos &&
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
