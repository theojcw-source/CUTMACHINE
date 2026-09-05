#include "ShotQuality.h"

#include "Json.h"
#include "Ulid.h"

#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>

extern char** environ;

namespace {

using mcp_json::Value;

void AppendTail(std::string& tail, const char* bytes, size_t count) {
    tail.append(bytes, count);
    constexpr size_t kMaximum = 8192;
    if (tail.size() > kMaximum) tail.erase(0, tail.size() - kMaximum);
}

// Both metrics are accumulated in integers and divided once, at the end,
// through __int128. Nothing here ever holds an intermediate float: two runs
// over identical pixels produce identical integers on any host, which is
// what makes the cache file reproducible (PHILOSOPHY.md principle 6).
int64_t ScaleToMetric(__int128 numerator, __int128 denominator) {
    if (denominator <= 0) return 0;
    const __int128 scaled = numerator * kShotQualityMetricScale / denominator;
    if (scaled < 0) return 0;
    if (scaled > std::numeric_limits<int64_t>::max())
        return std::numeric_limits<int64_t>::max();
    return static_cast<int64_t>(scaled);
}

bool ReadInt64Field(const Value& object, const std::string& key,
                    int64_t& output) {
    const Value* field = object.Find(key);
    return field != nullptr && field->AsInt64(output);
}

}  // namespace

const char* SharpnessGradeName(SharpnessGrade grade) {
    switch (grade) {
        case SharpnessGrade::Sharp:
            return "Sharp";
        case SharpnessGrade::Soft:
            return "Soft";
        case SharpnessGrade::Blurry:
            return "Blurry";
    }
    return "Sharp";
}

const char* SteadinessGradeName(SteadinessGrade grade) {
    switch (grade) {
        case SteadinessGrade::Steady:
            return "Steady";
        case SteadinessGrade::Moving:
            return "Moving";
        case SteadinessGrade::Shaky:
            return "Shaky";
    }
    return "Steady";
}

int64_t FrameSharpnessMetric(const uint8_t* luma, int32_t width,
                             int32_t height) {
    if (luma == nullptr || width < 3 || height < 3) return 0;
    // 3x3 Laplacian over the interior only: the border has no full
    // neighbourhood, and replicating edge pixels to fake one would inject a
    // zero response that drags the variance toward zero by an amount that
    // depends on the frame's aspect ratio rather than on its focus.
    const int64_t count =
        static_cast<int64_t>(width - 2) * static_cast<int64_t>(height - 2);
    __int128 sum = 0;
    __int128 sumOfSquares = 0;
    for (int32_t y = 1; y < height - 1; ++y) {
        const uint8_t* row = luma + static_cast<size_t>(y) * width;
        const uint8_t* above = row - width;
        const uint8_t* below = row + width;
        for (int32_t x = 1; x < width - 1; ++x) {
            const int32_t response = static_cast<int32_t>(above[x]) +
                                     static_cast<int32_t>(below[x]) +
                                     static_cast<int32_t>(row[x - 1]) +
                                     static_cast<int32_t>(row[x + 1]) -
                                     4 * static_cast<int32_t>(row[x]);
            sum += response;
            sumOfSquares += static_cast<__int128>(response) * response;
        }
    }
    // Population variance, kept exact: (n*sum2 - sum^2) / n^2 rather than
    // sum2/n - (sum/n)^2, which would round twice before subtracting.
    const __int128 numerator =
        static_cast<__int128>(count) * sumOfSquares - sum * sum;
    const __int128 denominator = static_cast<__int128>(count) * count;
    // Divided by 255^2 so the result is a fraction of full scale and does
    // not depend on the bit depth the analysis plane happens to use.
    return ScaleToMetric(numerator, denominator * 255 * 255);
}

int64_t FrameMotionMetric(const uint8_t* previous, const uint8_t* current,
                          int32_t width, int32_t height) {
    if (previous == nullptr || current == nullptr || width < 1 || height < 1)
        return 0;
    const int64_t count =
        static_cast<int64_t>(width) * static_cast<int64_t>(height);
    __int128 total = 0;
    for (int64_t index = 0; index < count; ++index) {
        const int32_t difference = static_cast<int32_t>(current[index]) -
                                   static_cast<int32_t>(previous[index]);
        total += difference < 0 ? -difference : difference;
    }
    return ScaleToMetric(total, static_cast<__int128>(count) * 255);
}

