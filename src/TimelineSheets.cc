#include "TimelineSheets.h"

#include "ColorManagement.h"
#include "Json.h"
#include "Ulid.h"

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <exception>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <utility>

extern char** environ;

namespace {

using mcp_json::Value;

struct VisibleSegment {
    const DocumentClip* clip = nullptr;
    int32_t track_index = 0;
    RationalTime start;
    RationalTime end;
};

Value TimeValue(const RationalTime& time) {
    Value value = Value::MakeObject();
    value.Set("value", Value::MakeInt(time.value));
    value.Set("rate", Value::MakeInt(time.rate));
    return value;
}

bool ClipEnd(const DocumentClip& clip, RationalTime& output,
             std::string& error) {
    if (clip.timeline_in.rate <= 0 || clip.duration.rate <= 0 ||
        clip.source_in.rate <= 0) {
        error = "clip '" + clip.id + "' has a non-positive time rate";
        return false;
    }
    if (clip.timeline_in.value < 0 || clip.duration.value < 0 ||
        clip.source_in.value < 0) {
        error = "clip '" + clip.id + "' has a negative time extent";
        return false;
    }
    try {
        output = clip.timeline_in.add(clip.duration);
    } catch (const std::exception&) {
        error = "clip '" + clip.id + "' timeline extent overflows";
        return false;
    }
    return true;
}

bool VisibleSegments(const Document& document,
                     std::vector<VisibleSegment>& output, std::string& error) {
    output.clear();
    std::vector<RationalTime> boundaries;
    for (const DocumentTrack& track : document.sequence.tracks) {
        if (track.kind != "video" || !track.visible) continue;
        for (const DocumentClip& clip : track.clips) {
            RationalTime end;
            if (!ClipEnd(clip, end, error)) return false;
            if (clip.duration.value == 0) continue;
            boundaries.push_back(clip.timeline_in);
            boundaries.push_back(end);
        }
    }
    std::sort(boundaries.begin(), boundaries.end(),
              [](const RationalTime& left, const RationalTime& right) {
                  return left < right;
              });
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()),
                     boundaries.end());

    for (size_t index = 0; index + 1 < boundaries.size(); ++index) {
        const RationalTime start = boundaries[index];
        const RationalTime end = boundaries[index + 1];
        if (start >= end) continue;
        const DocumentClip* topClip = nullptr;
        int32_t topTrack = std::numeric_limits<int32_t>::min();
        for (const DocumentTrack& track : document.sequence.tracks) {
            if (track.kind != "video" || !track.visible) continue;
            for (const DocumentClip& clip : track.clips) {
                RationalTime clipEnd;
                if (!ClipEnd(clip, clipEnd, error)) return false;
                if (clip.timeline_in <= start && clipEnd > start &&
                    track.index > topTrack) {
                    topClip = &clip;
                    topTrack = track.index;
                }
            }
        }
        if (topClip == nullptr) continue;
        if (!output.empty() && output.back().clip->id == topClip->id &&
            output.back().end == start) {
            output.back().end = end;
        } else {
            output.push_back({topClip, topTrack, start, end});
        }
    }
    return true;
}

bool FrameDuration(const Document& document, RationalTime& output,
                   std::string& error) {
    if (document.sequence.frame_rate.num <= 0 ||
        document.sequence.frame_rate.den <= 0) {
        error = "sequence frame rate must be positive";
        return false;
    }
    output = {document.sequence.frame_rate.den,
              document.sequence.frame_rate.num};
    return true;
}

bool FrameFor(const VisibleSegment& segment, const RationalTime& timeline,
              const std::string& role, const RationalTime* cutPosition,
              TimelineSheetFrame& output, std::string& error) {
    try {
        output.clip_id = segment.clip->id;
        output.source_id = segment.clip->source_id;
        output.timeline_position = timeline;
        output.source_position = segment.clip->source_in.add(
            timeline.sub(segment.clip->timeline_in));
        output.role = role;
        if (cutPosition != nullptr) {
            output.cut_position = *cutPosition;
            output.has_cut_position = true;
        }
        return true;
    } catch (const std::exception&) {
        error = "clip '" + segment.clip->id + "' source position overflows";
        return false;
    }
}

