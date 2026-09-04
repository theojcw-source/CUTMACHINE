#include "ResolveExport.h"

#include "Cli.h"
#include "Json.h"
#include "ProjectStorage.h"

#include <algorithm>
#include <filesystem>
#include <sstream>

bool ExactFrameCount(const RationalTime& time, const MediaRate& rate,
                     int64_t& frames) {
    if (time.rate <= 0 || rate.num <= 0 || rate.den <= 0) return false;
    const __int128 numerator = static_cast<__int128>(time.value) * rate.num;
    const __int128 denominator = static_cast<__int128>(time.rate) * rate.den;
    if (denominator == 0 || numerator % denominator != 0) return false;
    const __int128 quotient = numerator / denominator;
    if (quotient > INT64_MAX || quotient < INT64_MIN) return false;
    frames = static_cast<int64_t>(quotient);
    return true;
}

bool BuildResolveTimelineExport(const Document& document,
                                ResolveTimelineExport& exported,
                                std::string& error) {
    ResolveTimelineExport built;
    built.name = document.sequence.name;
    built.frame_rate = document.sequence.frame_rate;
    built.width = document.sequence.width;
    built.height = document.sequence.height;

    // Video layers are numbered from the bottom track up, by DocumentTrack
    // index, so layer 0 is the base cut and every higher layer covers it.
    std::vector<const DocumentTrack*> videoTracks;
    for (const DocumentTrack& track : document.sequence.tracks)
        if (track.kind == "video") videoTracks.push_back(&track);
    std::stable_sort(videoTracks.begin(), videoTracks.end(),
                     [](const DocumentTrack* left, const DocumentTrack* right) {
                         return left->index < right->index;
                     });

    // Base layer first, then each overlay in turn, and within a layer in
    // timeline order. Append order no longer decides where a clip lands --
    // record_frame does -- but sending a track at a time keeps the report
    // readable and matches the order a viewer builds the picture in.
    struct Ordered {
        int32_t layer;
        RationalTime timeline_in;
        const DocumentClip* clip;
    };
    std::vector<Ordered> ordered;
    for (size_t layer = 0; layer < videoTracks.size(); ++layer)
        for (const DocumentClip& clip : videoTracks[layer]->clips)
            ordered.push_back(
                {static_cast<int32_t>(layer), clip.timeline_in, &clip});
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const Ordered& left, const Ordered& right) {
                         if (left.layer != right.layer)
                             return left.layer < right.layer;
                         return left.timeline_in < right.timeline_in;
                     });
    if (ordered.empty()) {
        error = "the active timeline has no video clip to send";
        return false;
    }

    for (const Ordered& item : ordered) {
        const DocumentClip& clip = *item.clip;
        const auto media =
            std::find_if(document.library.begin(), document.library.end(),
                         [&](const LibraryMedia& entry) {
                             return entry.id == clip.source_id;
                         });
        if (media == document.library.end()) {
            error = "clip '" + clip.id + "' references media '" +
                    clip.source_id + "' that is not in the library";
            return false;
        }
        if (media->rate.num <= 0 || media->rate.den <= 0) {
            error = "media '" + media->filename + "' has no usable frame rate";
            return false;
        }
        int64_t startFrame = 0;
        int64_t durationFrames = 0;
        if (!ExactFrameCount(clip.source_in, media->rate, startFrame) ||
            !ExactFrameCount(clip.duration, media->rate, durationFrames)) {
            error = "clip '" + clip.id + "' on '" + media->filename +
                    "' does not start and end on whole source frames at " +
                    std::to_string(media->rate.num) + "/" +
                    std::to_string(media->rate.den) +
                    "; sending it would move the cut";
            return false;
        }
        if (durationFrames <= 0) {
            error = "clip '" + clip.id + "' is shorter than one source frame";
            return false;
        }
        int64_t recordFrame = 0;
        if (!ExactFrameCount(clip.timeline_in, document.sequence.frame_rate,
                             recordFrame)) {
            error = "clip '" + clip.id +
                    "' does not sit on a whole sequence "
                    "frame; sending it would move the cut";
            return false;
        }
        // A clip keeps its sound only when the montage plays exactly that
        // range on an audio track. Anything else -- an overlay laid over
        // someone else's words, or a picture the editor deliberately
        // silenced -- goes over picture-only, because Resolve would otherwise
        // bring its room tone along and lay it under the interview.
        bool withAudio = false;
        for (const DocumentTrack& track : document.sequence.tracks) {
            if (track.kind != "audio") continue;
            for (const DocumentClip& sound : track.clips) {
                if (sound.source_id == clip.source_id &&
                    sound.timeline_in == clip.timeline_in &&
                    sound.source_in == clip.source_in &&
                    sound.duration == clip.duration) {
                    withAudio = true;
                    break;
                }
            }
            if (withAudio) break;
        }

        ResolveTimelineClip sent;
        sent.filename = media->filename;
        sent.path = media->path;
        sent.video_layer = item.layer;
        sent.record_frame = recordFrame;
        sent.with_audio = withAudio;
        sent.start_frame = startFrame;
        // Resolve's clipInfo endFrame is exclusive: AppendToTimeline builds
        // end-start frames. Verified by sending a 9-clip cut and counting what
        // came back -- an inclusive end lost exactly one frame per clip.
        sent.end_frame = startFrame + durationFrames;
        built.clips.push_back(std::move(sent));
    }
    exported = std::move(built);
    error.clear();
    return true;
}