int64_t FrameHistogramDistanceMetric(const uint8_t* previous,
                                     const uint8_t* current, int32_t width,
                                     int32_t height) {
    if (previous == nullptr || current == nullptr || width < 1 || height < 1)
        return 0;
    const int64_t count =
        static_cast<int64_t>(width) * static_cast<int64_t>(height);
    // 256 / kShotQualityHistogramBins is exact, so every byte maps into
    // range and the compiler turns the division into a shift.
    constexpr int32_t kBinWidth = 256 / kShotQualityHistogramBins;
    int64_t previousBins[kShotQualityHistogramBins] = {};
    int64_t currentBins[kShotQualityHistogramBins] = {};
    for (int64_t index = 0; index < count; ++index) {
        ++previousBins[previous[index] / kBinWidth];
        ++currentBins[current[index] / kBinWidth];
    }
    __int128 total = 0;
    for (int32_t bin = 0; bin < kShotQualityHistogramBins; ++bin) {
        const int64_t difference = currentBins[bin] - previousBins[bin];
        total += difference < 0 ? -difference : difference;
    }
    // Both histograms hold exactly `count` entries, so what one bin gains
    // another loses and the absolute differences sum to twice the number of
    // pixels that moved. Dividing by 2*count therefore reads directly as
    // "the fraction of pixels that changed luma bin", and reaches full scale
    // only when the two planes share no bin at all.
    return ScaleToMetric(total, static_cast<__int128>(count) * 2);
}

int64_t ShotQualityPercentile(std::vector<int64_t> values, int percent) {
    if (values.empty()) return 0;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    std::sort(values.begin(), values.end());
    // Nearest-rank on the lower side: index = floor(p * (n-1) / 100). With
    // integer arithmetic this is exact and stable, and for p=50 on an even
    // count it deliberately picks the lower of the two middle values rather
    // than averaging them -- an averaged median would be the one place in
    // this file where a value not actually measured gets reported.
    const int64_t index = static_cast<int64_t>(percent) *
                          static_cast<int64_t>(values.size() - 1) / 100;
    return values[static_cast<size_t>(index)];
}

bool SummarizeClipShotQuality(const DocumentClip& clip,
                              const ShotQualityReport& report,
                              const ShotQualityThresholds& thresholds,
                              ClipShotQuality& summary, std::string& error) {
    error.clear();
    if (report.media_id != clip.source_id) {
        error = "shot quality report does not match the clip source";
        return false;
    }
    if (report.samples_per_second == 0) {
        error = "shot quality report has no analysis rate";
        return false;
    }
    const RationalTime clipEnd = clip.source_in.add(clip.duration);
    std::vector<int64_t> sharpness;
    std::vector<int64_t> motion;
    for (const ShotQualitySample& sample : report.samples) {
        if (sample.time.compare(clip.source_in) < 0) continue;
        if (sample.time.compare(clipEnd) >= 0) break;
        sharpness.push_back(sample.sharpness);
        motion.push_back(sample.motion);
    }
    if (sharpness.empty()) {
        error = "clip is shorter than one analysis sample interval";
        return false;
    }

    summary = ClipShotQuality{};
    summary.clip_id = clip.id;
    summary.source_id = clip.source_id;
    summary.samples = static_cast<int32_t>(sharpness.size());
    summary.sharpness_median = ShotQualityPercentile(sharpness, 50);
    summary.sharpness_worst = ShotQualityPercentile(sharpness, 10);
    summary.motion_median = ShotQualityPercentile(motion, 50);
    summary.motion_peak = ShotQualityPercentile(motion, 90);
    summary.source_median_sharpness = report.median_sharpness;

    // Graded on the worst decile, not the median: a shot that racks focus
    // halfway through reads healthy at p50 and is still unusable.
    if (report.median_sharpness > 0) {
        const __int128 ratio = static_cast<__int128>(summary.sharpness_worst) *
                               100 / report.median_sharpness;
        if (ratio < thresholds.blurry_ratio_percent)
            summary.sharpness = SharpnessGrade::Blurry;
        else if (ratio < thresholds.soft_ratio_percent)
            summary.sharpness = SharpnessGrade::Soft;
    }
    summary.source_median_motion = report.median_motion;
    // Same shape as the sharpness grade, in the other direction. The floor
    // keeps a locked-off tripod from grading its own sensor noise.
    const int64_t movingAt =
        std::max(thresholds.motion_floor,
                 report.median_motion * thresholds.moving_ratio_percent / 100);
    const int64_t shakyAt =
        std::max(thresholds.motion_floor,
                 report.median_motion * thresholds.shaky_ratio_percent / 100);
    if (summary.motion_peak >= shakyAt)
        summary.steadiness = SteadinessGrade::Shaky;
    else if (summary.motion_peak >= movingAt)
        summary.steadiness = SteadinessGrade::Moving;

    summary.clean = summary.sharpness == SharpnessGrade::Sharp &&
                    summary.steadiness == SteadinessGrade::Steady;
    return true;
}

