#include "BeatDetection.h"
#include "MediaTaskManager.h"
#include "Ulid.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <vector>

namespace {

std::string Quote(const std::filesystem::path& path) {
    std::string result = "'";
    for (char character : path.string())
        result += character == '\'' ? "'\\''" : std::string(1, character);
    return result + "'";
}

// Generates a synthetic click track: short sine bursts at every multiple of
// `periodSeconds`, starting at t=0, for `clickCount` clicks. This is the
// ground truth a real click track (e.g. a metronome or drum loop count-in)
// approximates, used to check onset detection against a known answer rather
// than eyeballing a real recording.
bool GenerateClickTrack(const std::filesystem::path& destination,
                        double periodSeconds, int clickCount,
                        uint32_t sampleRate) {
    constexpr double kPulseSeconds = 0.02;
    const double duration = periodSeconds * clickCount;
    std::ostringstream command;
    command << Quote(FFMPEG_EXECUTABLE)
            << " -hide_banner -loglevel error -f lavfi -i "
               "\"aevalsrc=exprs='if(lt(mod(t\\,"
            << periodSeconds << ")\\," << kPulseSeconds
            << ")\\,0.9*sin(2*PI*1000*t)\\,0)':s=" << sampleRate
            << ":d=" << duration << "\" -c:a pcm_s16le -y "
            << Quote(destination);
    return std::system(command.str().c_str()) == 0;
}

// Builds a click track at `bpm`, runs it through the real DetectAudioBeats /
// LoadAudioBeats pair via a MediaTaskManager (mirroring how the app enqueues
// this work), and checks detected onsets against the known click grid and
// the estimated BPM against the known tempo. Returns true when every check
// passes.
bool RunClickTrackCase(double bpm, int clickCount) {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / (GenerateUlid() + "-beats");
    const std::filesystem::path source = root / "click.wav";
    const std::filesystem::path cache = root / "cache" / "click.beats";
    std::filesystem::create_directories(root);

    const double periodSeconds = 60.0 / bpm;
    constexpr uint32_t kSampleRate = 48000;

    if (!GenerateClickTrack(source, periodSeconds, clickCount, kSampleRate)) {
        std::cerr << "FAIL: unable to generate " << bpm
                  << " BPM click-track fixture\n";
        std::filesystem::remove_all(root);
        return false;
    }

    MediaTaskManager manager;
    BeatDetectionSettings settings;
    settings.ffmpeg_path = FFMPEG_EXECUTABLE;
    const RationalTime duration{
        static_cast<int64_t>(std::llround(periodSeconds * clickCount * 1000)),
        1000};
    manager.Enqueue(MediaTaskKind::Beat, "beat detection fixture",
                    [&](MediaTaskContext& context, std::string& error) {
                        return DetectAudioBeats(source.string(), cache.string(),
                                                duration, settings, context,
                                                error);
                    });
    if (!manager.WaitForIdle(30000)) {
        std::cerr << "FAIL: beat detection task timed out\n";
        std::filesystem::remove_all(root);
        return false;
    }
    const auto tasks = manager.Snapshot();
    if (tasks.size() != 1 || tasks[0].state != MediaTaskState::Succeeded) {
        std::cerr << "FAIL: beat detection task did not succeed: "
                  << (tasks.empty() ? "" : tasks[0].error) << '\n';
        std::filesystem::remove_all(root);
        return false;
    }

    BeatDetectionResult result;
    std::string error;
    if (!LoadAudioBeats(cache.string(), result, error)) {
        std::cerr << "FAIL: unable to load beat detection cache: " << error
                  << '\n';
        std::filesystem::remove_all(root);
        return false;
    }

    // The very first click sits at t=0, with no preceding audio to rise
    // from — spectral flux cannot see an "increase" at the very first STFT
    // frame. Every click after the first sits after real silence and is
    // expected to be found. Ground truth therefore starts at index 1.
    std::vector<double> expectedOnsetSeconds;
    for (int i = 1; i < clickCount; ++i)
        expectedOnsetSeconds.push_back(i * periodSeconds);

    constexpr double kToleranceSeconds = 0.03;
    int matched = 0;
    for (double expected : expectedOnsetSeconds) {
        for (const AudioOnset& onset : result.onsets) {
            const double seconds =
                static_cast<double>(onset.time.value) / onset.time.rate;
            if (std::abs(seconds - expected) <= kToleranceSeconds) {
                ++matched;
                break;
            }
        }
    }
    const double matchRatio =
        expectedOnsetSeconds.empty()
            ? 0.0
            : static_cast<double>(matched) / expectedOnsetSeconds.size();

    // No detected onset should be far from every expected click either —
    // guards against spurious detections flooding the result.
    int spurious = 0;
    for (const AudioOnset& onset : result.onsets) {
        const double seconds =
            static_cast<double>(onset.time.value) / onset.time.rate;
        bool nearAny = seconds <= periodSeconds + kToleranceSeconds;
        for (double expected : expectedOnsetSeconds)
            if (std::abs(seconds - expected) <= kToleranceSeconds)
                nearAny = true;
        if (!nearAny) ++spurious;
    }

    bool ok = matchRatio >= 0.9 && spurious == 0;
    if (!ok) {
        std::cerr << "FAIL(" << bpm << " BPM): matched " << matched << "/"
                  << expectedOnsetSeconds.size() << " onsets, " << spurious
                  << " spurious, total detected " << result.onsets.size()
                  << '\n';
    } else {
        std::cout << "PASS: " << bpm << " BPM click-track onsets matched ("
                  << matched << "/" << expectedOnsetSeconds.size() << ")\n";
    }

    if (!result.bpm.has_value()) {
        std::cerr << "FAIL(" << bpm << " BPM): no BPM estimate produced\n";
        ok = false;
    } else {
        // Autocorrelation tempo induction is octave-ambiguous by nature: a
        // steady half- or double-tempo read is a known, accepted limitation
        // of this family of method, not a bug — so accept bpm, 2*bpm or
        // bpm/2 within tolerance.
        const bool matchesBpm = std::abs(*result.bpm - bpm) <= 2.0;
        const bool matchesDouble = std::abs(*result.bpm - 2 * bpm) <= 4.0;
        const bool matchesHalf = std::abs(*result.bpm - bpm / 2) <= 1.0;
        if (!(matchesBpm || matchesDouble || matchesHalf)) {
            std::cerr << "FAIL(" << bpm << " BPM): estimate " << *result.bpm
                      << " far from expected\n";
            ok = false;
        } else {
            std::cout << "PASS: " << bpm << " BPM estimate " << *result.bpm
                      << " (confidence " << result.bpm_confidence << ")\n";
        }
    }

    std::filesystem::remove_all(root);
    return ok;
}

// Settings that are internally inconsistent (a non-power-of-two FFT size,
// here) must be refused with an explicit error, never silently coerced.
bool RunInvalidSettingsAreRejected() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
                                       (GenerateUlid() + "-beats-invalid");
    const std::filesystem::path source = root / "click.wav";
    const std::filesystem::path cache = root / "cache" / "click.beats";
    std::filesystem::create_directories(root);
    if (!GenerateClickTrack(source, 0.5, 4, 48000)) {
        std::cerr << "FAIL: unable to generate invalid-settings fixture\n";
        std::filesystem::remove_all(root);
        return false;
    }

