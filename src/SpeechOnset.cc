#include "SpeechOnset.h"

#include "Json.h"
#include "Ulid.h"

#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>

extern char** environ;

namespace {

using mcp_json::Value;

// Keeps the last of FFmpeg's chatter, which is where its reason for failing
// is. Mirrors ShotQuality's helper of the same purpose.
void AppendTail(std::string& tail, const char* bytes, size_t count) {
    tail.append(bytes, count);
    constexpr size_t kMaximum = 4096;
    if (tail.size() > kMaximum) tail.erase(0, tail.size() - kMaximum);
}

bool ReadInt64Field(const Value& object, const std::string& key,
                    int64_t& output) {
    const Value* field = object.Find(key);
    return field != nullptr && field->AsInt64(output);
}

bool ReadTimeField(const Value& object, const std::string& key,
                   RationalTime& output) {
    const Value* field = object.Find(key);
    if (field == nullptr || !field->IsObject()) return false;
    int64_t value = 0;
    int64_t rate = 0;
    if (!ReadInt64Field(*field, "value", value) ||
        !ReadInt64Field(*field, "rate", rate) || rate <= 0 ||
        rate > std::numeric_limits<int32_t>::max())
        return false;
    output = RationalTime{value, static_cast<int32_t>(rate)};
    return true;
}

// Windows spanned by `milliseconds` on the report's grid, at least one: a
// sustain or pre-roll shorter than a window still has to mean something.
int64_t WindowsForMilliseconds(int64_t milliseconds,
                               uint32_t windowsPerSecond) {
    if (windowsPerSecond == 0) return 1;
    const int64_t windows =
        (milliseconds * static_cast<int64_t>(windowsPerSecond) + 999) / 1000;
    return windows > 0 ? windows : 1;
}

bool ValidateGroupThresholds(const SpeechOnsetThresholds& thresholds,
                             std::string& error) {
    if (thresholds.speech_ratio_percent < 1 ||
        thresholds.speech_ratio_percent > 100) {
        error = "speech_ratio_percent must be between 1 and 100";
        return false;
    }
    if (thresholds.group_gap_milliseconds < 20 ||
        thresholds.group_gap_milliseconds > 60000) {
        error = "group_gap_milliseconds must be between 20 and 60000";
        return false;
    }
    if (thresholds.group_floor_db < 0 || thresholds.group_floor_db > 120) {
        error = "group_floor_db must be between 0 and 120";
        return false;
    }
    if (thresholds.dominant_percentile < 1 ||
        thresholds.dominant_percentile > 100) {
        error = "dominant_percentile must be between 1 and 100";
        return false;
    }
    if (thresholds.dominance_db < 0 || thresholds.dominance_db > 120) {
        error = "dominance_db must be between 0 and 120";
        return false;
    }
    if (thresholds.dominant_sustain_windows < 1 ||
        thresholds.dominant_sustain_windows > 100000) {
        error = "dominant_sustain_windows must be between 1 and 100000";
        return false;
    }
    return true;
}

unsigned __int128 IntegerSquareRoot(unsigned __int128 value) {
    unsigned __int128 root = 0;
    unsigned __int128 bit = static_cast<unsigned __int128>(1) << 126;
    while (bit > value) bit >>= 2;
    while (bit != 0) {
        if (value >= root + bit) {
            value -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return root;
}

SpeechGroup GroupForBounds(const std::vector<int64_t>& levels, int64_t first,
                           int64_t last, int32_t grid) {
    SpeechGroup group;
    group.start = RationalTime{first, grid};
    group.end = RationalTime{last, grid};
    unsigned __int128 squares = 0;
    int64_t peak = 0;
    for (int64_t index = first; index < last; ++index) {
        const int64_t level = levels[static_cast<size_t>(index)];
        const unsigned __int128 positive =
            static_cast<unsigned __int128>(level > 0 ? level : 0);
        squares += positive * positive;
        peak = std::max(peak, level);
    }
    const uint64_t count = static_cast<uint64_t>(last - first);
    const int64_t rms =
        count == 0 ? 0
                   : static_cast<int64_t>(IntegerSquareRoot(
                         squares / static_cast<unsigned __int128>(count)));
    group.level_dbfs = SpeechLevelDbfs(rms);
    group.peak_dbfs = SpeechLevelDbfs(peak);
    return group;
}

}  // namespace

void RunningRmsLevel::Add(const int16_t* samples, size_t count) {
    if (samples == nullptr) return;
    for (size_t index = 0; index < count; ++index) {
        const unsigned __int128 value = static_cast<unsigned __int128>(
            static_cast<int64_t>(samples[index]) * samples[index]);
        total_ += value;
    }
    count_ += count;
}

int64_t RunningRmsLevel::Level() const {
    if (count_ == 0) return 0;
    const unsigned __int128 mean =
        total_ / static_cast<unsigned __int128>(count_);
    // Integer square root of the mean square, then scaled. Both steps stay in
    // __int128 so the same samples always produce the same figure, on any
    // standard library -- std::sqrt on a double would not promise that.
    unsigned __int128 root = 0;
    unsigned __int128 bit = static_cast<unsigned __int128>(1) << 30;
    unsigned __int128 remainder = mean;
    while (bit > remainder) bit >>= 2;
    while (bit != 0) {
        if (remainder >= root + bit) {
            remainder -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    // 32768 is full scale for signed 16-bit input.
    const unsigned __int128 scaled = root * kSpeechLevelScale / 32768;
    if (scaled >
        static_cast<unsigned __int128>(std::numeric_limits<int64_t>::max()))
        return std::numeric_limits<int64_t>::max();
    return static_cast<int64_t>(scaled);
}

int64_t WindowRmsLevel(const int16_t* samples, size_t count) {
    RunningRmsLevel level;
    level.Add(samples, count);
    return level.Level();
}

int64_t SpeechLevelPercentile(std::vector<int64_t> values, int percent) {
    if (values.empty()) return 0;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    std::sort(values.begin(), values.end());
    // Nearest-rank: rank = ceil(percent/100 * n), clamped into the array.
    size_t rank = static_cast<size_t>(
        (static_cast<int64_t>(percent) * static_cast<int64_t>(values.size()) +
         99) /
        100);
    if (rank == 0) rank = 1;
    if (rank > values.size()) rank = values.size();
    return values[rank - 1];
}

int64_t FirstSustainedWindow(const std::vector<int64_t>& levels,
                             int64_t firstWindow, int64_t threshold,
                             int64_t sustainWindows) {
    if (sustainWindows < 1) sustainWindows = 1;
    if (firstWindow < 0) firstWindow = 0;
    const int64_t count = static_cast<int64_t>(levels.size());
    int64_t run = 0;
    for (int64_t index = firstWindow; index < count; ++index) {
        if (levels[static_cast<size_t>(index)] >= threshold) {
            ++run;
            if (run >= sustainWindows) return index - run + 1;
        } else {
            run = 0;
        }
    }
    return -1;
}

int64_t SpeechLevelDbfs(int64_t level) {
    if (level <= 0) return -120;
    if (level >= kSpeechLevelScale) return 0;
    const long double ratio = static_cast<long double>(level) /
                              static_cast<long double>(kSpeechLevelScale);
    const int64_t result =
        static_cast<int64_t>(std::llround(20.0L * std::log10(ratio)));
    return std::max<int64_t>(-120, std::min<int64_t>(0, result));
}

bool BuildSpeechGroups(const std::vector<int64_t>& levels,
                       uint32_t windowsPerSecond, int64_t speechLevel,
                       int64_t noiseFloor,
                       const SpeechOnsetThresholds& thresholds,
                       std::vector<SpeechGroup>& groups, std::string& error) {
    groups.clear();
    error.clear();
    if (windowsPerSecond == 0 ||
        windowsPerSecond >
            static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) {
        error = "speech group analysis grid is invalid";
        return false;
    }
    if (!ValidateGroupThresholds(thresholds, error)) return false;
    if (speechLevel < 0 || noiseFloor < 0) {
        error = "speech group levels cannot be negative";
        return false;
    }
    if (levels.empty() ||
        speechLevel * 100 <
            noiseFloor * thresholds.minimum_dynamic_range_percent)
        return true;

    const int64_t floorDbfs = std::min<int64_t>(
        0, SpeechLevelDbfs(noiseFloor) + thresholds.group_floor_db);
    const int64_t gapWindows = WindowsForMilliseconds(
        thresholds.group_gap_milliseconds, windowsPerSecond);
    const int64_t count = static_cast<int64_t>(levels.size());
    int64_t groupStart = -1;
    int64_t lastVoiceEnd = -1;
    for (int64_t index = 0; index < count; ++index) {
        if (SpeechLevelDbfs(levels[static_cast<size_t>(index)]) < floorDbfs)
            continue;
        if (groupStart < 0) {
            groupStart = index;
        } else if (index - lastVoiceEnd >= gapWindows) {
            groups.push_back(
                GroupForBounds(levels, groupStart, lastVoiceEnd,
                               static_cast<int32_t>(windowsPerSecond)));
            groupStart = index;
        }
        lastVoiceEnd = index + 1;
    }
    if (groupStart >= 0) {
        groups.push_back(
            GroupForBounds(levels, groupStart, lastVoiceEnd,
                           static_cast<int32_t>(windowsPerSecond)));
    }
    return true;
}

bool SummarizeClipSpeechOnset(const DocumentClip& clip,
                              const SpeechOnsetReport& report,
                              const SpeechOnsetThresholds& thresholds,
                              const MediaRate& sequenceRate,
                              ClipSpeechOnset& summary, std::string& error) {
    error.clear();
    summary = ClipSpeechOnset{};
    summary.clip_id = clip.id;
    summary.source_id = clip.source_id;
    if (clip.source_id != report.media_id) {
        error = "speech onset report belongs to another source";
        return false;
    }
    if (report.windows_per_second == 0) {
        error = "speech onset report has no analysis grid";
        return false;
    }
    if (sequenceRate.num <= 0 || sequenceRate.den <= 0) {
        error = "sequence frame rate is invalid";
        return false;
    }
    if (!ValidateGroupThresholds(thresholds, error)) return false;
    const int32_t grid = static_cast<int32_t>(report.windows_per_second);
    summary.onset = RationalTime{0, grid};
    summary.lead_in = RationalTime{0, grid};
    summary.suggested_trim = RationalTime{0, sequenceRate.num};

    // Window bounds of the clip's own source range, on the report's grid.
    const RationalTime inPoint = clip.source_in.rescale(grid);
    const RationalTime outPoint =
        clip.source_in.add(clip.duration).rescale(grid);
    int64_t firstWindow = inPoint.value;
    int64_t lastWindow = outPoint.value;
    if (firstWindow < 0) firstWindow = 0;
    if (lastWindow > static_cast<int64_t>(report.levels.size()))
        lastWindow = static_cast<int64_t>(report.levels.size());
    if (lastWindow <= firstWindow) {
        summary.detail = "clip holds no analysis window";
        return true;
    }
    summary.windows = static_cast<int32_t>(lastWindow - firstWindow);

    // Levels are taken from the clip's own range, not the source's. A rush
    // holding one loud take and one quiet one would otherwise grade the quiet
    // clip against the loud one's level and find no speech in it at all.
    std::vector<int64_t> inside(
        report.levels.begin() + static_cast<size_t>(firstWindow),
        report.levels.begin() + static_cast<size_t>(lastWindow));
    summary.speech_level = SpeechLevelPercentile(inside, 90);
    summary.noise_floor = report.noise_floor;
    summary.threshold =
        summary.speech_level * thresholds.speech_ratio_percent / 100;

    std::vector<SpeechGroup> regrouped;
    const std::vector<SpeechGroup>* sourceGroups = &report.groups;
    if (report.groups.empty() ||
        report.group_gap_milliseconds != thresholds.group_gap_milliseconds ||
        report.group_floor_db != thresholds.group_floor_db) {
        if (!BuildSpeechGroups(report.levels, report.windows_per_second,
                               report.speech_level, report.noise_floor,
                               thresholds, regrouped, error))
            return false;
        sourceGroups = &regrouped;
    }
    for (const SpeechGroup& cached : *sourceGroups) {
        int64_t groupFirst = cached.start.rescale(grid).value;
        int64_t groupLast = cached.end.rescale(grid).value;
        groupFirst = std::max(groupFirst, firstWindow);
        groupLast = std::min(groupLast, lastWindow);
        if (groupLast <= groupFirst) continue;
        summary.groups.push_back(
            GroupForBounds(report.levels, groupFirst, groupLast, grid));
    }
    const int64_t dominantReference = SpeechLevelDbfs(SpeechLevelPercentile(
        inside, static_cast<int>(thresholds.dominant_percentile)));
    for (const SpeechGroup& group : summary.groups) {
        const int64_t groupWindows =
            group.end.rescale(grid).value - group.start.rescale(grid).value;
        if (groupWindows >= thresholds.dominant_sustain_windows &&
            group.level_dbfs >= dominantReference - thresholds.dominance_db) {
            summary.dominant_onset = group.start;
            summary.dominant_measured = true;
            break;
        }
    }

    if (summary.speech_level * 100 <
        summary.noise_floor * thresholds.minimum_dynamic_range_percent) {
        summary.detail =
            "clip has too little dynamic range to separate voice from room";
        return true;
    }
    const int64_t sustain = WindowsForMilliseconds(
        thresholds.sustain_milliseconds, report.windows_per_second);
    const int64_t onsetWindow = FirstSustainedWindow(
        report.levels, firstWindow, summary.threshold, sustain);
    if (onsetWindow < 0 || onsetWindow >= lastWindow) {
        summary.detail = "no sustained voice inside the clip";
        return true;
    }

    summary.measured = true;
    summary.onset = RationalTime{onsetWindow, grid};
    summary.lead_in = RationalTime{onsetWindow - firstWindow, grid};

    // The trim is published in whole sequence frames, floored: a floor can
    // only ever leave a frame of air in, never eat into the word. Rounding up
    // would clip an attack the pre-roll exists to protect.
    const int64_t preRollWindows = WindowsForMilliseconds(
        thresholds.pre_roll_milliseconds, report.windows_per_second);
    int64_t trimWindows = (onsetWindow - firstWindow) - preRollWindows;
    if (trimWindows < 0) trimWindows = 0;
    const int64_t toleranceWindows = WindowsForMilliseconds(
        thresholds.tolerance_milliseconds, report.windows_per_second);
    summary.tight = (onsetWindow - firstWindow) <= toleranceWindows;
    const int64_t trimFrames = trimWindows * sequenceRate.num /
                               (static_cast<int64_t>(grid) * sequenceRate.den);
    summary.suggested_trim =
        RationalTime{summary.tight ? 0 : trimFrames, sequenceRate.num};
    return true;
}

std::string SerializeSpeechOnset(const SpeechOnsetReport& report) {
    std::ostringstream output;
    output << "{\"version\":2,\"media_id\":\""
           << mcp_json::EscapeJsonString(report.media_id)
           << "\",\"windows_per_second\":" << report.windows_per_second
           << ",\"decode_sample_rate\":" << report.decode_sample_rate
           << ",\"speech_level\":" << report.speech_level
           << ",\"noise_floor\":" << report.noise_floor
           << ",\"group_gap_milliseconds\":" << report.group_gap_milliseconds
           << ",\"group_floor_db\":" << report.group_floor_db
           << ",\"groups\":[";
    for (size_t index = 0; index < report.groups.size(); ++index) {
        if (index) output << ',';
        const SpeechGroup& group = report.groups[index];
        output << "{\"start\":{\"value\":" << group.start.value
               << ",\"rate\":" << group.start.rate
               << "},\"end\":{\"value\":" << group.end.value
               << ",\"rate\":" << group.end.rate
               << "},\"level_dbfs\":" << group.level_dbfs
               << ",\"peak_dbfs\":" << group.peak_dbfs << '}';
    }
    output << "],\"levels\":[";
    for (size_t index = 0; index < report.levels.size(); ++index) {
        if (index) output << ',';
        output << report.levels[index];
    }
    output << "]}";
    return output.str();
}

bool DeserializeSpeechOnset(const std::string& json, SpeechOnsetReport& report,
                            std::string& error) {
    error.clear();
    report = SpeechOnsetReport{};
    Value root;
    std::string parseError;
    if (!Value::Parse(json, root, parseError) || !root.IsObject()) {
        error = "malformed speech onset cache: " + parseError;
        return false;
    }
    int64_t version = 0;
    if (!ReadInt64Field(root, "version", version) ||
        (version != 1 && version != 2)) {
        error = "unsupported speech onset cache version";
        return false;
    }
    const Value* mediaId = root.Find("media_id");
    if (mediaId == nullptr || !mediaId->IsString()) {
        error = "speech onset cache has no media_id";
        return false;
    }
    report.media_id = mediaId->AsString();
    int64_t windows = 0;
    int64_t rate = 0;
    int64_t speech = 0;
    int64_t floorLevel = 0;
    if (!ReadInt64Field(root, "windows_per_second", windows) || windows <= 0 ||
        !ReadInt64Field(root, "decode_sample_rate", rate) || rate <= 0 ||
        !ReadInt64Field(root, "speech_level", speech) ||
        !ReadInt64Field(root, "noise_floor", floorLevel)) {
        error = "speech onset cache is missing a field";
        return false;
    }
    report.windows_per_second = static_cast<uint32_t>(windows);
    report.decode_sample_rate = static_cast<uint32_t>(rate);
    report.speech_level = speech;
    report.noise_floor = floorLevel;
    const Value* levels = root.Find("levels");
    if (levels == nullptr || !levels->IsArray()) {
        error = "speech onset cache has no levels";
        return false;
    }
    report.levels.reserve(levels->AsArray().size());
    for (const Value& item : levels->AsArray()) {
        int64_t value = 0;
        if (!item.AsInt64(value)) {
            error = "speech onset cache holds a non-integer level";
            return false;
        }
        report.levels.push_back(value);
    }
    SpeechOnsetThresholds cacheThresholds;
    if (version == 2) {
        int64_t groupGap = 0;
        int64_t groupFloorDb = 0;
        if (!ReadInt64Field(root, "group_gap_milliseconds", groupGap) ||
            !ReadInt64Field(root, "group_floor_db", groupFloorDb)) {
            error = "speech onset cache has no group settings";
            return false;
        }
        cacheThresholds.group_gap_milliseconds = groupGap;
        cacheThresholds.group_floor_db = groupFloorDb;
        if (!ValidateGroupThresholds(cacheThresholds, error)) {
            error = "invalid speech onset cache: " + error;
            return false;
        }
        report.group_gap_milliseconds = groupGap;
        report.group_floor_db = groupFloorDb;
        const Value* groups = root.Find("groups");
        if (groups == nullptr || !groups->IsArray()) {
            error = "speech onset cache has no groups";
            return false;
        }
        RationalTime previousEnd{
            0, static_cast<int32_t>(report.windows_per_second)};
        for (const Value& item : groups->AsArray()) {
            SpeechGroup group;
            if (!item.IsObject() ||
                !ReadTimeField(item, "start", group.start) ||
                !ReadTimeField(item, "end", group.end) ||
                !ReadInt64Field(item, "level_dbfs", group.level_dbfs) ||
                !ReadInt64Field(item, "peak_dbfs", group.peak_dbfs)) {
                error = "speech onset cache holds a malformed group";
                return false;
            }
            const RationalTime start = group.start.rescale(
                static_cast<int32_t>(report.windows_per_second));
            const RationalTime end = group.end.rescale(
                static_cast<int32_t>(report.windows_per_second));
            if (start.value < previousEnd.value || end.value <= start.value ||
                end.value > static_cast<int64_t>(report.levels.size()) ||
                group.level_dbfs < -120 || group.level_dbfs > 0 ||
                group.peak_dbfs < group.level_dbfs || group.peak_dbfs > 0) {
                error = "speech onset cache holds an invalid group";
                return false;
            }
            group.start = start;
            group.end = end;
            report.groups.push_back(group);
            previousEnd = end;
        }
    } else {
        report.group_gap_milliseconds = cacheThresholds.group_gap_milliseconds;
        if (!BuildSpeechGroups(report.levels, report.windows_per_second,
                               report.speech_level, report.noise_floor,
                               cacheThresholds, report.groups, error)) {
            error = "cannot upgrade speech onset cache: " + error;
            return false;
        }
    }
    return true;
}

bool LoadSpeechOnset(const std::string& path, SpeechOnsetReport& report,
                     std::string& error) {
    error.clear();
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "no cached speech onset analysis for this source";
        return false;
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    return DeserializeSpeechOnset(contents.str(), report, error);
}

bool GenerateSpeechOnset(const std::string& inputPath,
                         const std::string& outputPath,
                         const std::string& mediaId,
                         const SpeechOnsetSettings& settings,
                         MediaTaskContext& context, std::string& error) {
    error.clear();
    if (!ValidateGroupThresholds(settings.thresholds, error)) return false;
    if (settings.windows_per_second == 0 || settings.decode_sample_rate == 0 ||
        settings.decode_sample_rate % settings.windows_per_second != 0) {
        // An exact division is required, not convenient: a window that spans
        // a fractional number of samples would make the envelope depend on
        // accumulated rounding, and two runs would stop agreeing.
        error =
            "decode sample rate must be a whole multiple of the window rate";
        return false;
    }
    std::error_code filesystemError;
    const std::filesystem::path destination(outputPath);
    std::filesystem::create_directories(destination.parent_path(),
                                        filesystemError);
    if (filesystemError) {
        error = "unable to create speech onset directory: " +
                filesystemError.message();
        return false;
    }

    const std::string rate = std::to_string(settings.decode_sample_rate);
    std::vector<std::string> storage = {
        settings.ffmpeg_path,
        "-hide_banner",
        "-loglevel",
        "error",
        "-nostdin",
        "-i",
        inputPath,
        "-map",
        "0:a:0",
        "-vn",
        "-sn",
        "-ac",
        "1",
        "-ar",
        rate,
        "-f",
        "s16le",
        "pipe:1",
    };
    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (std::string& value : storage) argv.push_back(value.data());
    argv.push_back(nullptr);

    int audioPipe[2] = {-1, -1};
    int errorPipe[2] = {-1, -1};
    if (pipe(audioPipe) != 0 || pipe(errorPipe) != 0) {
        error = "unable to create speech onset process pipes: " +
                std::string(std::strerror(errno));
        for (int descriptor : audioPipe)
            if (descriptor >= 0) close(descriptor);
        for (int descriptor : errorPipe)
            if (descriptor >= 0) close(descriptor);
        return false;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, audioPipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, errorPipe[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, audioPipe[0]);
    posix_spawn_file_actions_addclose(&actions, errorPipe[0]);
    posix_spawn_file_actions_addclose(&actions, audioPipe[1]);
    posix_spawn_file_actions_addclose(&actions, errorPipe[1]);
    pid_t process = 0;
    const int spawnResult =
        posix_spawnp(&process, storage.front().c_str(), &actions, nullptr,
                     argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(audioPipe[1]);
    close(errorPipe[1]);
    if (spawnResult != 0) {
        close(audioPipe[0]);
        close(errorPipe[0]);
        error = "unable to start FFmpeg: " +
                std::string(std::strerror(spawnResult));
        return false;
    }

    const size_t windowSamples =
        settings.decode_sample_rate / settings.windows_per_second;
    const size_t windowBytes = windowSamples * sizeof(int16_t);
    SpeechOnsetReport report;
    report.media_id = mediaId;
    report.windows_per_second = settings.windows_per_second;
    report.decode_sample_rate = settings.decode_sample_rate;
    std::vector<char> pendingBytes;
    std::string errorTail;
    bool audioOpen = true;
    bool errorOpen = true;
    bool cancelled = false;
    bool forced = false;
    auto cancellationStarted = std::chrono::steady_clock::time_point{};
    while (audioOpen || errorOpen) {
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
            {audioPipe[0], static_cast<short>(audioOpen ? POLLIN : 0), 0},
            {errorPipe[0], static_cast<short>(errorOpen ? POLLIN : 0), 0},
        };
        const int pollResult = poll(descriptors, 2, 100);
        if (pollResult < 0 && errno != EINTR) break;
        char bytes[65536];
        if (audioOpen &&
            (descriptors[0].revents & (POLLIN | POLLHUP | POLLERR))) {
            const ssize_t count = read(audioPipe[0], bytes, sizeof(bytes));
            if (count <= 0) {
                audioOpen = false;
                close(audioPipe[0]);
            } else {
                pendingBytes.insert(pendingBytes.end(), bytes, bytes + count);
                size_t offset = 0;
                while (pendingBytes.size() - offset >= windowBytes) {
                    const int16_t* window = reinterpret_cast<const int16_t*>(
                        pendingBytes.data() + offset);
                    report.levels.push_back(
                        WindowRmsLevel(window, windowSamples));
                    offset += windowBytes;
                }
                pendingBytes.erase(pendingBytes.begin(),
                                   pendingBytes.begin() + offset);
                context.SetProgress(0.0, "Analyse parole");
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
    if (audioOpen) close(audioPipe[0]);
    if (errorOpen) close(errorPipe[0]);
    if (cancelled || context.Cancelled()) {
        error = "speech onset analysis cancelled";
        return false;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        error = errorTail.empty() ? "FFmpeg speech onset analysis failed"
                                  : errorTail;
        return false;
    }
    if (report.levels.empty()) {
        error = "audio stream produced no analysis windows";
        return false;
    }
    report.speech_level = SpeechLevelPercentile(report.levels, 90);
    report.noise_floor = SpeechLevelPercentile(report.levels, 5);
    report.group_gap_milliseconds = settings.thresholds.group_gap_milliseconds;
    report.group_floor_db = settings.thresholds.group_floor_db;
    if (!BuildSpeechGroups(report.levels, report.windows_per_second,
                           report.speech_level, report.noise_floor,
                           settings.thresholds, report.groups, error))
        return false;

    // Written through a neighbouring temporary and renamed, so a cancelled or
    // crashed run never leaves a half-parsed cache for the next reader to
    // trust. rename, not link: the delivery volumes are exFAT, which has no
    // hard links (see Exporter::Run).
    const std::filesystem::path temporary =
        destination.parent_path() /
        (destination.stem().string() + ".partial-" + GenerateUlid() +
         destination.extension().string());
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        const std::string json = SerializeSpeechOnset(report);
        file.write(json.data(), static_cast<std::streamsize>(json.size()));
        if (!file) {
            file.close();
            std::filesystem::remove(temporary, filesystemError);
            error = "unable to write speech onset cache";
            return false;
        }
    }
    std::filesystem::rename(temporary, destination, filesystemError);
    if (filesystemError) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        error = "unable to publish speech onset cache: " +
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

}  // namespace

std::string DescribeSpeechOnsetForAgent(
    const Document& document, const std::map<Ulid, SpeechOnsetReport>& reports,
    const SpeechOnsetThresholds& thresholds) {
    const bool hasSolo = std::any_of(
        document.sequence.tracks.begin(), document.sequence.tracks.end(),
        [](const DocumentTrack& track) {
            return track.kind == "audio" && track.solo;
        });
    Value measured = Value::MakeArray();
    Value unanalyzed = Value::MakeArray();
    for (const DocumentTrack& track : document.sequence.tracks) {
        if (track.kind != "audio" || track.muted || (hasSolo && !track.solo))
            continue;
        for (const DocumentClip& clip : track.clips) {
            const auto found = reports.find(clip.source_id);
            ClipSpeechOnset summary;
            std::string error;
            if (found == reports.end() ||
                !SummarizeClipSpeechOnset(clip, found->second, thresholds,
                                          document.sequence.frame_rate, summary,
                                          error)) {
                Value entry = Value::MakeObject();
                entry.Set("clip_id", Value::MakeString(clip.id));
                entry.Set("source_id", Value::MakeString(clip.source_id));
                entry.Set("track_id", Value::MakeString(track.id));
                entry.Set("reason",
                          Value::MakeString(
                              found == reports.end()
                                  ? "no cached speech onset analysis for this "
                                    "source"
                                  : error));
                unanalyzed.Push(std::move(entry));
                continue;
            }
            Value entry = Value::MakeObject();
            entry.Set("clip_id", Value::MakeString(summary.clip_id));
            entry.Set("source_id", Value::MakeString(summary.source_id));
            entry.Set("track_id", Value::MakeString(track.id));
            entry.Set("link_group_id", Value::MakeString(clip.link_group_id));
            entry.Set("timeline_in", TimeValue(clip.timeline_in));
            entry.Set("source_in", TimeValue(clip.source_in));
            entry.Set("windows", Value::MakeInt(summary.windows));
            entry.Set("measured", Value::MakeBool(summary.measured));
            Value groups = Value::MakeArray();
            for (const SpeechGroup& group : summary.groups) {
                Value groupValue = Value::MakeObject();
                groupValue.Set("start", TimeValue(group.start));
                groupValue.Set("end", TimeValue(group.end));
                groupValue.Set("level_dbfs", Value::MakeInt(group.level_dbfs));
                groupValue.Set("peak_dbfs", Value::MakeInt(group.peak_dbfs));
                groups.Push(std::move(groupValue));
            }
            entry.Set("groups", std::move(groups));
            if (summary.measured) {
                entry.Set("onset", TimeValue(summary.onset));
                entry.Set("dominant_onset",
                          summary.dominant_measured
                              ? TimeValue(summary.dominant_onset)
                              : Value::MakeNull());
                entry.Set("lead_in", TimeValue(summary.lead_in));
                entry.Set("suggested_trim", TimeValue(summary.suggested_trim));
                entry.Set("tight", Value::MakeBool(summary.tight));
            } else {
                entry.Set("detail", Value::MakeString(summary.detail));
            }
            entry.Set("speech_level", Value::MakeInt(summary.speech_level));
            entry.Set("noise_floor", Value::MakeInt(summary.noise_floor));
            entry.Set("threshold", Value::MakeInt(summary.threshold));
            measured.Push(std::move(entry));
        }
    }
    Value limits = Value::MakeObject();
    limits.Set("speech_ratio_percent",
               Value::MakeInt(thresholds.speech_ratio_percent));
    limits.Set("sustain_milliseconds",
               Value::MakeInt(thresholds.sustain_milliseconds));
    limits.Set("minimum_dynamic_range_percent",
               Value::MakeInt(thresholds.minimum_dynamic_range_percent));
    limits.Set("pre_roll_milliseconds",
               Value::MakeInt(thresholds.pre_roll_milliseconds));
    limits.Set("tolerance_milliseconds",
               Value::MakeInt(thresholds.tolerance_milliseconds));
    limits.Set("group_gap_milliseconds",
               Value::MakeInt(thresholds.group_gap_milliseconds));
    limits.Set("group_floor_db", Value::MakeInt(thresholds.group_floor_db));
    limits.Set("dominant_percentile",
               Value::MakeInt(thresholds.dominant_percentile));
    limits.Set("dominance_db", Value::MakeInt(thresholds.dominance_db));
    limits.Set("dominant_sustain_windows",
               Value::MakeInt(thresholds.dominant_sustain_windows));
    Value root = Value::MakeObject();
    root.Set("timeline_id", Value::MakeString(document.sequence.id));
    root.Set("timeline_name", Value::MakeString(document.sequence.name));
    root.Set("level_scale", Value::MakeInt(kSpeechLevelScale));
    root.Set("thresholds", std::move(limits));
    root.Set("clips", std::move(measured));
    root.Set("unanalyzed", std::move(unanalyzed));
    return root.Dump();
}