std::vector<SourceShot> DetectSourceShots(
    const ShotQualityReport& report, const ShotSegmentationSettings& settings) {
    std::vector<SourceShot> shots;
    if (report.samples.empty() || report.samples_per_second == 0) return shots;
    const int32_t rate = static_cast<int32_t>(report.samples_per_second);

    // The two absolute conditions. They do not separate a cut from a camera
    // move -- see ShotSegmentationSettings, where the measurements are --
    // they only stop the local ratio below from firing on a neighbourhood
    // that barely moved at all.
    const int64_t histogramAt =
        std::max(settings.cut_histogram_floor,
                 report.median_histogram_distance *
                     settings.cut_histogram_ratio_percent / 100);
    const int64_t window =
        std::max<int64_t>(1, settings.cut_local_window_samples);

    // Sample 0 always opens a shot: the source starts somewhere, and its
    // two change metrics are zero by construction rather than by
    // measurement, so it can never be a detected boundary itself. For the
    // same reason it is left out of every neighbourhood below.
    std::vector<size_t> starts = {0};
    std::vector<int64_t> neighbourhood;
    for (size_t index = 1; index < report.samples.size(); ++index) {
        const ShotQualitySample& sample = report.samples[index];
        if (sample.motion < settings.cut_motion_floor) continue;
        if (sample.histogram_distance < histogramAt) continue;

        // The condition that does the actual work: a cut is instantaneous,
        // so it stands alone; a camera move has inertia, so its neighbours
        // moved nearly as much as it did.
        const size_t reach = static_cast<size_t>(window);
        const size_t first = index > reach ? index - reach : 1;
        const size_t limit = std::min(report.samples.size(), index + reach + 1);
        neighbourhood.clear();
        for (size_t other = first; other < limit; ++other)
            if (other != index)
                neighbourhood.push_back(report.samples[other].motion);
        // A median of zero means nothing around this sample moved at all,
        // and no multiple of zero is reachable. That is not a reason to
        // reject: a spike in a perfectly still neighbourhood is the clearest
        // boundary there is, so the ratio passes rather than dividing.
        const int64_t baseline = ShotQualityPercentile(neighbourhood, 50);
        if (baseline > 0 && static_cast<__int128>(sample.motion) * 100 <
                                static_cast<__int128>(baseline) *
                                    settings.cut_motion_local_percent)
            continue;
        // Too close to the previous boundary: dropped, not merged into it.
        // Keeping the earlier one is what makes the pass single-sweep and
        // its output independent of how many candidates cluster inside the
        // window -- a flash frame produces one dropped candidate whether it
        // lasted one sample or three.
        if (static_cast<int64_t>(index - starts.back()) <
            settings.minimum_samples)
            continue;
        starts.push_back(index);
    }
    // The tail gets the same minimum as everything else. Without this a cut
    // near the very end of a source leaves a stub shot that no rule
    // upstream would have allowed anywhere else in it.
    if (starts.size() > 1 &&
        static_cast<int64_t>(report.samples.size() - starts.back()) <
            settings.minimum_samples)
        starts.pop_back();

    shots.reserve(starts.size());
    for (size_t shotIndex = 0; shotIndex < starts.size(); ++shotIndex) {
        const size_t first = starts[shotIndex];
        const size_t limit = shotIndex + 1 < starts.size()
                                 ? starts[shotIndex + 1]
                                 : report.samples.size();
        SourceShot shot;
        shot.first_sample = static_cast<int32_t>(first);
        shot.sample_count = static_cast<int32_t>(limit - first);
        shot.start = RationalTime{static_cast<int64_t>(first), rate};
        shot.end = RationalTime{static_cast<int64_t>(limit), rate};

        std::vector<int64_t> sharpness;
        std::vector<int64_t> motion;
        sharpness.reserve(limit - first);
        size_t keyframe = first;
        for (size_t index = first; index < limit; ++index) {
            sharpness.push_back(report.samples[index].sharpness);
            // A shot's first sample measures its change against the *last
            // sample of the previous shot*, which for a detected boundary is
            // the cut itself. Counting it would put the cut's own spike into
            // the shot's motion median and report every shot as moving.
            if (index > first) motion.push_back(report.samples[index].motion);
            // Strictly greater, so a tie keeps the earliest sample and two
            // runs over the same media pick the same frame.
            if (report.samples[index].sharpness >
                report.samples[keyframe].sharpness)
                keyframe = index;
        }
        shot.keyframe_sample = static_cast<int32_t>(keyframe);
        shot.keyframe = RationalTime{static_cast<int64_t>(keyframe), rate};
        shot.median_sharpness = ShotQualityPercentile(std::move(sharpness), 50);
        shot.median_motion = ShotQualityPercentile(std::move(motion), 50);
        shots.push_back(shot);
    }
    return shots;
}