std::vector<size_t> EvenSelection(size_t count, size_t maximum) {
    if (count <= maximum) {
        std::vector<size_t> all(count);
        for (size_t index = 0; index < count; ++index) all[index] = index;
        return all;
    }
    std::vector<size_t> selected;
    selected.reserve(maximum);
    if (maximum == 1) return {count / 2};
    for (size_t index = 0; index < maximum; ++index) {
        selected.push_back(index * (count - 1) / (maximum - 1));
    }
    return selected;
}

std::string ExactSecondsString(const RationalTime& time) {
    const bool negative = time.value < 0;
    const uint64_t magnitude =
        negative ? static_cast<uint64_t>(-(time.value + 1)) + 1
                 : static_cast<uint64_t>(time.value);
    const uint64_t rate = static_cast<uint64_t>(time.rate);
    const uint64_t whole = magnitude / rate;
    uint64_t remainder = magnitude % rate;
    std::ostringstream output;
    if (negative) output << '-';
    output << whole;
    if (remainder == 0) return output.str();
    output << '.';
    for (int digit = 0; digit < 12 && remainder != 0; ++digit) {
        remainder *= 10;
        output << static_cast<char>('0' + remainder / rate);
        remainder %= rate;
    }
    return output.str();
}

bool RunFfmpeg(const std::vector<std::string>& arguments, std::string& error) {
    std::vector<std::string> storage = arguments;
    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (std::string& value : storage) argv.push_back(value.data());
    argv.push_back(nullptr);
    pid_t process = 0;
    const int spawnResult =
        posix_spawnp(&process, storage.front().c_str(), nullptr, nullptr,
                     argv.data(), environ);
    if (spawnResult != 0) {
        error = "unable to start FFmpeg: " +
                std::string(std::strerror(spawnResult));
        return false;
    }
    int status = 0;
    while (waitpid(process, &status, 0) < 0 && errno == EINTR) {
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        error = "FFmpeg could not render the timeline sheet";
        return false;
    }
    return true;
}

const LibraryMedia* FindMedia(const Document& document, const Ulid& sourceId) {
    return document.FindLibraryMedia(sourceId);
}

std::filesystem::path ResolvePath(const std::filesystem::path& directory,
                                  const std::string& path) {
    std::filesystem::path resolved(path);
    if (resolved.is_relative()) resolved = directory / resolved;
    return resolved.lexically_normal();
}

std::string EscapeFilterPath(const std::filesystem::path& path) {
    std::string escaped;
    for (char character : path.string()) {
        if (character == '\\' || character == '\'' || character == ':')
            escaped.push_back('\\');
        escaped.push_back(character);
    }
    return escaped;
}

}  // namespace

