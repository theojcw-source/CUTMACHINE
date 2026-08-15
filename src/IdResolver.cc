#include "IdResolver.h"

#include "Document.h"

#include <algorithm>

std::vector<Ulid> CollectDocumentIds(const Document& document) {
    std::vector<Ulid> ids;
    ids.push_back(document.sequence.id);
    for (const DocumentTrack& track : document.sequence.tracks) {
        ids.push_back(track.id);
        for (const DocumentClip& clip : track.clips) ids.push_back(clip.id);
    }
    for (const DocumentSource& source : document.sources)
        ids.push_back(source.id);
    for (const LibraryMedia& media : document.library) ids.push_back(media.id);
    for (const DocumentBin& bin : document.bins) ids.push_back(bin.id);
    for (const DocumentMarker& marker : document.sequence.markers)
        ids.push_back(marker.id);
    for (const DocumentTransition& transition : document.sequence.transitions)
        ids.push_back(transition.id);
    for (const CaptionStyle& style : document.sequence.caption_styles)
        ids.push_back(style.id);
    for (const MulticamGroup& group : document.sequence.multicam_groups) {
        ids.push_back(group.id);
        for (const MulticamAngle& angle : group.angles) ids.push_back(angle.id);
    }
    return ids;
}

IdResolver::IdResolver(const Document& document)
    : universe_(CollectDocumentIds(document)),
      sequence_id_(document.sequence.id) {}

bool IdResolver::Resolve(const std::string& fieldName, const std::string& input,
                         Ulid& output, std::string& error) const {
    if (input.empty()) {
        error = "'" + fieldName + "' must not be empty";
        return false;
    }
    // A full ID is checked first and, if present, wins outright: since every
    // ID in the universe has the same fixed length, a full match can never
    // also be a proper prefix of a different one, so this cannot mask an
    // ambiguity.
    if (std::find(universe_.begin(), universe_.end(), input) !=
        universe_.end()) {
        output = input;
        return true;
    }
    const Ulid* match = nullptr;
    size_t matchCount = 0;
    for (const Ulid& id : universe_) {
        if (id.size() >= input.size() &&
            id.compare(0, input.size(), input) == 0) {
            if (matchCount == 0) match = &id;
            ++matchCount;
            if (matchCount > 1) break;
        }
    }
    if (matchCount == 0) {
        error = "'" + fieldName + "': no object matches id or prefix '" +
                input + "'";
        return false;
    }
    if (matchCount > 1) {
        error = "'" + fieldName + "': id prefix '" + input +
                "' is ambiguous, matches more than one object";
        return false;
    }
    output = *match;
    return true;
}