// Cache layout, written strictly and read tolerantly (the same split
// Transcription.cc uses). Sample k's time is {k, samples_per_second} by
// construction, so the index is the timestamp and only the three metrics
// are stored -- parallel integer arrays rather than an array of objects,
// which keeps a one-hour rush's cache in the low hundreds of kilobytes and
// still reads plainly in a text editor. There is no float literal anywhere
// in the file.
std::string SerializeShotQuality(const ShotQualityReport& report) {
    std::ostringstream output;
    output << "{\"version\":3,\"media_id\":\""
           << mcp_json::EscapeJsonString(report.media_id)
           << "\",\"samples_per_second\":" << report.samples_per_second
           << ",\"analysis_width\":" << report.analysis_width
           << ",\"analysis_height\":" << report.analysis_height
           << ",\"median_sharpness\":" << report.median_sharpness
           << ",\"median_motion\":" << report.median_motion
           << ",\"median_histogram_distance\":"
           << report.median_histogram_distance << ",\"sharpness\":[";
    for (size_t index = 0; index < report.samples.size(); ++index) {
        if (index) output << ',';
        output << report.samples[index].sharpness;
    }
    output << "],\"motion\":[";
    for (size_t index = 0; index < report.samples.size(); ++index) {
        if (index) output << ',';
        output << report.samples[index].motion;
    }
    output << "],\"histogram\":[";
    for (size_t index = 0; index < report.samples.size(); ++index) {
        if (index) output << ',';
        output << report.samples[index].histogram_distance;
    }
    output << "]}";
    return output.str();
}

bool DeserializeShotQuality(const std::string& json, ShotQualityReport& report,
                            std::string& error) {
    error.clear();
    Value root;
    std::string parseError;
    if (!Value::Parse(json, root, parseError) || !root.IsObject()) {
        error = "malformed shot quality cache: " + parseError;
        return false;
    }
    int64_t version = 0;
    // Older caches are refused rather than migrated, and the reason has not
    // changed between the two bumps: a cache is cheap to rebuild (ten
    // seconds for a seven-minute rush), while inventing a metric that was
    // never measured would be the silent fallback this project refuses
    // everywhere else. Version 1 stored no median_motion, because motion was
    // graded against an absolute threshold measurement later showed to be
    // wrong. Version 2 stored no histogram distance, so DetectSourceShots
    // could not tell one of its two conditions from the other and would have
    // had to call every whip pan a cut.
    if (!ReadInt64Field(root, "version", version) || version != 3) {
        error = (version == 1 || version == 2)
                    ? "shot quality cache predates shot segmentation; "
                      "re-run the analysis"
                    : "unsupported shot quality cache version";
        return false;
    }
    ShotQualityReport parsed;
    const Value* mediaId = root.Find("media_id");
    if (mediaId == nullptr || !mediaId->IsString()) {
        error = "shot quality cache has no media_id";
        return false;
    }
    parsed.media_id = mediaId->AsString();
    int64_t rate = 0;
    int64_t width = 0;
    int64_t height = 0;
    if (!ReadInt64Field(root, "samples_per_second", rate) || rate <= 0 ||
        rate > std::numeric_limits<int32_t>::max() ||
        !ReadInt64Field(root, "analysis_width", width) || width <= 0 ||
        width > std::numeric_limits<int32_t>::max() ||
        !ReadInt64Field(root, "analysis_height", height) || height <= 0 ||
        height > std::numeric_limits<int32_t>::max()) {
        error = "shot quality cache has invalid analysis settings";
        return false;
    }
    parsed.samples_per_second = static_cast<uint32_t>(rate);
    parsed.analysis_width = static_cast<int32_t>(width);
    parsed.analysis_height = static_cast<int32_t>(height);
    if (!ReadInt64Field(root, "median_sharpness", parsed.median_sharpness) ||
        parsed.median_sharpness < 0 ||
        !ReadInt64Field(root, "median_motion", parsed.median_motion) ||
        parsed.median_motion < 0 ||
        !ReadInt64Field(root, "median_histogram_distance",
                        parsed.median_histogram_distance) ||
        parsed.median_histogram_distance < 0) {
        error =
            "shot quality cache has no median sharpness, motion or "
            "histogram distance";
        return false;
    }
    const Value* sharpness = root.Find("sharpness");
    const Value* motion = root.Find("motion");
    const Value* histogram = root.Find("histogram");
    if (sharpness == nullptr || !sharpness->IsArray() || motion == nullptr ||
        !motion->IsArray() || histogram == nullptr || !histogram->IsArray() ||
        sharpness->AsArray().size() != motion->AsArray().size() ||
        sharpness->AsArray().size() != histogram->AsArray().size()) {
        error = "shot quality cache metric arrays are missing or mismatched";
        return false;
    }
    parsed.samples.reserve(sharpness->AsArray().size());
    for (size_t index = 0; index < sharpness->AsArray().size(); ++index) {
        ShotQualitySample sample;
        if (!sharpness->AsArray()[index].AsInt64(sample.sharpness) ||
            !motion->AsArray()[index].AsInt64(sample.motion) ||
            !histogram->AsArray()[index].AsInt64(sample.histogram_distance) ||
            sample.sharpness < 0 || sample.motion < 0 ||
            sample.histogram_distance < 0) {
            error = "shot quality cache contains an invalid metric";
            return false;
        }
        sample.time =
            RationalTime{static_cast<int64_t>(index),
                         static_cast<int32_t>(parsed.samples_per_second)};
        parsed.samples.push_back(sample);
    }
    report = std::move(parsed);
    return true;
}