bool BuildTimelineSheetPlan(const Document& document, TimelineSheetKind kind,
                            const TimelineSheetSettings& settings,
                            TimelineSheetPlan& output, std::string& error) {
    output = {};
    output.kind = kind;
    error.clear();
    if (settings.maximum_images <= 0) {
        error = "maximum_images must be positive";
        return false;
    }
    if (kind == TimelineSheetKind::Cuts && settings.maximum_images < 2) {
        error = "a cut sheet needs room for at least one image pair";
        return false;
    }
    std::vector<VisibleSegment> segments;
    if (!VisibleSegments(document, segments, error)) return false;
    RationalTime frameDuration;
    if (!FrameDuration(document, frameDuration, error)) return false;

    if (kind == TimelineSheetKind::Contact) {
        output.total_candidates = static_cast<int64_t>(segments.size());
        const std::vector<size_t> selected =
            EvenSelection(segments.size(), settings.maximum_images);
        for (size_t index : selected) {
            const VisibleSegment& segment = segments[index];
            int64_t frames = 0;
            try {
                frames = segment.end.sub(segment.start)
                             .to_frames(document.sequence.frame_rate.num,
                                        document.sequence.frame_rate.den);
            } catch (const std::exception&) {
                error = "visible segment for clip '" + segment.clip->id +
                        "' is not aligned to the sequence frame rate";
                return false;
            }
            if (frames <= 0) continue;
            const RationalTime middle = segment.start.add(
                {frames / 2 * document.sequence.frame_rate.den,
                 document.sequence.frame_rate.num});
            TimelineSheetFrame frame;
            if (!FrameFor(segment, middle, "middle", nullptr, frame, error))
                return false;
            output.frames.push_back(std::move(frame));
        }
        return true;
    }

    std::vector<size_t> cuts;
    for (size_t index = 1; index < segments.size(); ++index) {
        if (segments[index - 1].end == segments[index].start)
            cuts.push_back(index);
    }
    output.total_candidates = static_cast<int64_t>(cuts.size());
    const size_t maximumCuts =
        std::max<size_t>(1, static_cast<size_t>(settings.maximum_images / 2));
    for (size_t selectedIndex : EvenSelection(cuts.size(), maximumCuts)) {
        const size_t rightIndex = cuts[selectedIndex];
        const VisibleSegment& left = segments[rightIndex - 1];
        const VisibleSegment& right = segments[rightIndex];
        const RationalTime cut = right.start;
        const RationalTime before = cut.sub(frameDuration);
        if (before < left.start) {
            error = "visible segment before cut is shorter than one frame";
            return false;
        }
        TimelineSheetFrame leftFrame;
        TimelineSheetFrame rightFrame;
        if (!FrameFor(left, before, "before", &cut, leftFrame, error) ||
            !FrameFor(right, cut, "after", &cut, rightFrame, error))
            return false;
        output.frames.push_back(std::move(leftFrame));
        output.frames.push_back(std::move(rightFrame));
    }
    return true;
}

std::string SerializeTimelineSheetPlan(const TimelineSheetPlan& plan) {
    Value root = Value::MakeObject();
    root.Set("kind", Value::MakeString(plan.kind == TimelineSheetKind::Contact
                                           ? "contact_sheet"
                                           : "cut_sheet"));
    root.Set("total_candidates", Value::MakeInt(plan.total_candidates));
    root.Set("selected_images",
             Value::MakeInt(static_cast<int64_t>(plan.frames.size())));
    Value frames = Value::MakeArray();
    for (size_t index = 0; index < plan.frames.size(); ++index) {
        const TimelineSheetFrame& frame = plan.frames[index];
        Value value = Value::MakeObject();
        value.Set("cell", Value::MakeInt(static_cast<int64_t>(index)));
        value.Set("role", Value::MakeString(frame.role));
        value.Set("clip_id", Value::MakeString(frame.clip_id));
        value.Set("source_id", Value::MakeString(frame.source_id));
        value.Set("timeline_position", TimeValue(frame.timeline_position));
        value.Set("source_in", TimeValue(frame.source_position));
        if (frame.has_cut_position)
            value.Set("cut_position", TimeValue(frame.cut_position));
        frames.Push(std::move(value));
    }
    root.Set("frames", std::move(frames));
    return root.Dump();
}

