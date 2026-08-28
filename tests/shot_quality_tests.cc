#include "MediaTaskManager.h"
#include "ShotQuality.h"
#include "Ulid.h"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

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

constexpr int32_t kWidth = 64;
constexpr int32_t kHeight = 48;

std::vector<uint8_t> FlatFrame(uint8_t value) {
    return std::vector<uint8_t>(
        static_cast<size_t>(kWidth) * static_cast<size_t>(kHeight), value);
}

// A vertical bar pattern: `period` controls how often an edge occurs, so a
// small period is a busy (visually sharp) frame and a large one is flat.
std::vector<uint8_t> BarFrame(int32_t period, uint8_t low, uint8_t high) {
    std::vector<uint8_t> frame = FlatFrame(low);
    for (int32_t y = 0; y < kHeight; ++y) {
        for (int32_t x = 0; x < kWidth; ++x) {
            if ((x / period) % 2 == 1)
                frame[static_cast<size_t>(y) * kWidth + x] = high;
        }
    }
    return frame;
}

// A cheap separable box blur, applied in place. Blurring an image must lower
// the Laplacian variance; that is the entire premise of the metric, so the
// test proves it rather than assuming it.
std::vector<uint8_t> Blurred(const std::vector<uint8_t>& frame, int passes) {
    std::vector<uint8_t> current = frame;
    for (int pass = 0; pass < passes; ++pass) {
        std::vector<uint8_t> next = current;
        for (int32_t y = 1; y < kHeight - 1; ++y) {
            for (int32_t x = 1; x < kWidth - 1; ++x) {
                const size_t index = static_cast<size_t>(y) * kWidth + x;
                const int32_t total =
                    current[index - 1] + current[index] + current[index + 1] +
                    current[index - kWidth] + current[index + kWidth];
                next[index] = static_cast<uint8_t>(total / 5);
            }
        }
        current = std::move(next);
    }
    return current;
}

ShotQualityReport ReportFrom(const std::string& mediaId, uint32_t rate,
                             const std::vector<int64_t>& sharpness,
                             const std::vector<int64_t>& motion) {
    ShotQualityReport report;
    report.media_id = mediaId;
    report.samples_per_second = rate;
    report.analysis_width = kWidth;
    report.analysis_height = kHeight;
    for (size_t index = 0; index < sharpness.size(); ++index) {
        ShotQualitySample sample;
        sample.time = RationalTime{static_cast<int64_t>(index),
                                   static_cast<int32_t>(rate)};
        sample.sharpness = sharpness[index];
        sample.motion = index < motion.size() ? motion[index] : 0;
        report.samples.push_back(sample);
    }
    report.median_sharpness = ShotQualityPercentile(sharpness, 50);
    std::vector<int64_t> motionAfterFirst(
        motion.begin() + (motion.empty() ? 0 : 1), motion.end());
    report.median_motion = ShotQualityPercentile(motionAfterFirst, 50);
    return report;
}

DocumentClip ClipOver(const std::string& sourceId, int64_t inValue,
                      int64_t durationValue, int32_t rate) {
    DocumentClip clip;
    clip.id = "01KQ000000000000000000000C";
    clip.source_id = sourceId;
    clip.source_in = {inValue, rate};
    clip.duration = {durationValue, rate};
    clip.timeline_in = {0, rate};
    return clip;
}

}  // namespace