bool LoadShotQuality(const std::string& path, ShotQualityReport& report,
                     std::string& error) {
    error.clear();
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "unable to open shot quality cache";
        return false;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (!input.good() && !input.eof()) {
        error = "unable to read shot quality cache";
        return false;
    }
    return DeserializeShotQuality(buffer.str(), report, error);
}

bool GenerateShotQuality(const std::string& inputPath,
                         const std::string& outputPath,
                         const std::string& mediaId,
                         const RationalTime& duration,
                         const ShotQualitySettings& settings,
                         MediaTaskContext& context, std::string& error) {
    error.clear();
    if (duration.rate <= 0 || duration.value <= 0 ||
        settings.samples_per_second == 0 ||
        settings.samples_per_second > std::numeric_limits<int32_t>::max() ||
        settings.analysis_width < 3 || settings.analysis_height < 3) {
        error = "invalid shot quality settings or source duration";
        return false;
    }
    std::error_code filesystemError;
    const std::filesystem::path destination(outputPath);
    std::filesystem::create_directories(destination.parent_path(),
                                        filesystemError);
    if (filesystemError) {
        error = "unable to create shot quality directory: " +
                filesystemError.message();
        return false;
    }

    // `fps` resamples onto the analysis grid, so sample k is the frame the
    // filter emitted for time k/samples_per_second regardless of the source's
    // own frame rate -- which is what lets ShotQualitySample::time be
    // reconstructed from the array index alone. `format=gray` gives a
    // tightly packed 8-bit plane, so the analysis functions need no stride.
    const std::string filter =
        "fps=" + std::to_string(settings.samples_per_second) +
        ",scale=" + std::to_string(settings.analysis_width) + ":" +
        std::to_string(settings.analysis_height) +
        ":flags=bilinear,format=gray";
    std::vector<std::string> storage = {
        settings.ffmpeg_path,
        "-hide_banner",
        "-loglevel",
        "error",
        "-nostdin",
        "-i",
        inputPath,
        "-map",
        "0:v:0",
        "-an",
        "-sn",
        "-vf",
        filter,
        "-f",
        "rawvideo",
        "-pix_fmt",
        "gray",
        "pipe:1",
    };
    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (std::string& value : storage) argv.push_back(value.data());
    argv.push_back(nullptr);

    int framePipe[2] = {-1, -1};
    int errorPipe[2] = {-1, -1};
    if (pipe(framePipe) != 0 || pipe(errorPipe) != 0) {
        error = "unable to create shot quality process pipes: " +
                std::string(std::strerror(errno));
        for (int descriptor : framePipe)
            if (descriptor >= 0) close(descriptor);
        for (int descriptor : errorPipe)
            if (descriptor >= 0) close(descriptor);
        return false;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, framePipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, errorPipe[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, framePipe[0]);
    posix_spawn_file_actions_addclose(&actions, errorPipe[0]);
    posix_spawn_file_actions_addclose(&actions, framePipe[1]);
    posix_spawn_file_actions_addclose(&actions, errorPipe[1]);
    pid_t process = 0;
    const int spawnResult =
        posix_spawnp(&process, storage.front().c_str(), &actions, nullptr,
                     argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(framePipe[1]);
    close(errorPipe[1]);
    if (spawnResult != 0) {
        close(framePipe[0]);
        close(errorPipe[0]);
        error = "unable to start FFmpeg: " +
                std::string(std::strerror(spawnResult));
        return false;
    }

    const size_t frameBytes = static_cast<size_t>(settings.analysis_width) *
                              static_cast<size_t>(settings.analysis_height);
    ShotQualityReport report;
    report.media_id = mediaId;
    report.samples_per_second = settings.samples_per_second;
    report.analysis_width = settings.analysis_width;
    report.analysis_height = settings.analysis_height;
    std::vector<uint8_t> previousFrame;
    std::vector<char> pendingBytes;
    std::string errorTail;
    bool frameOpen = true;
    bool errorOpen = true;
    bool cancelled = false;
    bool forced = false;
    auto cancellationStarted = std::chrono::steady_clock::time_point{};
    const long double expectedFrames =
        static_cast<long double>(duration.value) / duration.rate *
        settings.samples_per_second;
    while (frameOpen || errorOpen) {
        if (!cancelled && context.Cancelled()) {
            cancelled = true;
            cancellationStarted = std::chrono::steady_clock::now();
            kill(process, SIGTERM);
        }
        if (cancelled && !forced &&
            std::chrono::steady_clock::now() - cancellationStarted >
                std::chrono::seconds(2)) {
            forced = true;
            kill(process, SIGKILL);
        }
        pollfd descriptors[2] = {
            {framePipe[0], static_cast<short>(frameOpen ? POLLIN : 0), 0},
            {errorPipe[0], static_cast<short>(errorOpen ? POLLIN : 0), 0},
        };
        const int pollResult = poll(descriptors, 2, 100);
        if (pollResult < 0 && errno != EINTR) break;
        char bytes[65536];
        if (frameOpen &&
            (descriptors[0].revents & (POLLIN | POLLHUP | POLLERR))) {
            const ssize_t count = read(framePipe[0], bytes, sizeof(bytes));
            if (count <= 0) {
                frameOpen = false;
                close(framePipe[0]);
            } else {
                pendingBytes.insert(pendingBytes.end(), bytes, bytes + count);
                size_t offset = 0;
                while (pendingBytes.size() - offset >= frameBytes) {
                    const uint8_t* frame = reinterpret_cast<const uint8_t*>(
                        pendingBytes.data() + offset);
                    ShotQualitySample sample;
                    sample.time = RationalTime{
                        static_cast<int64_t>(report.samples.size()),
                        static_cast<int32_t>(settings.samples_per_second)};
                    sample.sharpness =
                        FrameSharpnessMetric(frame, settings.analysis_width,
                                             settings.analysis_height);
                    sample.motion =
                        previousFrame.empty()
                            ? 0
                            : FrameMotionMetric(previousFrame.data(), frame,
                                                settings.analysis_width,
                                                settings.analysis_height);
                    sample.histogram_distance =
                        previousFrame.empty() ? 0
                                              : FrameHistogramDistanceMetric(
                                                    previousFrame.data(), frame,
                                                    settings.analysis_width,
                                                    settings.analysis_height);
                    report.samples.push_back(sample);
                    previousFrame.assign(frame, frame + frameBytes);
                    offset += frameBytes;
                }
                pendingBytes.erase(pendingBytes.begin(),
                                   pendingBytes.begin() + offset);
                context.SetProgress(
                    expectedFrames <= 0.0L
                        ? 0.0
                        : static_cast<double>(report.samples.size() /
                                              expectedFrames),
                    "Analyse image");
            }
        }
        if (errorOpen &&
            (descriptors[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            const ssize_t count = read(errorPipe[0], bytes, sizeof(bytes));
            if (count <= 0) {
                errorOpen = false;
                close(errorPipe[0]);
            } else {
                AppendTail(errorTail, bytes, static_cast<size_t>(count));
            }
        }
    }
    int status = 0;
    while (waitpid(process, &status, 0) < 0 && errno == EINTR) {
    }
    if (frameOpen) close(framePipe[0]);
    if (errorOpen) close(errorPipe[0]);
    if (cancelled || context.Cancelled()) {
        error = "shot quality analysis cancelled";
        return false;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        error = errorTail.empty() ? "FFmpeg shot quality analysis failed"
                                  : errorTail;
        return false;
    }
    if (report.samples.empty()) {
        error = "video stream produced no analysis samples";
        return false;
    }
    std::vector<int64_t> allSharpness;
    std::vector<int64_t> allMotion;
    std::vector<int64_t> allHistogram;
    allSharpness.reserve(report.samples.size());
    allMotion.reserve(report.samples.size());
    allHistogram.reserve(report.samples.size());
    for (size_t index = 0; index < report.samples.size(); ++index) {
        allSharpness.push_back(report.samples[index].sharpness);
        // The first sample has no predecessor, so its zero is an absence of
        // measurement rather than a still frame; counting it would drag the
        // median down on a short source. Both change metrics share that.
        if (index > 0) {
            allMotion.push_back(report.samples[index].motion);
            allHistogram.push_back(report.samples[index].histogram_distance);
        }
    }
    report.median_sharpness =
        ShotQualityPercentile(std::move(allSharpness), 50);
    report.median_motion = ShotQualityPercentile(std::move(allMotion), 50);
    report.median_histogram_distance =
        ShotQualityPercentile(std::move(allHistogram), 50);

    // Written through a neighbouring temporary and renamed, so a cancelled
    // or crashed run never leaves a half-parsed cache behind for the next
    // reader to trust.
    const std::filesystem::path temporary =
        destination.parent_path() /
        (destination.stem().string() + ".partial-" + GenerateUlid() +
         destination.extension().string());
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        const std::string json = SerializeShotQuality(report);
        file.write(json.data(), static_cast<std::streamsize>(json.size()));
        if (!file) {
            file.close();
            std::filesystem::remove(temporary, filesystemError);
            error = "unable to write shot quality cache";
            return false;
        }
    }
    std::filesystem::rename(temporary, destination, filesystemError);
    if (filesystemError) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        error = "unable to publish shot quality cache: " +
                filesystemError.message();
        return false;
    }
    return true;
}

namespace {

Value TimeValue(const RationalTime& time) {
    Value value = Value::MakeObject();
    value.Set("value", Value::MakeInt(time.value));
    value.Set("rate", Value::MakeInt(time.rate));
    return value;
}

// Timeline span left visible once every clip on a higher-index video track
// is subtracted. Export.cc composites video tracks in ascending index order,
// each over the accumulated result, so a higher index is literally on top --
// this mirrors that rule rather than guessing at it. All arithmetic stays in
// RationalTime, so a clip covered to the frame reports exactly zero.
RationalTime VisibleDuration(const Document& document,
                             const DocumentTrack& track,
                             const DocumentClip& clip) {
    const RationalTime clipEnd = clip.timeline_in.add(clip.duration);
    std::vector<std::pair<RationalTime, RationalTime>> gaps;
    gaps.emplace_back(clip.timeline_in, clipEnd);
    for (const DocumentTrack& above : document.sequence.tracks) {
        if (above.kind != "video" || above.index <= track.index) continue;
        if (!above.visible) continue;
        for (const DocumentClip& cover : above.clips) {
            const RationalTime coverStart = cover.timeline_in;
            const RationalTime coverEnd = coverStart.add(cover.duration);
            std::vector<std::pair<RationalTime, RationalTime>> next;
            for (const auto& span : gaps) {
                if (coverEnd.compare(span.first) <= 0 ||
                    coverStart.compare(span.second) >= 0) {
                    next.push_back(span);
                    continue;
                }
                if (span.first.compare(coverStart) < 0)
                    next.emplace_back(span.first, coverStart);
                if (span.second.compare(coverEnd) > 0)
                    next.emplace_back(coverEnd, span.second);
            }
            gaps = std::move(next);
        }
    }
    RationalTime visible{0, clip.duration.rate > 0 ? clip.duration.rate : 1};
    for (const auto& span : gaps)
        visible = visible.add(span.second.sub(span.first));
    return visible;
}

}  // namespace

std::string DescribeShotQualityForAgent(
    const Document& document, const std::map<Ulid, ShotQualityReport>& reports,
    const ShotQualityThresholds& thresholds,
    const ShotSegmentationSettings& segmentation) {
    Value graded = Value::MakeArray();
    Value ungraded = Value::MakeArray();
    for (const DocumentTrack& track : document.sequence.tracks) {
        if (track.kind != "video") continue;
        for (const DocumentClip& clip : track.clips) {
            const auto found = reports.find(clip.source_id);
            ClipShotQuality summary;
            std::string error;
            if (found == reports.end() ||
                !SummarizeClipShotQuality(clip, found->second, thresholds,
                                          summary, error)) {
                Value entry = Value::MakeObject();
                entry.Set("clip_id", Value::MakeString(clip.id));
                entry.Set("source_id", Value::MakeString(clip.source_id));
                entry.Set("track_id", Value::MakeString(track.id));
                entry.Set("reason",
                          Value::MakeString(found == reports.end()
                                                ? "no cached shot quality "
                                                  "analysis for this source"
                                                : error));
                ungraded.Push(std::move(entry));
                continue;
            }
            Value entry = Value::MakeObject();
            entry.Set("clip_id", Value::MakeString(summary.clip_id));
            entry.Set("source_id", Value::MakeString(summary.source_id));
            entry.Set("track_id", Value::MakeString(track.id));
            entry.Set("timeline_in", TimeValue(clip.timeline_in));
            entry.Set("duration", TimeValue(clip.duration));
            entry.Set("samples", Value::MakeInt(summary.samples));
            entry.Set("sharpness",
                      Value::MakeString(SharpnessGradeName(summary.sharpness)));
            entry.Set(
                "steadiness",
                Value::MakeString(SteadinessGradeName(summary.steadiness)));
            const RationalTime visible = VisibleDuration(document, track, clip);
            const bool fullyCovered = visible.compare(RationalTime{0, 1}) == 0;
            entry.Set("clean", Value::MakeBool(summary.clean));
            entry.Set("visible", TimeValue(visible));
            entry.Set("fully_covered", Value::MakeBool(fullyCovered));
            // What a caller should actually act on: measured badly *and*
            // still on screen. Kept as its own field so "clean" keeps
            // meaning "measured well" and nothing else.
            entry.Set("needs_attention",
                      Value::MakeBool(!summary.clean && !fullyCovered));
            entry.Set("sharpness_median",
                      Value::MakeInt(summary.sharpness_median));
            entry.Set("sharpness_worst",
                      Value::MakeInt(summary.sharpness_worst));
            entry.Set("source_median_sharpness",
                      Value::MakeInt(summary.source_median_sharpness));
            entry.Set("motion_median", Value::MakeInt(summary.motion_median));
            entry.Set("motion_peak", Value::MakeInt(summary.motion_peak));
            entry.Set("source_median_motion",
                      Value::MakeInt(summary.source_median_motion));
            graded.Push(std::move(entry));
        }
    }

    // What the rushes hold, as opposed to what the timeline already uses.
    // Keyed by source rather than by clip because a shot is a property of
    // the media: two clips cut from the same take share these, and a caller
    // looking for an unused shot has no clip to hang the question on.
    Value sources = Value::MakeArray();
    for (const auto& entry : reports) {
        const ShotQualityReport& report = entry.second;
        Value item = Value::MakeObject();
        item.Set("source_id", Value::MakeString(entry.first));
        item.Set("samples_per_second",
                 Value::MakeInt(report.samples_per_second));
        item.Set("median_sharpness", Value::MakeInt(report.median_sharpness));
        item.Set("median_motion", Value::MakeInt(report.median_motion));
        item.Set("median_histogram_distance",
                 Value::MakeInt(report.median_histogram_distance));
        Value shotList = Value::MakeArray();
        for (const SourceShot& shot : DetectSourceShots(report, segmentation)) {
            Value described = Value::MakeObject();
            // Source-domain times, so these can be used as a clip's
            // source_in and duration without conversion.
            described.Set("start", TimeValue(shot.start));
            described.Set("end", TimeValue(shot.end));
            described.Set("duration", TimeValue(shot.end.sub(shot.start)));
            // The frame to pull when one still has to stand for the shot.
            described.Set("keyframe", TimeValue(shot.keyframe));
            described.Set("samples", Value::MakeInt(shot.sample_count));
            described.Set("median_sharpness",
                          Value::MakeInt(shot.median_sharpness));
            described.Set("median_motion", Value::MakeInt(shot.median_motion));
            shotList.Push(std::move(described));
        }
        item.Set("shots", std::move(shotList));
        sources.Push(std::move(item));
    }

    Value limits = Value::MakeObject();
    limits.Set("blurry_ratio_percent",
               Value::MakeInt(thresholds.blurry_ratio_percent));
    limits.Set("soft_ratio_percent",
               Value::MakeInt(thresholds.soft_ratio_percent));
    limits.Set("moving_ratio_percent",
               Value::MakeInt(thresholds.moving_ratio_percent));
    limits.Set("shaky_ratio_percent",
               Value::MakeInt(thresholds.shaky_ratio_percent));
    limits.Set("motion_floor", Value::MakeInt(thresholds.motion_floor));

    Value cuts = Value::MakeObject();
    cuts.Set("cut_motion_floor", Value::MakeInt(segmentation.cut_motion_floor));
    cuts.Set("cut_histogram_floor",
             Value::MakeInt(segmentation.cut_histogram_floor));
    cuts.Set("cut_histogram_ratio_percent",
             Value::MakeInt(segmentation.cut_histogram_ratio_percent));
    cuts.Set("cut_motion_local_percent",
             Value::MakeInt(segmentation.cut_motion_local_percent));
    cuts.Set("cut_local_window_samples",
             Value::MakeInt(segmentation.cut_local_window_samples));
    cuts.Set("minimum_samples", Value::MakeInt(segmentation.minimum_samples));

    Value root = Value::MakeObject();
    root.Set("timeline_id", Value::MakeString(document.sequence.id));
    root.Set("timeline_name", Value::MakeString(document.sequence.name));
    root.Set("metric_scale", Value::MakeInt(kShotQualityMetricScale));
    root.Set("thresholds", std::move(limits));
    root.Set("segmentation", std::move(cuts));
    root.Set("clips", std::move(graded));
    root.Set("unanalyzed", std::move(ungraded));
    root.Set("sources", std::move(sources));
    return root.Dump();
}