bool RenderTimelineSheet(const Document& document,
                         const std::filesystem::path& projectDirectory,
                         const TimelineSheetPlan& plan,
                         const TimelineSheetSettings& settings,
                         std::string& jpegBytes, std::string& error) {
    jpegBytes.clear();
    error.clear();
    if (plan.frames.empty()) {
        error = plan.kind == TimelineSheetKind::Contact
                    ? "the timeline has no visible video shots"
                    : "the timeline has no visible cuts";
        return false;
    }
    if (settings.columns <= 0 || settings.cell_dimension < 16 ||
        settings.jpeg_quality < 2 || settings.jpeg_quality > 31) {
        error = "invalid timeline sheet render settings";
        return false;
    }

    const std::filesystem::path temporaryDirectory =
        std::filesystem::temp_directory_path();
    const std::string id = GenerateUlid();
    const std::filesystem::path imagePath =
        temporaryDirectory / ("cutmachine-sheet-" + id + ".jpg");
    const std::filesystem::path lutPath =
        temporaryDirectory / ("cutmachine-sheet-" + id + ".cube");
    const auto cleanup = [&]() {
        std::error_code ignored;
        std::filesystem::remove(imagePath, ignored);
        std::filesystem::remove(lutPath, ignored);
    };

    std::vector<std::string> arguments = {settings.ffmpeg_path, "-hide_banner",
                                          "-loglevel", "error", "-nostdin"};
    for (const TimelineSheetFrame& frame : plan.frames) {
        const LibraryMedia* media = FindMedia(document, frame.source_id);
        if (media == nullptr) {
            cleanup();
            error = "unknown media_id '" + frame.source_id + "'";
            return false;
        }
        if (media->metadata_complete && !media->has_video) {
            cleanup();
            error = "media_id '" + frame.source_id +
                    "' is audio-only; timeline sheets require video";
            return false;
        }
        arguments.insert(
            arguments.end(),
            {"-ss", ExactSecondsString(frame.source_position), "-i",
             ResolvePath(projectDirectory, media->path).string()});
    }

    const int32_t columns = std::min<int32_t>(
        settings.columns, static_cast<int32_t>(plan.frames.size()));
    std::ostringstream graph;
    for (size_t index = 0; index < plan.frames.size(); ++index) {
        graph << '[' << index << ":v:0]scale=w=" << settings.cell_dimension
              << ":h=" << settings.cell_dimension
              << ":force_original_aspect_ratio=decrease:flags=lanczos,pad="
              << settings.cell_dimension << ':' << settings.cell_dimension
              << ":(ow-iw)/2:(oh-ih)/2:black,setsar=1[cell" << index << "];";
    }
    if (plan.frames.size() == 1) {
        graph << "[cell0]null";
    } else {
        for (size_t index = 0; index < plan.frames.size(); ++index)
            graph << "[cell" << index << ']';
        graph << "xstack=inputs=" << plan.frames.size() << ":layout=";
        for (size_t index = 0; index < plan.frames.size(); ++index) {
            if (index) graph << '|';
            graph << (index % columns) * settings.cell_dimension << '_'
                  << (index / columns) * settings.cell_dimension;
        }
        graph << ":fill=black";
    }

    if (document.color_management.enabled) {
        const ColorManagementSettings preview =
            ColorManagementForSdrPreview(document.color_management);
        std::string cube;
        if (!BuildOpenColorIoCube(preview, cube, error)) {
            cleanup();
            error = "unable to build OpenColorIO sheet transform: " + error;
            return false;
        }
        std::ofstream lut(lutPath, std::ios::binary | std::ios::trunc);
        lut << cube;
        lut.close();
        if (!lut) {
            cleanup();
            error = "unable to create the timeline sheet colour transform";
            return false;
        }
        graph << ",scale=";
        bool hasInputOption = false;
        if (document.color_management.input_range != "auto") {
            graph << "in_range="
                  << (document.color_management.input_range == "full" ? "full"
                                                                      : "tv");
            hasInputOption = true;
        }
        if (document.color_management.input_ycbcr_matrix != "auto") {
            if (hasInputOption) graph << ':';
            graph << "in_color_matrix="
                  << (document.color_management.input_ycbcr_matrix ==
                              "bt2020_ncl"
                          ? "bt2020"
                          : "bt709");
            hasInputOption = true;
        }
        if (hasInputOption) graph << ':';
        graph << "out_range=full,format=gbrp16le,lut3d=file='"
              << EscapeFilterPath(lutPath)
              << "':interp=trilinear,scale=out_range=tv:out_color_matrix=bt709";
    }
    graph << ",format=yuvj420p[sheet]";

    arguments.insert(arguments.end(), {"-filter_complex", graph.str(), "-map",
                                       "[sheet]", "-frames:v", "1", "-q:v",
                                       std::to_string(settings.jpeg_quality),
                                       "-y", imagePath.string()});
    if (!RunFfmpeg(arguments, error)) {
        cleanup();
        return false;
    }
    std::ifstream input(imagePath, std::ios::binary);
    if (!input) {
        cleanup();
        error = "unable to read the rendered timeline sheet";
        return false;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    jpegBytes = buffer.str();
    cleanup();
    if (jpegBytes.empty()) {
        error = "the timeline sheet produced no image";
        return false;
    }
    return true;
}