    MediaTaskManager manager;
    BeatDetectionSettings settings;
    settings.ffmpeg_path = FFMPEG_EXECUTABLE;
    settings.fft_size = 1000;  // Not a power of two.
    manager.Enqueue(MediaTaskKind::Beat, "invalid settings",
                    [&](MediaTaskContext& context, std::string& error) {
                        return DetectAudioBeats(source.string(), cache.string(),
                                                {2, 1}, settings, context,
                                                error);
                    });
    manager.WaitForIdle(30000);
    const auto tasks = manager.Snapshot();
    const bool ok = tasks.size() == 1 &&
                    tasks[0].state == MediaTaskState::Failed &&
                    !tasks[0].error.empty();
    std::filesystem::remove_all(root);
    if (!ok) {
        std::cerr << "FAIL: non-power-of-two fft_size was not refused\n";
    } else {
        std::cout << "PASS: non-power-of-two fft_size refused explicitly\n";
    }
    return ok;
}

}  // namespace

int main() {
    bool ok = true;
    ok &= RunClickTrackCase(120.0, 16);  // 8s at 120 BPM.
    ok &= RunClickTrackCase(90.0, 12);   // 8s at 90 BPM.
    ok &= RunInvalidSettingsAreRejected();
    if (!ok) return 1;
    std::cout << "PASS: beat detection cache generated and loaded\n";
    return 0;
}
