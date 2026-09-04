#pragma once

// RESOLVE-2026-08 -- the return trip: a CUTMACHINE timeline described in the
// terms DaVinci Resolve's MediaPool.AppendToTimeline() consumes.
//
// This is deliberately not an EDL/FCPXML/OTIO writer. The media is already in
// Resolve's Media Pool -- the same files CUTMACHINE imported -- so the cut can
// be rebuilt by naming source frame ranges, with no interchange format to
// serialize, parse, or get subtly wrong at either end. What an interchange
// format would buy (other NLEs, archival) is a separate job; see ROADMAP.md.
//
// The exactness rule is the whole point of putting this in the engine rather
// than in the bridge script. Resolve addresses source frames as integers, and
// `--describe` publishes frames through RationalTime::to_frames, which
// *floors*. Flooring a 50 fps rush's position in a 25 fps sequence silently
// moves the cut. So this converts each boundary itself and refuses any clip
// whose position or duration does not land on a whole source frame, rather
// than shipping a montage that is a frame off somewhere nobody will look.
//
// Video tracks only: Resolve appends a clip's own audio with it, so sending
// CUTMACHINE's separate audio clips as well would double every sound. Which
// video clips may bring that audio is decided per clip -- see `with_audio`.
//
// Layering is carried explicitly. A montage that covers an interview with a
// cutaway needs the overlay to land *on top of* the clip it covers, and
// AppendToTimeline can only append end to end unless each clip names its
// track and its position. Flattening overlapping tracks into one running
// order, which is what the v1 schema could express, silently turned a 71 s
// layered cut into a 92 s linear one -- every cutaway playing after the
// sentence it was meant to cover.

#include "Document.h"

#include <cstdint>
#include <string>
#include <vector>

struct ResolveTimelineClip {
    // The absolute path is what identifies the media in Resolve's Media Pool;
    // the filename is carried for diagnostics and for the fallback match.
    std::string path;
    std::string filename;
    // Half-open frame range [start, end) in the *source's own* frame domain,
    // which is what AppendToTimeline's clipInfo expects -- not the sequence's.
    // Measured against a real round trip: Resolve builds end-start frames, so
    // an inclusive end silently drops the last frame of every single clip.
    int64_t start_frame = 0;
    int64_t end_frame = 0;
    // Video layer, 0 for the bottom track and counting up. The bridge maps it
    // to Resolve's 1-based trackIndex and creates the tracks a new timeline
    // does not start with; keeping it 0-based here matches DocumentTrack's
    // own ordering rather than Resolve's convention.
    int32_t video_layer = 0;
    // Position on the sequence timeline, in whole sequence frames counted
    // from its start. The bridge adds Resolve's own timeline start (90000 at
    // 25 fps for a 01:00:00:00 start), which is a Resolve fact and so is not
    // baked in here.
    int64_t record_frame = 0;
    // Whether Resolve should bring this clip's own audio along. True only for
    // a clip the montage also plays, unchanged, on an audio track. An overlay
    // that exists for its picture alone must not drop its room tone under the
    // interview it covers, and a base clip must not lose its sound.
    bool with_audio = false;
};

struct ResolveTimelineExport {
    std::string name;
    MediaRate frame_rate{25, 1};
    int32_t width = 0;
    int32_t height = 0;
    std::vector<ResolveTimelineClip> clips;
};

// Exact conversion of a time in its own rational domain to a whole frame
// count at `rate`. Returns false when the time falls between two frames.
bool ExactFrameCount(const RationalTime& time, const MediaRate& rate,
                     int64_t& frames);

// Fails when a clip references unknown media, when a source has no usable
// rate, or when any boundary is not a whole source frame. Never rounds.
bool BuildResolveTimelineExport(const Document& document,
                                ResolveTimelineExport& exported,
                                std::string& error);

std::string SerializeResolveTimelineExport(
    const ResolveTimelineExport& exported);

// Headless entry point: describes an explicit timeline, or the active one
// when timelineId is empty, for the bridge script. Read-only.
int ExportResolveTimelineCommand(const std::string& projectPath,
                                 std::string& output,
                                 const std::string& timelineId = {});
