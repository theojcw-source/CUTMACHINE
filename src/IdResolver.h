#pragma once

// Prefix-based short-ID resolution for the MCP surface (ROADMAP.md F1.2).
//
// Conceptually the same idea a comparable agent-native editor's MCP surface
// uses -- an agent sees and sends short unique prefixes of ULIDs instead of
// full 26-character IDs, and an ambiguous prefix is an explicit error
// rather than a silent guess -- but written from that description, not
// from any specific implementation's (GPLv3) source. See ROADMAP.md
// guard-rail 7.
//
// The resolver itself is deliberately kind-agnostic: it just answers
// "which full ID in this document does this string identify". A prefix
// that resolves to a real ID of the wrong kind (e.g. a track ID where a
// clip_id was expected) is still caught, exactly as an unresolved bad ID
// would be, by ApplyOperation's own Document::Find* lookups -- the
// resolver does not duplicate that validation.

#include "Ulid.h"

#include <string>
#include <vector>

class Document;

class IdResolver {
public:
    // Builds the resolver over every stable ID currently addressable in
    // `document`: sequence, tracks, clips, sources, library media, bins,
    // markers, transitions, caption styles, multicam groups and angles.
    explicit IdResolver(const Document& document);

    // `input` may be a full ID (checked first, as an exact match) or a
    // non-empty prefix of exactly one known ID. Returns false with a
    // human-readable `error` naming `fieldName` when `input` is empty,
    // matches nothing, or matches more than one ID -- never guesses.
    bool Resolve(const std::string& fieldName, const std::string& input,
                 Ulid& output, std::string& error) const;

    const std::vector<Ulid>& Universe() const { return universe_; }

    // The document's own sequence ID. Exposed so tools that operate on "the
    // sequence" (there is exactly one per document) do not need to ask the
    // caller to look it up and pass it back.
    const Ulid& SequenceId() const { return sequence_id_; }

private:
    std::vector<Ulid> universe_;
    Ulid sequence_id_;
};

// Exposed for testing and for callers that already hold a universe (e.g. one
// gathered from a freshly generated batch of IDs mid-operation).
std::vector<Ulid> CollectDocumentIds(const Document& document);
