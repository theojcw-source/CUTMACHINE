#pragma once

// SEQ-2026-08 -- deriving a sequence format from the rushes it will hold.
//
// The gap this closes is not the mutation: `update_sequence` has always been
// able to set width, height and rate. The gap is that somebody had to *know*
// the numbers, and the only caller who could was a human reading a media
// panel. An agent asked to "set the right format" would have to invent
// 2160x3840, which is exactly what PHILOSOPHY.md principle 7 forbids: the
// caller names the intent ("match the rushes"), the engine resolves the exact
// values. Same division of labour as ResolveIntervalSplits and
// ResolveWordRemoval.
//
// Two things make this worth an engine function rather than a glance at the
// library:
//
//   - Rotation. A phone or mirrorless rush is stored landscape with a 90°
//     display matrix; its *displayed* frame is portrait. Choosing the stored
//     size gives a sideways sequence, and no amount of care at the call site
//     prevents that mistake being made once per project. Ingest.cc computes
//     displayed size in floating point because it only needs the word
//     "portrait"; a sequence format needs exact pixels, so the swap here is
//     integer arithmetic on a right angle and nothing else.
//   - Disagreement. Real projects mix rates and sizes. The rule is majority,
//     with every other format reported alongside, so a caller can see what it
//     is not choosing instead of being silently conformed.

#include "Document.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct SequenceFormatCandidate {
    int32_t width = 0;
    int32_t height = 0;
    MediaRate frame_rate{0, 1};
    // How many media in the library display in exactly this format.
    size_t media_count = 0;
};

struct SequenceFormatProposal {
    SequenceFormatCandidate chosen;
    // Every distinct displayed format found, most common first, `chosen`
    // included. A caller that reports only `chosen` hides a mixed-rate
    // shoot from the person who needs to know about it.
    std::vector<SequenceFormatCandidate> candidates;
    // Media that carry a usable displayed format.
    size_t media_considered = 0;
    // Media left out: audio-only, zero-sized, no frame rate, or rotated by
    // something that is not a right angle. Never silently folded into a
    // candidate, because none of them has a format to contribute.
    size_t media_ignored = 0;
    bool unanimous = false;
};

// Exact displayed dimensions, swapping width and height for a quarter turn.
// Returns false when `rotation_degrees` is not a multiple of 90, which no
// exact integer format can describe.
bool DisplayDimensions(const LibraryMedia& media, int32_t& width,
                       int32_t& height);

// Picks the format the most media already display in. Ties break on the
// larger frame, then the higher rate, then the wider frame -- deterministic
// down to the last field, so the same library always yields the same answer
// whatever order it is stored in. Fails only when no media has a usable
// format at all.
bool ResolveSequenceFormat(const std::vector<LibraryMedia>& library,
                           SequenceFormatProposal& proposal,
                           std::string& error);

// The proposal as the JSON both the CLI and the MCP tool report. Kept here so
// the two surfaces cannot drift into describing the same computation
// differently. `extraFields` is spliced in before the closing brace as
// already-serialized text (each starting with a comma), which is how the MCP
// tool adds what only it knows: whether it applied the format or found the
// sequence already conforming.
std::string SequenceFormatProposalJson(const SequenceFormatProposal& proposal,
                                       const std::string& extraFields = "");