int main() {
    // ---- Sharpness: a blurred frame must measure softer than its origin ----
    const std::vector<uint8_t> busy = BarFrame(2, 16, 240);
    const std::vector<uint8_t> calm = BarFrame(16, 16, 240);
    const std::vector<uint8_t> flat = FlatFrame(128);

    const int64_t busySharpness =
        FrameSharpnessMetric(busy.data(), kWidth, kHeight);
    const int64_t calmSharpness =
        FrameSharpnessMetric(calm.data(), kWidth, kHeight);
    const int64_t flatSharpness =
        FrameSharpnessMetric(flat.data(), kWidth, kHeight);

    Check(flatSharpness == 0, "a flat frame has no Laplacian variance");
    Check(busySharpness > calmSharpness,
          "a frame with more edges measures sharper than one with fewer");
    Check(calmSharpness > flatSharpness,
          "any edge at all measures sharper than none");

    const std::vector<uint8_t> softened = Blurred(busy, 3);
    const int64_t softenedSharpness =
        FrameSharpnessMetric(softened.data(), kWidth, kHeight);
    Check(softenedSharpness < busySharpness,
          "blurring a frame lowers its sharpness metric");
    Check(softenedSharpness > 0,
          "a blurred frame still carries some measurable detail");

    // Determinism: the metric is integer arithmetic end to end, so the same
    // pixels must give bit-identical results on repeated calls.
    Check(FrameSharpnessMetric(busy.data(), kWidth, kHeight) == busySharpness,
          "sharpness is deterministic for identical pixels");

    // Degenerate planes are answered, not crashed on.
    Check(FrameSharpnessMetric(nullptr, kWidth, kHeight) == 0,
          "a null plane measures zero sharpness");
    Check(FrameSharpnessMetric(busy.data(), 2, 2) == 0,
          "a plane with no 3x3 interior measures zero sharpness");

    // ---- Motion: identical frames are still, opposite frames are maximal --
    Check(FrameMotionMetric(busy.data(), busy.data(), kWidth, kHeight) == 0,
          "a frame compared against itself shows no motion");

    const std::vector<uint8_t> black = FlatFrame(0);
    const std::vector<uint8_t> white = FlatFrame(255);
    Check(FrameMotionMetric(black.data(), white.data(), kWidth, kHeight) ==
              kShotQualityMetricScale,
          "black to white is full-scale motion");
    Check(FrameMotionMetric(white.data(), black.data(), kWidth, kHeight) ==
              kShotQualityMetricScale,
          "motion is symmetric: it measures magnitude, not direction");

    const std::vector<uint8_t> grey = FlatFrame(128);
    const int64_t partial =
        FrameMotionMetric(black.data(), grey.data(), kWidth, kHeight);
    Check(partial > 0 && partial < kShotQualityMetricScale,
          "a partial change measures between still and full scale");

    // ---- Percentiles -------------------------------------------------------
    const std::vector<int64_t> ramp = {10, 20, 30, 40, 50};
    Check(ShotQualityPercentile(ramp, 0) == 10, "p0 is the minimum");
    Check(ShotQualityPercentile(ramp, 100) == 50, "p100 is the maximum");
    Check(ShotQualityPercentile(ramp, 50) == 30, "p50 of a ramp is its middle");
    Check(ShotQualityPercentile({}, 50) == 0, "an empty set percentiles to 0");
    Check(ShotQualityPercentile({7}, 90) == 7, "a single value is every rank");
    // Deliberately the lower of the two middle values, never their average:
    // every reported figure must be one that was actually measured.
    Check(ShotQualityPercentile({10, 20, 30, 40}, 50) == 20,
          "an even count takes the lower middle rather than averaging");

    // ---- Clip summary ------------------------------------------------------
    const std::string sourceId = "01KQ0000000000000000000001";
    // Twelve samples at 4/s. The take is mostly usable and ends on a short
    // soft, moving stretch -- which is the shape real footage has, and the
    // shape a source-relative grade needs: a median taken over a source that
    // is half ruined would sit between the two states and call neither of
    // them bad. That failure mode is real and is documented on
    // ShotQualityThresholds; the fixture stays honest about it by not
    // pretending it cannot happen.
    const ShotQualityReport report = ReportFrom(
        sourceId, 4,
        {4000, 4200, 3900, 4100, 4000, 3950, 4050, 3900, 950, 900, 1000, 880},
        {0, 2000, 1500, 1800, 2200, 1900, 1700, 2100, 120000, 130000, 118000,
         125000});

    ShotQualityThresholds thresholds;
    ClipShotQuality summary;
    std::string error;

    // The usable stretch: samples 0..7, i.e. source seconds [0, 2).
    Check(SummarizeClipShotQuality(ClipOver(sourceId, 0, 8, 4), report,
                                   thresholds, summary, error),
          "the sharp stretch summarizes: " + error);
    Check(summary.samples == 8, "the sharp stretch contains eight samples");
    Check(summary.sharpness == SharpnessGrade::Sharp,
          "the sharp stretch grades Sharp");
    Check(summary.steadiness == SteadinessGrade::Steady,
          "the sharp stretch grades Steady");
    Check(summary.clean, "the sharp stretch is clean");
    Check(summary.clip_id == "01KQ000000000000000000000C" &&
              summary.source_id == sourceId,
          "the summary carries the clip and source it describes");

    // The ruined tail: samples 8..11.
    Check(SummarizeClipShotQuality(ClipOver(sourceId, 8, 4, 4), report,
                                   thresholds, summary, error),
          "the soft tail summarizes: " + error);
    Check(summary.sharpness == SharpnessGrade::Blurry,
          "a tail measuring a quarter of the source median grades Blurry");
    Check(summary.steadiness == SteadinessGrade::Shaky,
          "a tail above the shaky threshold grades Shaky");
    Check(!summary.clean, "the soft tail is not clean");
    Check(summary.source_median_sharpness == report.median_sharpness,
          "the summary reports the source median it was graded against");

    // A clip whose source range holds no sample is refused rather than
    // silently graded from nothing.
    Check(!SummarizeClipShotQuality(ClipOver(sourceId, 40, 1, 100), report,
                                    thresholds, summary, error),
          "a clip with no samples inside it is refused");
    Check(!error.empty(), "the refusal explains itself");

    // A report belonging to another source is refused, never resolved
    // against the wrong media -- the same rule ResolveWordRemoval enforces.
    Check(!SummarizeClipShotQuality(
              ClipOver("01KQ0000000000000000000009", 0, 5, 4), report,
              thresholds, summary, error),
          "a mismatched source is refused");

    // Sampling grid and clip timebase need not agree: samples 0..7 at 4/s
    // span exactly source frames [0, 50) at 25/s, and the comparison is
    // exact in both directions because RationalTime::compare never converts.
    Check(SummarizeClipShotQuality(ClipOver(sourceId, 0, 50, 25), report,
                                   thresholds, summary, error),
          "a clip on the media's own 25/s grid summarizes: " + error);
    Check(summary.samples == 8,
          "a 25/s clip spanning two seconds covers the same eight 4/s samples");

    // The documented failure mode of a source-relative grade, asserted
    // rather than left in a comment: when half a source is ruined its median
    // sits between the two states and neither half stands out. A caller that
    // needs to catch this reads the absolute medians, which is why the
    // summary reports them.
    const ShotQualityReport halfRuined = ReportFrom(
        sourceId, 4, {4000, 4000, 4000, 4000, 900, 900, 900, 900}, {});
    ClipShotQuality ruinedHalf;
    ClipShotQuality goodHalf;
    Check(SummarizeClipShotQuality(ClipOver(sourceId, 4, 4, 4), halfRuined,
                                   thresholds, ruinedHalf, error) &&
              SummarizeClipShotQuality(ClipOver(sourceId, 0, 4, 4), halfRuined,
                                       thresholds, goodHalf, error),
          "both halves of a half-ruined source summarize: " + error);
    Check(ruinedHalf.sharpness == SharpnessGrade::Sharp &&
              goodHalf.sharpness == SharpnessGrade::Sharp,
          "a source that is half ruined grades both halves Sharp -- the known "
          "cost of grading against the source's own median");
    // The relative signal does not merely weaken here, it vanishes: the
    // median lands on the ruined value, so the bad half is graded against
    // itself. Nothing in the grade can recover that, which is exactly why
    // the absolute figures are reported and why the header says so.
    Check(ruinedHalf.sharpness_median == ruinedHalf.source_median_sharpness,
          "the ruined half is graded against its own value, so the relative "
          "signal is gone entirely");
    Check(goodHalf.sharpness_median > ruinedHalf.sharpness_median * 4,
          "only the absolute medians still separate the two halves");

    // A source whose baseline motion is high must not have its ordinary
    // stretches graded Moving. This is the failure an absolute threshold
    // actually produced on real footage: anchored on a static take, it
    // called 67% of a handheld interview "moving" and 34% of it "shaky".
    // The numbers below are that interview's measured shape -- median 51 616,
    // ordinary peaks around 105 000, genuine spikes past 200 000.
    const ShotQualityReport handheld = ReportFrom(
        sourceId, 4, {3000, 3000, 3000, 3000, 3000, 3000, 3000, 3000},
        {0, 48000, 51616, 55000, 105000, 49000, 52000, 230000});
    ClipShotQuality ordinary;
    ClipShotQuality spike;
    Check(SummarizeClipShotQuality(ClipOver(sourceId, 1, 6, 4), handheld,
                                   thresholds, ordinary, error),
          "the ordinary stretch of a handheld take summarizes: " + error);
    Check(ordinary.steadiness == SteadinessGrade::Steady,
          "normal handheld movement is not a defect: p90 " +
              std::to_string(ordinary.motion_peak) + " against source median " +
              std::to_string(ordinary.source_median_motion));
    Check(SummarizeClipShotQuality(ClipOver(sourceId, 7, 1, 4), handheld,
                                   thresholds, spike, error) &&
              spike.steadiness == SteadinessGrade::Shaky,
          "but a genuine spike above three times the source median still "
          "grades Shaky");

    // The opposite guard: on a locked-off tripod the median approaches zero,
    // and without the floor every ratio would fire on sensor noise.
    const ShotQualityReport tripod =
        ReportFrom(sourceId, 4, {3000, 3000, 3000, 3000}, {0, 120, 90, 400});
    ClipShotQuality still;
    Check(SummarizeClipShotQuality(ClipOver(sourceId, 1, 3, 4), tripod,
                                   thresholds, still, error),
          "a tripod take summarizes: " + error);
    Check(still.steadiness == SteadinessGrade::Steady,
          "a fourfold jump in sensor noise is still a still frame, because "
          "the absolute floor outranks the ratio");

    // ---- Cache round-trip --------------------------------------------------
    const std::string json = SerializeShotQuality(report);
    Check(json.find('.') == std::string::npos,
          "the cache holds no float literal");
    ShotQualityReport reloaded;
    Check(DeserializeShotQuality(json, reloaded, error),
          "the cache parses back: " + error);
    Check(SerializeShotQuality(reloaded) == json,
          "serialize -> parse -> serialize is byte-identical");
    Check(reloaded.samples.size() == report.samples.size() &&
              reloaded.media_id == report.media_id &&
              reloaded.samples_per_second == report.samples_per_second &&
              reloaded.median_sharpness == report.median_sharpness,
          "the reloaded report carries the same header");
    bool sameSamples = true;
    for (size_t index = 0; index < report.samples.size(); ++index) {
        sameSamples =
            sameSamples &&
            reloaded.samples[index].sharpness ==
                report.samples[index].sharpness &&
            reloaded.samples[index].motion == report.samples[index].motion &&
            reloaded.samples[index].time.compare(report.samples[index].time) ==
                0;
    }
    Check(sameSamples, "every sample survives the round trip exactly");

    ShotQualityReport rejected;
    Check(!DeserializeShotQuality("{\"version\":3}", rejected, error),
          "a future cache version is refused rather than guessed at");
    // The version that graded motion against an absolute threshold is
    // refused by name, so a stale cache is re-analysed instead of silently
    // producing grades from a rule that measurement disproved.
    Check(!DeserializeShotQuality(
              "{\"version\":1,\"media_id\":\"x\",\"samples_per_second\":4,"
              "\"analysis_width\":8,\"analysis_height\":8,"
              "\"median_sharpness\":1,\"sharpness\":[1],\"motion\":[0]}",
              rejected, error) &&
              error.find("re-run") != std::string::npos,
          "a pre-relative-motion cache is refused and says to re-run");
    Check(!DeserializeShotQuality(
              "{\"version\":2,\"media_id\":\"x\",\"samples_per_second\":4,"
              "\"analysis_width\":8,\"analysis_height\":8,"
              "\"median_sharpness\":1,\"median_motion\":1,"
              "\"sharpness\":[1,2],\"motion\":[1]}",
              rejected, error),
          "mismatched metric arrays are refused");

    // ---- End to end, through FFmpeg on real decoded frames ---------------
    // Everything above works on synthetic planes. This section proves the
    // decode path actually feeds them: without it, the analysis could be
    // measuring the wrong plane, the wrong stride or the wrong frames and
    // every assertion above would still pass.
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / (GenerateUlid() + "-shotqc");
    std::filesystem::create_directories(root);
    const std::filesystem::path halfSoft = root / "half-soft.mp4";
    const std::filesystem::path moving = root / "moving.mp4";
    const std::filesystem::path reportPath = root / "cache" / "half-soft.json";
    const std::filesystem::path movingReportPath =
        root / "cache" / "moving.json";

    // One still source whose last quarter is heavily blurred. A quarter, not
    // a half, on purpose: sharpness is graded against the source's own
    // median, so a source that is half ruined has its median sitting between
    // the two states and grades neither as bad. That is the documented cost
    // of a relative grade, and it is pinned by its own assertion below --
    // this fixture is the ordinary case, a mostly usable take with a bad
    // stretch at the end.
    const std::string makeHalfSoft =
        Quote(FFMPEG_EXECUTABLE) +
        " -hide_banner -loglevel error -f lavfi -i "
        "'testsrc2=size=320x240:rate=25:duration=4' -vf "
        "'loop=loop=-1:size=1:start=0,trim=duration=4,setpts=N/25/TB,"
        "boxblur=8:2:enable=gte(t\\,3)' -c:v libx264 -pix_fmt yuv420p -y " +
        Quote(halfSoft);
    // The same generator left animated, as the moving counterpart.
    const std::string makeMoving = Quote(FFMPEG_EXECUTABLE) +
                                   " -hide_banner -loglevel error -f lavfi -i "
                                   "'testsrc2=size=320x240:rate=25:duration=4' "
                                   "-c:v libx264 -pix_fmt yuv420p -y " +
                                   Quote(moving);
    if (std::system(makeHalfSoft.c_str()) != 0 ||
        std::system(makeMoving.c_str()) != 0) {
        std::cerr << "FAIL: unable to generate shot quality fixtures\n";
        std::filesystem::remove_all(root);
        return 1;
    }

    ShotQualitySettings settings;
    settings.ffmpeg_path = FFMPEG_EXECUTABLE;
    MediaTaskManager manager;
    const std::string stillId = "01KQ00000000000000000000ST";
    const std::string movingId = "01KQ00000000000000000000MV";
    manager.Enqueue(MediaTaskKind::ShotQuality, "half soft",
                    [&](MediaTaskContext& context, std::string& taskError) {
                        return GenerateShotQuality(
                            halfSoft.string(), reportPath.string(), stillId,
                            {4, 1}, settings, context, taskError);
                    });
    manager.Enqueue(MediaTaskKind::ShotQuality, "moving",
                    [&](MediaTaskContext& context, std::string& taskError) {
                        return GenerateShotQuality(
                            moving.string(), movingReportPath.string(),
                            movingId, {4, 1}, settings, context, taskError);
                    });
    if (!manager.WaitForIdle(120000)) {
        std::cerr << "FAIL: shot quality analysis timed out\n";
        std::filesystem::remove_all(root);
        return 1;
    }

    ShotQualityReport measured;
    Check(LoadShotQuality(reportPath.string(), measured, error),
          "the analysis wrote a readable cache: " + error);
    Check(measured.media_id == stillId,
          "the cache carries the media identity it was asked for");
    // Four seconds at four samples a second, give or take the encoder's
    // final frame.
    Check(measured.samples.size() >= 15 && measured.samples.size() <= 17,
          "a four second source produces about sixteen samples at 4/s, got " +
              std::to_string(measured.samples.size()));

    // The take is sharp until t=3 and blurred after: the measurement has to
    // see that on real decoded pixels, not just on synthetic ones.
    std::vector<int64_t> sharpPart;
    std::vector<int64_t> softPart;
    std::vector<int64_t> stillMotion;
    for (const ShotQualitySample& sample : measured.samples) {
        if (sample.time.compare({3, 1}) < 0)
            sharpPart.push_back(sample.sharpness);
        else
            softPart.push_back(sample.sharpness);
        if (sample.time.value > 0) stillMotion.push_back(sample.motion);
    }
    const int64_t sharpMedian = ShotQualityPercentile(sharpPart, 50);
    const int64_t softMedian = ShotQualityPercentile(softPart, 50);
    Check(sharpMedian > softMedian * 2,
          "the blurred stretch measures at least twice as soft as the rest "
          "(" +
              std::to_string(sharpMedian) + " vs " +
              std::to_string(softMedian) + ")");

    // A frozen source must show essentially no motion. This is what catches
    // a decode path that silently sampled the wrong frames.
    Check(ShotQualityPercentile(stillMotion, 90) < 5000,
          "a frozen source measures still, got p90 " +
              std::to_string(ShotQualityPercentile(stillMotion, 90)));

    ShotQualityReport movingMeasured;
    Check(LoadShotQuality(movingReportPath.string(), movingMeasured, error),
          "the moving fixture analyses too: " + error);
    std::vector<int64_t> movingMotion;
    for (const ShotQualitySample& sample : movingMeasured.samples)
        if (sample.time.value > 0) movingMotion.push_back(sample.motion);
    Check(ShotQualityPercentile(movingMotion, 50) >
              ShotQualityPercentile(stillMotion, 90),
          "an animated source measures more motion than a frozen one (" +
              std::to_string(ShotQualityPercentile(movingMotion, 50)) + " vs " +
              std::to_string(ShotQualityPercentile(stillMotion, 90)) + ")");

    // And the grade follows the measurement: a clip over the blurred half is
    // refused as an illustration while one over the sharp half is not.
    DocumentClip sharpClip = ClipOver(stillId, 0, 25, 25);
    DocumentClip softClip = ClipOver(stillId, 80, 15, 25);
    ClipShotQuality sharpSummary;
    ClipShotQuality softSummary;
    Check(SummarizeClipShotQuality(sharpClip, measured, thresholds,
                                   sharpSummary, error),
          "the sharp second summarizes: " + error);
    Check(SummarizeClipShotQuality(softClip, measured, thresholds, softSummary,
                                   error),
          "the blurred stretch summarizes: " + error);
    Check(sharpSummary.sharpness == SharpnessGrade::Sharp,
          "a clip over the sharp stretch is usable");
    Check(softSummary.sharpness != SharpnessGrade::Sharp,
          "a clip over the blurred stretch is not (worst decile " +
              std::to_string(softSummary.sharpness_worst) +
              " against source median " +
              std::to_string(softSummary.source_median_sharpness) + ")");
    Check(!softSummary.clean, "and it is reported as not clean");

    // The cache is reproducible: the same media analysed twice must produce
    // the same bytes, which is what makes a stored grade trustworthy.
    const std::filesystem::path second = root / "cache" / "again.json";
    MediaTaskManager repeat;
    repeat.Enqueue(MediaTaskKind::ShotQuality, "half soft again",
                   [&](MediaTaskContext& context, std::string& taskError) {
                       return GenerateShotQuality(
                           halfSoft.string(), second.string(), stillId, {4, 1},
                           settings, context, taskError);
                   });
    Check(repeat.WaitForIdle(120000), "the repeat analysis finishes");
    ShotQualityReport again;
    Check(LoadShotQuality(second.string(), again, error),
          "the repeat cache reads back: " + error);
    Check(SerializeShotQuality(again) == SerializeShotQuality(measured),
          "analysing the same media twice produces identical bytes");

    std::filesystem::remove_all(root);

    if (failures == 0) std::cout << "shot quality tests passed\n";
    return failures == 0 ? 0 : 1;
}