std::string SerializeResolveTimelineExport(
    const ResolveTimelineExport& exported) {
    std::ostringstream output;
    output << "{\"ok\":true,\"schema\":\"cutmachine.resolve-timeline.v2\""
           << ",\"name\":\"" << mcp_json::EscapeJsonString(exported.name)
           << "\",\"frame_rate\":{\"num\":" << exported.frame_rate.num
           << ",\"den\":" << exported.frame_rate.den
           << "},\"width\":" << exported.width
           << ",\"height\":" << exported.height << ",\"clips\":[";
    for (size_t index = 0; index < exported.clips.size(); ++index) {
        const ResolveTimelineClip& clip = exported.clips[index];
        if (index) output << ',';
        output << "{\"path\":\"" << mcp_json::EscapeJsonString(clip.path)
               << "\",\"filename\":\""
               << mcp_json::EscapeJsonString(clip.filename)
               << "\",\"start_frame\":" << clip.start_frame
               << ",\"end_frame\":" << clip.end_frame
               << ",\"video_layer\":" << clip.video_layer
               << ",\"record_frame\":" << clip.record_frame
               << ",\"with_audio\":" << (clip.with_audio ? "true" : "false")
               << "}";
    }
    output << "]}\n";
    return output.str();
}

int ExportResolveTimelineCommand(const std::string& projectPath,
                                 std::string& output,
                                 const std::string& timelineId) {
    Project project;
    std::string error;
    if (!LoadStoredProject(projectPath, project, error)) {
        return FailCliCommand("InvalidDocument", error, output);
    }
    const Ulid selectedTimeline =
        timelineId.empty() ? project.active_timeline_id : timelineId;
    if (!project.FindTimeline(selectedTimeline)) {
        return FailCliCommand("UnknownSequence",
                              "unknown timeline_id '" + selectedTimeline + "'",
                              output);
    }
    Document document = project.MakeDocument(selectedTimeline);
    // Media paths are stored relative to the package; the bridge matches them
    // against Resolve's Media Pool, which knows only absolute paths.
    const std::filesystem::path base =
        std::filesystem::absolute(projectPath).parent_path();
    for (LibraryMedia& media : document.library) {
        std::filesystem::path stored(media.path);
        if (stored.is_relative()) stored = base / stored;
        std::error_code pathError;
        const std::filesystem::path resolved =
            std::filesystem::weakly_canonical(stored, pathError);
        if (!pathError) media.path = resolved.string();
    }
    ResolveTimelineExport exported;
    if (!BuildResolveTimelineExport(document, exported, error)) {
        return FailCliCommand("InvalidExport", error, output);
    }
    output = SerializeResolveTimelineExport(exported);
    return 0;
}
