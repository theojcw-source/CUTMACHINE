#pragma once

#include "Document.h"
#include "Operations.h"
#include "RationalTime.h"

#include <cstdint>
#include <string>
#include <vector>

// QC-2026-09 A4 -- addresses the timeline by the material on it rather than
// by where that material happens to have landed.
//
// Every decision an editor makes about an interview is made in the rush:
// "cut after he says the name", "this take starts at 1412". Every operation
// in Operations.h except the word-level ones takes a *timeline* position.
// The conversion between the two is one subtraction and one addition, and
// doing it by hand is exactly the kind of arithmetic that is right nine
// times and wrong the tenth. Measured, on one session: every timeline
// position computed by hand produced at least one error, and the script that
// took source bounds and resolved the position itself produced none.
//
// So this is PHILOSOPHY.md principle 7 applied to position: the caller names
// the frame of the rush it means, and the engine works out which clip is
// playing it and where. Nothing here mutates a document; the resolvers below
// produce positions and deltas for the existing operations to consume.
//
// A source frame can legitimately be on the timeline more than once -- the
// picture and its detached sound, or one rush used in two places -- so this
// reports every match rather than picking one. That is not the ambiguity
// IdResolver refuses: two clips playing the same frame is a fact about the
// montage, and hiding it would be the error.

struct SourceFrameMatch {
    Ulid clip_id;
    Ulid track_id;
    std::string track_kind;
    int32_t track_index = 0;
    Ulid link_group_id;
    // Where that frame of the rush sits on the timeline. Exact: computed by
    // RationalTime::add/sub, which reconcile two timebases without rounding,
    // so a 23.976 rush in a 25 i/s sequence resolves as exactly as a matched
    // one.
    RationalTime timeline_position;
    // Distance from the clip's own head, which is what a trim would have to
    // move to reach this frame.
    RationalTime offset_in_clip;
    RationalTime clip_source_in;
    RationalTime clip_duration;
    RationalTime clip_timeline_in;
};

// Every clip on `document`'s timeline that plays frame `sourceFrame` of
// `mediaId`, in that media's own frame rate, ordered by track index then by
// timeline position. Fails when the media is not mounted as a source or the
// frame is negative; an empty `matches` is a success meaning the frame is
// not on this timeline, which is a fact a caller can act on.
bool ResolveSourceFrame(const Document& document, const Ulid& mediaId,
                        int64_t sourceFrame,
                        std::vector<SourceFrameMatch>& matches,
                        std::string& error);

// The timeline position at which `clipId` plays frame `sourceFrame` of its
// own source -- what a split needs. Refuses a frame the clip does not play,
// rather than producing a position outside it that ApplyOperation would then
// reject with a less useful message.
bool ResolveClipSourceFramePosition(const Document& document,
                                    const Ulid& clipId, int64_t sourceFrame,
                                    RationalTime& timelinePosition,
                                    std::string& error);

// The trim delta that puts one edge of `clipId` on `sourceFrame`.
//
// Head means "make the clip start on this frame". Tail means "make this the
// last frame the clip plays" -- inclusive, because that is what an editor
// means by a frame number, and because the alternative (an exclusive end) is
// an off-by-one waiting to happen at exactly the moment nobody is checking.
// The engine owns that +1, which is the whole point.
bool ResolveClipSourceFrameTrim(const Document& document, const Ulid& clipId,
                                int64_t sourceFrame, TrimEdge edge,
                                RationalTime& delta, std::string& error);

// The agent- and CLI-facing view of a resolution.
std::string DescribeSourceFrameMatches(
    const Ulid& mediaId, int64_t sourceFrame,
    const std::vector<SourceFrameMatch>& matches);
