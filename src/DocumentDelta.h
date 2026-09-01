#pragma once

#include "Document.h"

#include <cstdint>
#include <string>
#include <vector>

// DELTA-2026-08 -- what one mutation changed, so a caller can patch its own
// model of the timeline instead of re-reading the whole thing.
//
// This exists because of a measured cost. Editing one interview through the
// MCP server meant calling `describe` after almost every mutation: a 250 KB
// snapshot re-read a dozen times to learn that four clips had moved by the
// same 32 frames. That is expensive, and worse, it is a second source of
// truth -- a caller that re-reads has no way to tell a change it caused from
// a change it did not.
//
// The delta is computed by diffing the document before and after, never by
// having each operation describe itself. There are forty operations; forty
// hand-written descriptions would drift from what the operations actually do,
// and the drift would be silent. One diff cannot disagree with the mutation
// it observed.
//
// Pure: no FFmpeg, no filesystem, no Document mutation. Sits below both the
// CLI and the MCP server so neither surface is privileged (PHILOSOPHY.md
// principle 3).

// Resulting state of a clip that was created or changed. Resulting state
// rather than a patch: a patch needs the reader to already agree about the
// previous value, which is the assumption that made re-reading necessary.
struct DeltaClip {
    Ulid clip_id;
    Ulid track_id;
    Ulid source_id;
    RationalTime source_in{0, 1};
    RationalTime duration{0, 1};
    RationalTime timeline_in{0, 1};
    // True when the clip did not exist before. A caller adds it rather than
    // patching, and knows not to look for a previous position.
    bool created = false;
};

// One ripple, stated as a rule rather than as a list. A trim that shifts
// forty clips is one line here and forty in `clips`; the compression is the
// point, and it is exact -- the rule is only emitted when every clip on the
// track at or after `from` moved by exactly `by` and changed nothing else.
struct DeltaShift {
    Ulid track_id;
    // Position, before the mutation, of the first clip the rule covers.
    RationalTime from{0, 1};
    // Signed. Negative when a ripple closed a gap.
    RationalTime by{0, 1};
    int32_t count = 0;
};

struct DeltaTrack {
    Ulid track_id;
    std::string kind;
    int32_t index = 0;
};

struct DocumentDelta {
    std::vector<DeltaClip> clips;
    std::vector<Ulid> removed_clip_ids;
    std::vector<DeltaShift> shifted;
    std::vector<DeltaTrack> created_tracks;
    std::vector<Ulid> removed_track_ids;
    // Timeline duration after the mutation. Always published, because it is
    // one line and it is what a caller checks first.
    RationalTime duration{0, 1};
    // Name, size or frame rate of the sequence changed. The caller re-reads
    // the sequence header; it is small, unlike the tracks.
    bool sequence_changed = false;
};

// True when `left` and `right` are the same clip in every field.
//
// Every field of DocumentClip must be compared here. A field left out makes
// a delta that silently under-reports, which is worse than no delta at all --
// a caller would trust a stale model. tests/document_delta_tests.cc mutates
// each field in turn and expects the clip to be reported; extend both when
// DocumentClip grows.
bool ClipsEqual(const DocumentClip& left, const DocumentClip& right);

// Diffs two states of the same timeline. Returns whether anything changed.
// Never fails: two documents always have a difference or they do not.
bool ComputeDocumentDelta(const Document& before, const Document& after,
                          DocumentDelta& delta);

// JSON object, ready to be embedded in a tool result. Omits empty sections
// so an edit that moved one clip does not pay for six empty arrays.
std::string SerializeDocumentDelta(const DocumentDelta& delta);
