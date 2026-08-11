#include "Operations.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace {

void Fail(EditError code, const std::string& text, EditError& error,
          std::string& message) {
    error = code;
    message = text;
}

ExactClipTimes TimesOf(const DocumentClip& clip) {
    return {clip.source_in, clip.duration, clip.timeline_in};
}

RationalTime PhaseOf(const DocumentClip& clip) {
    return clip.timeline_in.sub(clip.source_in);
}

std::vector<ExactTimelinePosition> PositionsAfter(const DocumentTrack& track,
                                                  size_t first) {
    std::vector<ExactTimelinePosition> positions;
    for (size_t index = first; index < track.clips.size(); ++index) {
        positions.push_back(
            {track.clips[index].id, track.clips[index].timeline_in});
    }
    return positions;
}

bool ApplyExactPositions(Document& document,
                         const std::vector<ExactTimelinePosition>& positions,
                         EditError& error, std::string& message) {
    for (const ExactTimelinePosition& position : positions) {
        DocumentClip* clip = document.FindClip(position.clip_id);
        if (!clip) {
            Fail(EditError::UnknownClip,
                 "exact ripple state references unknown clip_id '" +
                     position.clip_id + "'",
                 error, message);
            return false;
        }
        if (position.timeline_in.rate <= 0 || position.timeline_in.value < 0) {
            Fail(EditError::InvalidTimelineIn,
                 "exact ripple state has invalid timeline_in", error, message);
            return false;
        }
        clip->timeline_in = position.timeline_in;
    }
    return true;
}

bool ValidateResult(const Document& candidate, EditError& error,
                    std::string& message) {
    std::string validation;
    if (candidate.Validate(validation)) return true;
    const EditError code = validation.find("overlap") != std::string::npos
                               ? EditError::Overlap
                               : EditError::ValidationFailed;
    Fail(code, validation, error, message);
    return false;
}

bool ValidateSourceRange(const DocumentSource& source,
                         const RationalTime& sourceIn,
                         const RationalTime& duration, EditError& error,
                         std::string& message) {
    if (sourceIn.rate <= 0 || duration.rate <= 0) {
        Fail(EditError::ArithmeticError, "time rate must be positive", error,
             message);
        return false;
    }
    if (duration.value <= 0) {
        Fail(EditError::InvalidDuration, "duration must be positive", error,
             message);
        return false;
    }
    if (sourceIn.value < 0 || sourceIn.add(duration) > source.duration) {
        Fail(EditError::SourceOutOfBounds,
             "source range is outside source_id '" + source.id + "'", error,
             message);
        return false;
    }
    return true;
}

bool ApplyInsert(Document& candidate, InsertClipOperation& operation,
                 Operation& inverse, EditError& error, std::string& message) {
    DocumentTrack* track = candidate.FindTrack(operation.track_id);
    if (!track) {
        Fail(EditError::UnknownTrack,
             "unknown track_id '" + operation.track_id + "'", error, message);
        return false;
    }
    const DocumentSource* source = candidate.FindSource(operation.source_id);
    if (!source) {
        Fail(EditError::UnknownSource,
             "unknown source_id '" + operation.source_id + "'", error, message);
        return false;
    }
    if (operation.timeline_in.rate <= 0 || operation.timeline_in.value < 0) {
        Fail(EditError::InvalidTimelineIn,
             "timeline_in must be non-negative with a positive rate", error,
             message);
        return false;
    }
    if (!ValidateSourceRange(*source, operation.source_in, operation.duration,
                             error, message)) {
        return false;
    }

    if (operation.clip_id.empty()) operation.clip_id = GenerateUlid();
    if (!IsValidUlid(operation.clip_id) ||
        candidate.FindClip(operation.clip_id) ||
        candidate.FindSource(operation.clip_id) ||
        candidate.FindTrack(operation.clip_id)) {
        Fail(EditError::DuplicateId,
             "insert clip_id is invalid or already exists: '" +
                 operation.clip_id + "'",
             error, message);
        return false;
    }

    auto insertion = std::lower_bound(
        track->clips.begin(), track->clips.end(), operation.timeline_in,
        [](const DocumentClip& clip, const RationalTime& position) {
            return clip.timeline_in < position;
        });
    const size_t insertionIndex =
        static_cast<size_t>(std::distance(track->clips.begin(), insertion));
    if (insertion != track->clips.begin()) {
        const DocumentClip& previous = *std::prev(insertion);
        if (operation.timeline_in <
            previous.timeline_in.add(previous.duration)) {
            Fail(EditError::Overlap,
                 "insertion timeline_in overlaps clip_id '" + previous.id + "'",
                 error, message);
            return false;
        }
    }

    const std::vector<ExactTimelinePosition> before =
        PositionsAfter(*track, insertionIndex);
    for (size_t index = insertionIndex; index < track->clips.size(); ++index) {
        track->clips[index].timeline_in =
            track->clips[index].timeline_in.add(operation.duration);
    }
    track->clips.insert(
        track->clips.begin() + static_cast<std::ptrdiff_t>(insertionIndex),
        DocumentClip{operation.clip_id, operation.source_id,
                     operation.source_in, operation.duration,
                     operation.timeline_in});
    if (!operation.exact_timeline_result.empty() &&
        !ApplyExactPositions(candidate, operation.exact_timeline_result, error,
                             message)) {
        return false;
    }
    if (!ValidateResult(candidate, error, message)) return false;

    if (operation.exact_timeline_result.empty()) {
        DocumentTrack* updated = candidate.FindTrack(operation.track_id);
        operation.exact_timeline_result =
            PositionsAfter(*updated, insertionIndex + 1);
    }
    inverse = RemoveClipOperation{operation.clip_id, before};
    return true;
}

bool ApplyRemove(Document& candidate, RemoveClipOperation& operation,
                 Operation& inverse, EditError& error, std::string& message) {
    DocumentTrack* track = candidate.FindTrackForClip(operation.clip_id);
    if (!track) {
        Fail(EditError::UnknownClip,
             "unknown clip_id '" + operation.clip_id + "'", error, message);
        return false;
    }
    const auto found = std::find_if(
        track->clips.begin(), track->clips.end(),
        [&](const DocumentClip& clip) { return clip.id == operation.clip_id; });
    const size_t index =
        static_cast<size_t>(std::distance(track->clips.begin(), found));
    const DocumentClip removed = *found;
    const Ulid trackId = track->id;
    const std::vector<ExactTimelinePosition> before =
        PositionsAfter(*track, index + 1);
    for (size_t next = index + 1; next < track->clips.size(); ++next) {
        track->clips[next].timeline_in =
            track->clips[next].timeline_in.sub(removed.duration);
    }
    track->clips.erase(track->clips.begin() +
                       static_cast<std::ptrdiff_t>(index));
    if (!operation.exact_timeline_result.empty() &&
        !ApplyExactPositions(candidate, operation.exact_timeline_result, error,
                             message)) {
        return false;
    }
    if (!ValidateResult(candidate, error, message)) return false;
    if (operation.exact_timeline_result.empty()) {
        DocumentTrack* updated = candidate.FindTrack(trackId);
        operation.exact_timeline_result = PositionsAfter(*updated, index);
    }
    inverse = InsertClipOperation{trackId,
                                  removed.source_id,
                                  removed.source_in,
                                  removed.duration,
                                  removed.timeline_in,
                                  removed.id,
                                  before};
    return true;
}

bool ApplyClear(Document& candidate, ClearClipOperation& operation,
                Operation& inverse, EditError& error, std::string& message) {
    if (!operation.exact_track_result.empty()) {
        if (operation.exact_track_result.size() != 1) {
            Fail(EditError::InvalidOperation,
                 "exact clear state must contain the clip track", error,
                 message);
            return false;
        }
        DocumentTrack* track =
            candidate.FindTrack(operation.exact_track_result.front().track_id);
        if (!track) {
            Fail(EditError::UnknownTrack,
                 "exact clear references an unknown track", error, message);
            return false;
        }
        const ExactTrackState before{track->id, track->clips};
        track->clips = operation.exact_track_result.front().clips;
        if (!ValidateResult(candidate, error, message)) return false;
        inverse = ClearClipOperation{operation.clip_id, {before}};
        return true;
    }
    DocumentTrack* track = candidate.FindTrackForClip(operation.clip_id);
    if (!track) {
        Fail(EditError::UnknownClip,
             "unknown clip_id '" + operation.clip_id + "'", error, message);
        return false;
    }
    const ExactTrackState before{track->id, track->clips};
    track->clips.erase(
        std::remove_if(track->clips.begin(), track->clips.end(),
                       [&](const DocumentClip& clip) {
                           return clip.id == operation.clip_id;
                       }),
        track->clips.end());
    if (!ValidateResult(candidate, error, message)) return false;
    operation.exact_track_result = {{track->id, track->clips}};
    inverse = ClearClipOperation{operation.clip_id, {before}};
    return true;
}

bool Negate(const RationalTime& value, RationalTime& output) {
    if (value.value == std::numeric_limits<int64_t>::min()) return false;
    output = {-value.value, value.rate};
    return true;
}

bool ApplyTrim(Document& candidate, TrimClipOperation& operation,
               Operation& inverse, EditError& error, std::string& message) {
    DocumentClip* clip = candidate.FindClip(operation.clip_id);
    if (!clip) {
        Fail(EditError::UnknownClip,
             "unknown clip_id '" + operation.clip_id + "'", error, message);
        return false;
    }
    if (operation.delta.rate <= 0) {
        Fail(EditError::ArithmeticError, "trim delta rate must be positive",
             error, message);
        return false;
    }
    const DocumentSource* source = candidate.FindSource(clip->source_id);
    if (!source) {
        Fail(EditError::UnknownSource,
             "clip references unknown source_id '" + clip->source_id + "'",
             error, message);
        return false;
    }
    const ExactClipTimes before = TimesOf(*clip);
    if (operation.edge == TrimEdge::Head) {
        clip->source_in = clip->source_in.add(operation.delta);
        clip->duration = clip->duration.sub(operation.delta);
        clip->timeline_in = clip->timeline_in.add(operation.delta);
    } else {
        clip->duration = clip->duration.add(operation.delta);
    }
    if (clip->duration.value <= 0) {
        Fail(EditError::InvalidDuration,
             "trim would make duration zero or negative", error, message);
        return false;
    }
    if (clip->timeline_in.value < 0) {
        Fail(EditError::InvalidTimelineIn,
             "trim would make timeline_in negative", error, message);
        return false;
    }
    if (!ValidateSourceRange(*source, clip->source_in, clip->duration, error,
                             message)) {
        return false;
    }
    if (operation.exact_clip_result) {
        clip->source_in = operation.exact_clip_result->source_in;
        clip->duration = operation.exact_clip_result->duration;
        clip->timeline_in = operation.exact_clip_result->timeline_in;
    }
    if (!ValidateResult(candidate, error, message)) return false;
    if (!operation.exact_clip_result)
        operation.exact_clip_result = TimesOf(*clip);

    RationalTime inverseDelta;
    if (!Negate(operation.delta, inverseDelta)) {
        Fail(EditError::ArithmeticError, "trim delta cannot be negated", error,
             message);
        return false;
    }
    inverse = TrimClipOperation{operation.clip_id, operation.edge, inverseDelta,
                                before};
    return true;
}

bool ApplyMove(Document& candidate, MoveClipOperation& operation,
               Operation& inverse, EditError& error, std::string& message) {
    DocumentTrack* sourceTrack = candidate.FindTrackForClip(operation.clip_id);
    if (!sourceTrack) {
        Fail(EditError::UnknownClip,
             "unknown clip_id '" + operation.clip_id + "'", error, message);
        return false;
    }
    DocumentTrack* targetTrack = candidate.FindTrack(operation.track_id);
    if (!targetTrack) {
        Fail(EditError::UnknownTrack,
             "unknown track_id '" + operation.track_id + "'", error, message);
        return false;
    }
    if (sourceTrack->kind != targetTrack->kind) {
        Fail(EditError::InvalidOperation,
             "cannot move a clip between tracks of different kinds", error,
             message);
        return false;
    }
    if (operation.timeline_in.rate <= 0 || operation.timeline_in.value < 0) {
        Fail(EditError::InvalidTimelineIn,
             "move timeline_in must be non-negative with a positive rate",
             error, message);
        return false;
    }

    const auto snapshotTracks = [&](const std::vector<Ulid>& trackIds) {
        std::vector<ExactTrackState> snapshots;
        for (const Ulid& trackId : trackIds) {
            if (std::any_of(snapshots.begin(), snapshots.end(),
                            [&](const ExactTrackState& state) {
                                return state.track_id == trackId;
                            }))
                continue;
            const DocumentTrack* track = candidate.FindTrack(trackId);
            if (track) snapshots.push_back({track->id, track->clips});
        }
        return snapshots;
    };

    if (!operation.exact_track_result.empty()) {
        const Ulid currentTrackId = sourceTrack->id;
        const RationalTime currentTimelineIn =
            candidate.FindClip(operation.clip_id)->timeline_in;
        std::vector<Ulid> affected;
        for (const ExactTrackState& state : operation.exact_track_result)
            affected.push_back(state.track_id);
        const std::vector<ExactTrackState> before = snapshotTracks(affected);
        if (before.size() != operation.exact_track_result.size()) {
            Fail(EditError::UnknownTrack,
                 "exact move state references an unknown track", error,
                 message);
            return false;
        }
        for (const ExactTrackState& state : operation.exact_track_result) {
            candidate.FindTrack(state.track_id)->clips = state.clips;
        }
        if (!ValidateResult(candidate, error, message)) return false;
        inverse = MoveClipOperation{operation.clip_id, currentTrackId,
                                    currentTimelineIn, before};
        return true;
    }

    const Ulid sourceTrackId = sourceTrack->id;
    const std::vector<ExactTrackState> before =
        snapshotTracks({sourceTrackId, operation.track_id});
    const auto found = std::find_if(
        sourceTrack->clips.begin(), sourceTrack->clips.end(),
        [&](const DocumentClip& clip) { return clip.id == operation.clip_id; });
    const DocumentClip original = *found;
    sourceTrack->clips.erase(found);

    // Removing a clip cannot invalidate the tracks vector, but reacquiring the
    // destination avoids retaining a clip-vector-dependent pointer across the
    // erase and keeps same-track moves straightforward.
    targetTrack = candidate.FindTrack(operation.track_id);
    DocumentClip moved = original;
    moved.timeline_in = operation.timeline_in;
    const RationalTime movedEnd = moved.timeline_in.add(moved.duration);
    std::vector<DocumentClip> overwritten;
    overwritten.reserve(targetTrack->clips.size() + 2);
    for (const DocumentClip& existing : targetTrack->clips) {
        const RationalTime existingEnd =
            existing.timeline_in.add(existing.duration);
        if (existingEnd <= moved.timeline_in ||
            existing.timeline_in >= movedEnd) {
            overwritten.push_back(existing);
            continue;
        }

        const bool keepLeft = existing.timeline_in < moved.timeline_in;
        const bool keepRight = existingEnd > movedEnd;
        if (keepLeft) {
            DocumentClip left = existing;
            left.duration = moved.timeline_in.sub(existing.timeline_in);
            overwritten.push_back(std::move(left));
        }
        if (keepRight) {
            DocumentClip right = existing;
            if (keepLeft) {
                do {
                    right.id = GenerateUlid();
                } while (candidate.FindClip(right.id));
            }
            const RationalTime sourceOffset =
                movedEnd.sub(existing.timeline_in);
            right.source_in = existing.source_in.add(sourceOffset);
            right.duration = existingEnd.sub(movedEnd);
            right.timeline_in = movedEnd;
            overwritten.push_back(std::move(right));
        }
    }
    overwritten.push_back(std::move(moved));
    std::stable_sort(overwritten.begin(), overwritten.end(),
                     [](const DocumentClip& left, const DocumentClip& right) {
                         return left.timeline_in < right.timeline_in;
                     });
    targetTrack->clips = std::move(overwritten);
    if (!ValidateResult(candidate, error, message)) return false;

    operation.exact_track_result =
        snapshotTracks({sourceTrackId, operation.track_id});
    inverse = MoveClipOperation{operation.clip_id, sourceTrackId,
                                original.timeline_in, before};
    return true;
}

bool ApplyMoveLinked(Document& candidate, MoveLinkedClipsOperation& operation,
                     Operation& inverse, EditError& error,
                     std::string& message) {
    const auto snapshots = [&](const std::vector<Ulid>& ids) {
        std::vector<ExactTrackState> result;
        for (const Ulid& id : ids) {
            if (std::any_of(result.begin(), result.end(),
                            [&](const ExactTrackState& state) {
                                return state.track_id == id;
                            }))
                continue;
            const DocumentTrack* track = candidate.FindTrack(id);
            if (track) result.push_back({id, track->clips});
        }
        return result;
    };
    if (!operation.exact_track_result.empty()) {
        std::vector<Ulid> ids;
        for (const ExactTrackState& state : operation.exact_track_result)
            ids.push_back(state.track_id);
        const std::vector<ExactTrackState> before = snapshots(ids);
        if (before.size() != operation.exact_track_result.size()) {
            Fail(EditError::UnknownTrack,
                 "exact linked move references an unknown track", error,
                 message);
            return false;
        }
        for (const ExactTrackState& state : operation.exact_track_result)
            candidate.FindTrack(state.track_id)->clips = state.clips;
        if (!ValidateResult(candidate, error, message)) return false;
        inverse = MoveLinkedClipsOperation{operation.link_group_id,
                                           operation.moves, before};
        return true;
    }
    if (operation.moves.size() < 2 || operation.link_group_id.empty()) {
        Fail(EditError::InvalidOperation,
             "MoveLinkedClips requires at least two linked members", error,
             message);
        return false;
    }
    std::vector<Ulid> affectedTracks;
    std::vector<LinkedClipMove> inverseMoves;
    std::vector<Ulid> seenClips;
    for (const LinkedClipMove& move : operation.moves) {
        const DocumentClip* clip = candidate.FindClip(move.clip_id);
        const DocumentTrack* source = candidate.FindTrackForClip(move.clip_id);
        const DocumentTrack* target = candidate.FindTrack(move.track_id);
        if (!clip || !source || !target) {
            Fail(!target ? EditError::UnknownTrack : EditError::UnknownClip,
                 "linked move references an unknown clip or track", error,
                 message);
            return false;
        }
        if (clip->link_group_id != operation.link_group_id ||
            std::find(seenClips.begin(), seenClips.end(), clip->id) !=
                seenClips.end()) {
            Fail(EditError::InvalidOperation,
                 "linked move members must be unique and share link_group_id",
                 error, message);
            return false;
        }
        seenClips.push_back(clip->id);
        affectedTracks.push_back(source->id);
        affectedTracks.push_back(target->id);
        inverseMoves.push_back({clip->id, source->id, clip->timeline_in});
    }
    const std::vector<ExactTrackState> before = snapshots(affectedTracks);
    for (const LinkedClipMove& move : operation.moves) {
        MoveClipOperation single{
            move.clip_id, move.track_id, move.timeline_in, {}};
        Operation ignored = RemoveClipOperation{};
        if (!ApplyMove(candidate, single, ignored, error, message))
            return false;
    }
    operation.exact_track_result = snapshots(affectedTracks);
    inverse = MoveLinkedClipsOperation{operation.link_group_id,
                                       std::move(inverseMoves), before};
    return true;
}

bool ApplyTrimLinked(Document& candidate, TrimLinkedClipsOperation& operation,
                     Operation& inverse, EditError& error,
                     std::string& message) {
    const auto snapshots = [&](const std::vector<Ulid>& ids) {
        std::vector<ExactTrackState> result;
        for (const Ulid& id : ids) {
            if (std::any_of(result.begin(), result.end(),
                            [&](const ExactTrackState& state) {
                                return state.track_id == id;
                            }))
                continue;
            const DocumentTrack* track = candidate.FindTrack(id);
            if (track) result.push_back({id, track->clips});
        }
        return result;
    };
    if (!operation.exact_track_result.empty()) {
        std::vector<Ulid> ids;
        for (const ExactTrackState& state : operation.exact_track_result)
            ids.push_back(state.track_id);
        const std::vector<ExactTrackState> before = snapshots(ids);
        if (before.size() != operation.exact_track_result.size()) {
            Fail(EditError::UnknownTrack,
                 "exact linked trim references an unknown track", error,
                 message);
            return false;
        }
        for (const ExactTrackState& state : operation.exact_track_result)
            candidate.FindTrack(state.track_id)->clips = state.clips;
        if (!ValidateResult(candidate, error, message)) return false;
        inverse = TrimLinkedClipsOperation{operation.link_group_id,
                                           operation.trims, before};
        return true;
    }
    if (operation.trims.size() < 2 || operation.link_group_id.empty()) {
        Fail(EditError::InvalidOperation,
             "TrimLinkedClips requires at least two linked members", error,
             message);
        return false;
    }
    std::vector<Ulid> trackIds;
    std::vector<Ulid> seen;
    for (const LinkedClipTrim& trim : operation.trims) {
        const DocumentClip* clip = candidate.FindClip(trim.clip_id);
        const DocumentTrack* track = candidate.FindTrackForClip(trim.clip_id);
        if (!clip || !track || clip->link_group_id != operation.link_group_id ||
            std::find(seen.begin(), seen.end(), trim.clip_id) != seen.end()) {
            Fail(EditError::InvalidOperation,
                 "linked trim members must be unique and share link_group_id",
                 error, message);
            return false;
        }
        seen.push_back(trim.clip_id);
        trackIds.push_back(track->id);
    }
    const std::vector<ExactTrackState> before = snapshots(trackIds);
    for (const LinkedClipTrim& trim : operation.trims) {
        TrimClipOperation single{trim.clip_id, trim.edge, trim.delta,
                                 std::nullopt};
        Operation ignored = RemoveClipOperation{};
        if (!ApplyTrim(candidate, single, ignored, error, message))
            return false;
    }
    operation.exact_track_result = snapshots(trackIds);
    inverse = TrimLinkedClipsOperation{operation.link_group_id, operation.trims,
                                       before};
    return true;
}

bool ApplyRemoveLinked(Document& candidate,
                       RemoveLinkedClipsOperation& operation,
                       Operation& inverse, EditError& error,
                       std::string& message) {
    const auto snapshots = [&](const std::vector<Ulid>& ids) {
        std::vector<ExactTrackState> result;
        for (const Ulid& id : ids) {
            if (std::any_of(result.begin(), result.end(),
                            [&](const ExactTrackState& state) {
                                return state.track_id == id;
                            }))
                continue;
            const DocumentTrack* track = candidate.FindTrack(id);
            if (track) result.push_back({id, track->clips});
        }
        return result;
    };
    if (!operation.exact_track_result.empty()) {
        std::vector<Ulid> ids;
        for (const ExactTrackState& state : operation.exact_track_result)
            ids.push_back(state.track_id);
        const std::vector<ExactTrackState> before = snapshots(ids);
        if (before.size() != operation.exact_track_result.size()) {
            Fail(EditError::UnknownTrack,
                 "exact linked remove references an unknown track", error,
                 message);
            return false;
        }
        for (const ExactTrackState& state : operation.exact_track_result)
            candidate.FindTrack(state.track_id)->clips = state.clips;
        if (!ValidateResult(candidate, error, message)) return false;
        inverse = RemoveLinkedClipsOperation{operation.link_group_id,
                                             operation.clip_ids, before};
        return true;
    }
    if (operation.clip_ids.size() < 2 || operation.link_group_id.empty()) {
        Fail(EditError::InvalidOperation,
             "RemoveLinkedClips requires at least two linked members", error,
             message);
        return false;
    }
    std::vector<Ulid> trackIds;
    std::vector<Ulid> seen;
    for (const Ulid& id : operation.clip_ids) {
        const DocumentClip* clip = candidate.FindClip(id);
        const DocumentTrack* track = candidate.FindTrackForClip(id);
        if (!clip || !track || clip->link_group_id != operation.link_group_id ||
            std::find(seen.begin(), seen.end(), id) != seen.end()) {
            Fail(EditError::InvalidOperation,
                 "linked remove members must be unique and share link_group_id",
                 error, message);
            return false;
        }
        seen.push_back(id);
        trackIds.push_back(track->id);
    }
    const std::vector<ExactTrackState> before = snapshots(trackIds);
    for (const Ulid& id : operation.clip_ids) {
        RemoveClipOperation single{id, {}};
        Operation ignored = RemoveClipOperation{};
        if (!ApplyRemove(candidate, single, ignored, error, message))
            return false;
    }
    operation.exact_track_result = snapshots(trackIds);
    inverse = RemoveLinkedClipsOperation{operation.link_group_id,
                                         operation.clip_ids, before};
    return true;
}

bool ApplyClearLinked(Document& candidate,
                      ClearLinkedClipsOperation& operation,
                      Operation& inverse, EditError& error,
                      std::string& message) {
    const auto snapshots = [&](const std::vector<Ulid>& ids) {
        std::vector<ExactTrackState> result;
        for (const Ulid& id : ids) {
            const DocumentTrack* track = candidate.FindTrackForClip(id);
            if (!track || std::any_of(result.begin(), result.end(),
                                      [&](const ExactTrackState& state) {
                                          return state.track_id == track->id;
                                      }))
                continue;
            result.push_back({track->id, track->clips});
        }
        return result;
    };
    if (!operation.exact_track_result.empty()) {
        std::vector<ExactTrackState> before;
        for (const ExactTrackState& state : operation.exact_track_result) {
            DocumentTrack* track = candidate.FindTrack(state.track_id);
            if (!track) {
                Fail(EditError::UnknownTrack,
                     "exact linked clear references an unknown track", error,
                     message);
                return false;
            }
            before.push_back({track->id, track->clips});
        }
        for (const ExactTrackState& state : operation.exact_track_result)
            candidate.FindTrack(state.track_id)->clips = state.clips;
        if (!ValidateResult(candidate, error, message)) return false;
        inverse = ClearLinkedClipsOperation{operation.link_group_id,
                                            operation.clip_ids, before};
        return true;
    }
    if (operation.link_group_id.empty() || operation.clip_ids.size() < 2) {
        Fail(EditError::InvalidOperation,
             "ClearLinkedClips requires at least two linked members", error,
             message);
        return false;
    }
    std::vector<Ulid> seen;
    for (const Ulid& id : operation.clip_ids) {
        const DocumentClip* clip = candidate.FindClip(id);
        if (!clip || clip->link_group_id != operation.link_group_id ||
            std::find(seen.begin(), seen.end(), id) != seen.end()) {
            Fail(EditError::InvalidOperation,
                 "linked clear members must be unique and share "
                 "link_group_id",
                 error, message);
            return false;
        }
        seen.push_back(id);
    }
    const std::vector<ExactTrackState> before = snapshots(operation.clip_ids);
    for (const Ulid& id : operation.clip_ids) {
        DocumentTrack* track = candidate.FindTrackForClip(id);
        track->clips.erase(
            std::remove_if(track->clips.begin(), track->clips.end(),
                           [&](const DocumentClip& clip) {
                               return clip.id == id;
                           }),
            track->clips.end());
    }
    if (!ValidateResult(candidate, error, message)) return false;
    operation.exact_track_result = before;
    for (ExactTrackState& state : operation.exact_track_result)
        state.clips = candidate.FindTrack(state.track_id)->clips;
    inverse = ClearLinkedClipsOperation{operation.link_group_id,
                                        operation.clip_ids, before};
    return true;
}

bool ApplyDeleteGap(Document& candidate, DeleteGapOperation& operation,
                    Operation& inverse, EditError& error,
                    std::string& message) {
    std::vector<Ulid> trackIds{operation.track_id};
    for (const Ulid& id : operation.linked_track_ids) {
        if (id == operation.track_id ||
            std::find(trackIds.begin(), trackIds.end(), id) != trackIds.end()) {
            Fail(EditError::InvalidOperation,
                 "linked gap track_ids must be unique", error, message);
            return false;
        }
        trackIds.push_back(id);
    }
    std::vector<ExactTrackState> before;
    for (const Ulid& id : trackIds) {
        DocumentTrack* track = candidate.FindTrack(id);
        if (!track) {
            Fail(EditError::UnknownTrack, "unknown track_id '" + id + "'",
                 error, message);
            return false;
        }
        before.push_back({track->id, track->clips});
    }
    if (!operation.exact_track_result.empty()) {
        if (operation.exact_track_result.size() != trackIds.size()) {
            Fail(EditError::InvalidOperation,
                 "exact gap state must contain every destination track", error,
                 message);
            return false;
        }
        for (const ExactTrackState& state : operation.exact_track_result) {
            if (std::find(trackIds.begin(), trackIds.end(), state.track_id) ==
                trackIds.end()) {
                Fail(EditError::InvalidOperation,
                     "exact gap state contains an unexpected track", error,
                     message);
                return false;
            }
            candidate.FindTrack(state.track_id)->clips = state.clips;
        }
        if (!ValidateResult(candidate, error, message)) return false;
        inverse = DeleteGapOperation{operation.track_id,
                                     operation.gap_start,
                                     operation.gap_duration,
                                     before,
                                     operation.linked_track_ids};
        return true;
    }
    if (operation.gap_start.rate <= 0 || operation.gap_start.value < 0 ||
        operation.gap_duration.rate <= 0 || operation.gap_duration.value <= 0) {
        Fail(EditError::InvalidOperation,
             "gap start and duration must describe a positive range", error,
             message);
        return false;
    }
    const RationalTime gapEnd = operation.gap_start.add(operation.gap_duration);
    for (const Ulid& id : trackIds) {
        DocumentTrack* track = candidate.FindTrack(id);
        bool hasFollowingClip = false;
        for (const DocumentClip& clip : track->clips) {
            const RationalTime clipEnd = clip.timeline_in.add(clip.duration);
            if (clip.timeline_in < gapEnd && clipEnd > operation.gap_start) {
                Fail(EditError::InvalidOperation,
                     "DeleteGap range contains clip_id '" + clip.id + "'",
                     error, message);
                return false;
            }
            if (clip.timeline_in >= gapEnd) hasFollowingClip = true;
        }
        if (!hasFollowingClip) {
            Fail(EditError::InvalidOperation,
                 "DeleteGap requires a following clip on every linked track",
                 error, message);
            return false;
        }
        for (DocumentClip& clip : track->clips) {
            if (clip.timeline_in >= gapEnd)
                clip.timeline_in = clip.timeline_in.sub(operation.gap_duration);
        }
    }
    if (!ValidateResult(candidate, error, message)) return false;
    operation.exact_track_result.clear();
    for (const Ulid& id : trackIds) {
        const DocumentTrack* track = candidate.FindTrack(id);
        operation.exact_track_result.push_back({id, track->clips});
    }
    inverse = DeleteGapOperation{operation.track_id,
                                 operation.gap_start,
                                 operation.gap_duration,
                                 before,
                                 operation.linked_track_ids};
    return true;
}

bool ApplyDetachAudio(Document& candidate, DetachAudioOperation& operation,
                      Operation& inverse, EditError& error,
                      std::string& message) {
    DocumentTrack* videoTrack =
        candidate.FindTrackForClip(operation.video_clip_id);
    DocumentTrack* audioTrack = candidate.FindTrack(operation.audio_track_id);
    if (!videoTrack) {
        Fail(EditError::UnknownClip,
             "unknown video_clip_id '" + operation.video_clip_id + "'", error,
             message);
        return false;
    }
    if (!audioTrack) {
        Fail(EditError::UnknownTrack,
             "unknown audio_track_id '" + operation.audio_track_id + "'", error,
             message);
        return false;
    }
    const auto snapshots = [&](const std::vector<Ulid>& ids) {
        std::vector<ExactTrackState> result;
        for (const Ulid& id : ids) {
            if (std::any_of(result.begin(), result.end(),
                            [&](const ExactTrackState& state) {
                                return state.track_id == id;
                            }))
                continue;
            const DocumentTrack* track = candidate.FindTrack(id);
            if (track) result.push_back({id, track->clips});
        }
        return result;
    };
    if (!operation.exact_track_result.empty()) {
        std::vector<Ulid> ids;
        for (const ExactTrackState& state : operation.exact_track_result)
            ids.push_back(state.track_id);
        const std::vector<ExactTrackState> before = snapshots(ids);
        if (before.size() != operation.exact_track_result.size()) {
            Fail(EditError::UnknownTrack,
                 "exact detach state references an unknown track", error,
                 message);
            return false;
        }
        for (const ExactTrackState& state : operation.exact_track_result)
            candidate.FindTrack(state.track_id)->clips = state.clips;
        if (!ValidateResult(candidate, error, message)) return false;
        inverse = DetachAudioOperation{operation.video_clip_id,
                                       operation.audio_track_id,
                                       operation.audio_clip_id, before};
        return true;
    }
    if (videoTrack->kind != "video" || audioTrack->kind != "audio") {
        Fail(EditError::InvalidOperation,
             "DetachAudio requires a video clip and an audio track", error,
             message);
        return false;
    }
    DocumentClip* videoClip = candidate.FindClip(operation.video_clip_id);
    if (!videoClip->include_audio) {
        Fail(EditError::InvalidOperation,
             "video clip audio is already detached", error, message);
        return false;
    }
    const LibraryMedia* media =
        candidate.FindLibraryMedia(videoClip->source_id);
    if (media && media->metadata_complete && !media->has_audio) {
        Fail(EditError::InvalidOperation, "source media has no audio stream",
             error, message);
        return false;
    }
    if (operation.audio_clip_id.empty())
        operation.audio_clip_id = GenerateUlid();
    if (!IsValidUlid(operation.audio_clip_id) ||
        candidate.FindClip(operation.audio_clip_id) ||
        candidate.FindTrack(operation.audio_clip_id) ||
        candidate.FindSource(operation.audio_clip_id) ||
        candidate.FindLibraryMedia(operation.audio_clip_id)) {
        Fail(EditError::DuplicateId,
             "audio_clip_id is invalid or already exists", error, message);
        return false;
    }
    const std::vector<ExactTrackState> before =
        snapshots({videoTrack->id, audioTrack->id});
    const RationalTime detachedEnd =
        videoClip->timeline_in.add(videoClip->duration);
    for (const DocumentClip& existing : audioTrack->clips) {
        if (existing.timeline_in < detachedEnd &&
            existing.timeline_in.add(existing.duration) >
                videoClip->timeline_in) {
            Fail(EditError::Overlap,
                 "detached audio would overlap clip_id '" + existing.id + "'",
                 error, message);
            return false;
        }
    }
    DocumentClip audioClip = *videoClip;
    audioClip.id = operation.audio_clip_id;
    videoClip->include_audio = false;
    videoClip->link_group_id = operation.audio_clip_id;
    videoClip->sync_anchor_clip_id = videoClip->id;
    videoClip->sync_reference_delta = {0, 1};
    audioClip.link_group_id = operation.audio_clip_id;
    audioClip.sync_anchor_clip_id = videoClip->id;
    audioClip.sync_reference_delta = {0, 1};
    audioTrack->clips.push_back(std::move(audioClip));
    std::stable_sort(audioTrack->clips.begin(), audioTrack->clips.end(),
                     [](const DocumentClip& left, const DocumentClip& right) {
                         return left.timeline_in < right.timeline_in;
                     });
    if (!ValidateResult(candidate, error, message)) return false;
    operation.exact_track_result = snapshots({videoTrack->id, audioTrack->id});
    inverse =
        DetachAudioOperation{operation.video_clip_id, operation.audio_track_id,
                             operation.audio_clip_id, before};
    return true;
}

bool ApplyAddTrack(Document& candidate, AddTrackOperation& operation,
                   Operation& inverse, EditError& error, std::string& message) {
    if (operation.track_id.empty()) operation.track_id = GenerateUlid();
    if (!IsValidUlid(operation.track_id) ||
        candidate.FindTrack(operation.track_id) ||
        candidate.FindClip(operation.track_id) ||
        candidate.FindSource(operation.track_id) ||
        candidate.FindLibraryMedia(operation.track_id)) {
        Fail(EditError::DuplicateId,
             "track_id is invalid or already exists: '" + operation.track_id +
                 "'",
             error, message);
        return false;
    }
    if (operation.kind != "video" && operation.kind != "audio") {
        Fail(EditError::InvalidOperation,
             "track kind must be 'video' or 'audio'", error, message);
        return false;
    }
    if (operation.index < 0 ||
        std::any_of(candidate.sequence.tracks.begin(), candidate.sequence.tracks.end(),
                    [&](const DocumentTrack& track) {
                        return track.index == operation.index;
                    })) {
        Fail(EditError::InvalidOperation,
             "track index must be non-negative and unique", error, message);
        return false;
    }
    candidate.sequence.tracks.push_back(
        {operation.track_id, operation.kind, operation.index, operation.clips});
    if (!ValidateResult(candidate, error, message)) return false;
    inverse = RemoveTrackOperation{operation.track_id};
    return true;
}

bool ApplyRemoveTrack(Document& candidate, RemoveTrackOperation& operation,
                      Operation& inverse, EditError& error,
                      std::string& message) {
    const auto found =
        std::find_if(candidate.sequence.tracks.begin(), candidate.sequence.tracks.end(),
                     [&](const DocumentTrack& track) {
                         return track.id == operation.track_id;
                     });
    if (found == candidate.sequence.tracks.end()) {
        Fail(EditError::UnknownTrack,
             "unknown track_id '" + operation.track_id + "'", error, message);
        return false;
    }
    inverse =
        AddTrackOperation{found->id, found->kind, found->index, found->clips};
    candidate.sequence.tracks.erase(found);
    return ValidateResult(candidate, error, message);
}

bool ApplyUpdateSequence(Document& candidate,
                         UpdateSequenceOperation& operation,
                         Operation& inverse, EditError& error,
                         std::string& message) {
    if (operation.sequence_id != candidate.sequence.id) {
        Fail(EditError::UnknownSequence,
             "unknown sequence_id '" + operation.sequence_id + "'", error,
             message);
        return false;
    }
    const DocumentSequence& current = candidate.sequence;
    inverse = UpdateSequenceOperation{current.id, current.name, current.width,
                                      current.height, current.frame_rate};
    candidate.sequence.name = operation.name;
    candidate.sequence.width = operation.width;
    candidate.sequence.height = operation.height;
    candidate.sequence.frame_rate = operation.frame_rate;
    return ValidateResult(candidate, error, message);
}

bool ApplyAddBin(Document& candidate, AddBinOperation& operation,
                 Operation& inverse, EditError& error, std::string& message) {
    if (operation.bin_id.empty()) operation.bin_id = GenerateUlid();
    if (!IsValidUlid(operation.bin_id) || candidate.FindBin(operation.bin_id) ||
        candidate.FindTrack(operation.bin_id) ||
        candidate.FindClip(operation.bin_id) ||
        candidate.FindSource(operation.bin_id) ||
        candidate.FindLibraryMedia(operation.bin_id)) {
        Fail(EditError::DuplicateId,
             "bin_id is invalid or already exists: '" + operation.bin_id + "'",
             error, message);
        return false;
    }
    if (operation.name.empty() || operation.name.size() > 128 ||
        std::any_of(operation.name.begin(), operation.name.end(),
                    [](unsigned char character) { return character < 0x20; })) {
        Fail(EditError::InvalidOperation,
             "bin name must contain between 1 and 128 bytes", error, message);
        return false;
    }
    if (!operation.parent_id.empty() &&
        !candidate.FindBin(operation.parent_id)) {
        Fail(EditError::UnknownBin,
             "unknown parent bin_id '" + operation.parent_id + "'", error,
             message);
        return false;
    }
    candidate.bins.push_back(
        {operation.bin_id, operation.name, operation.parent_id});
    if (!ValidateResult(candidate, error, message)) return false;
    inverse = RemoveBinOperation{operation.bin_id, operation.name,
                                 operation.parent_id};
    return true;
}

bool ApplyRemoveBin(Document& candidate, RemoveBinOperation& operation,
                    Operation& inverse, EditError& error,
                    std::string& message) {
    const auto found = std::find_if(
        candidate.bins.begin(), candidate.bins.end(),
        [&](const DocumentBin& bin) { return bin.id == operation.bin_id; });
    if (found == candidate.bins.end()) {
        Fail(EditError::UnknownBin, "unknown bin_id '" + operation.bin_id + "'",
             error, message);
        return false;
    }
    if (std::any_of(candidate.library.begin(), candidate.library.end(),
                    [&](const LibraryMedia& media) {
                        return media.bin_id == operation.bin_id;
                    })) {
        Fail(EditError::InvalidOperation,
             "cannot remove a bin that still contains media", error, message);
        return false;
    }
    if (std::any_of(candidate.bins.begin(), candidate.bins.end(),
                    [&](const DocumentBin& bin) {
                        return bin.parent_id == operation.bin_id;
                    })) {
        Fail(EditError::InvalidOperation,
             "cannot remove a bin that still contains child bins", error,
             message);
        return false;
    }
    operation.name = found->name;
    operation.parent_id = found->parent_id;
    inverse = AddBinOperation{found->id, found->name, found->parent_id};
    candidate.bins.erase(found);
    return ValidateResult(candidate, error, message);
}

bool ApplyRenameBin(Document& candidate, RenameBinOperation& operation,
                    Operation& inverse, EditError& error,
                    std::string& message) {
    DocumentBin* bin = candidate.FindBin(operation.bin_id);
    if (!bin) {
        Fail(EditError::UnknownBin, "unknown bin_id '" + operation.bin_id + "'",
             error, message);
        return false;
    }
    if (operation.name.empty() || operation.name.size() > 128 ||
        std::any_of(operation.name.begin(), operation.name.end(),
                    [](unsigned char character) { return character < 0x20; })) {
        Fail(EditError::InvalidOperation,
             "bin name must contain between 1 and 128 bytes", error, message);
        return false;
    }
    inverse = RenameBinOperation{bin->id, bin->name};
    bin->name = operation.name;
    return ValidateResult(candidate, error, message);
}

bool ApplyMoveBin(Document& candidate, MoveBinOperation& operation,
                  Operation& inverse, EditError& error,
                  std::string& message) {
    DocumentBin* bin = candidate.FindBin(operation.bin_id);
    if (!bin) {
        Fail(EditError::UnknownBin, "unknown bin_id '" + operation.bin_id +
                                        "'",
             error, message);
        return false;
    }
    if (operation.parent_id == operation.bin_id) {
        Fail(EditError::InvalidOperation, "a bin cannot contain itself", error,
             message);
        return false;
    }
    if (!operation.parent_id.empty() &&
        !candidate.FindBin(operation.parent_id)) {
        Fail(EditError::UnknownBin,
             "unknown parent bin_id '" + operation.parent_id + "'", error,
             message);
        return false;
    }
    const DocumentBin* cursor = candidate.FindBin(operation.parent_id);
    while (cursor) {
        if (cursor->id == operation.bin_id) {
            Fail(EditError::InvalidOperation,
                 "a bin cannot be moved into one of its descendants", error,
                 message);
            return false;
        }
        cursor = candidate.FindBin(cursor->parent_id);
    }
    inverse = MoveBinOperation{bin->id, bin->parent_id};
    bin->parent_id = operation.parent_id;
    return ValidateResult(candidate, error, message);
}

bool ApplySetMediaBin(Document& candidate, SetMediaBinOperation& operation,
                      Operation& inverse, EditError& error,
                      std::string& message) {
    LibraryMedia* media = candidate.FindLibraryMedia(operation.media_id);
    if (!media) {
        Fail(EditError::UnknownMedia,
             "unknown media_id '" + operation.media_id + "'", error, message);
        return false;
    }
    if (!operation.bin_id.empty() && !candidate.FindBin(operation.bin_id)) {
        Fail(EditError::UnknownBin, "unknown bin_id '" + operation.bin_id + "'",
             error, message);
        return false;
    }
    inverse = SetMediaBinOperation{operation.media_id, media->bin_id};
    media->bin_id = operation.bin_id;
    return ValidateResult(candidate, error, message);
}

bool ApplyAddMarker(Document& candidate, AddMarkerOperation& operation,
                    Operation& inverse, EditError& error,
                    std::string& message) {
    if (operation.marker.id.empty()) operation.marker.id = GenerateUlid();
    const Ulid& id = operation.marker.id;
    const bool collision =
        id == candidate.sequence.id || candidate.FindMarker(id) ||
        candidate.FindBin(id) || candidate.FindLibraryMedia(id) ||
        candidate.FindSource(id) || candidate.FindTrack(id) ||
        candidate.FindClip(id);
    if (!IsValidUlid(id) || collision) {
        Fail(EditError::DuplicateId,
             "marker_id is invalid or already exists: '" + id + "'", error,
             message);
        return false;
    }
    if (operation.insertion_index < -1 ||
        (operation.insertion_index >= 0 &&
         static_cast<uint64_t>(operation.insertion_index) >
             candidate.sequence.markers.size())) {
        Fail(EditError::InvalidOperation,
             "marker insertion_index is outside the marker list", error,
             message);
        return false;
    }
    if (operation.insertion_index < 0)
        operation.insertion_index =
            static_cast<int64_t>(candidate.sequence.markers.size());
    candidate.sequence.markers.insert(
        candidate.sequence.markers.begin() + operation.insertion_index,
        operation.marker);
    if (!ValidateResult(candidate, error, message)) return false;
    inverse = RemoveMarkerOperation{id};
    return true;
}

bool ApplyRemoveMarker(Document& candidate, RemoveMarkerOperation& operation,
                       Operation& inverse, EditError& error,
                       std::string& message) {
    const auto found = std::find_if(
        candidate.sequence.markers.begin(), candidate.sequence.markers.end(),
        [&](const DocumentMarker& marker) { return marker.id == operation.marker_id; });
    if (found == candidate.sequence.markers.end()) {
        Fail(EditError::UnknownMarker,
             "unknown marker_id '" + operation.marker_id + "'", error,
             message);
        return false;
    }
    const int64_t index = static_cast<int64_t>(
        std::distance(candidate.sequence.markers.begin(), found));
    const DocumentMarker removed = *found;
    candidate.sequence.markers.erase(found);
    if (!ValidateResult(candidate, error, message)) return false;
    inverse = AddMarkerOperation{removed, index};
    return true;
}

bool ApplyUpdateMarker(Document& candidate, UpdateMarkerOperation& operation,
                       Operation& inverse, EditError& error,
                       std::string& message) {
    DocumentMarker* marker = candidate.FindMarker(operation.marker_id);
    if (!marker) {
        Fail(EditError::UnknownMarker,
             "unknown marker_id '" + operation.marker_id + "'", error,
             message);
        return false;
    }
    const DocumentMarker before = *marker;
    marker->name = operation.name;
    marker->time = operation.time;
    marker->color = operation.color;
    marker->category = operation.category;
    if (!ValidateResult(candidate, error, message)) return false;
    inverse = UpdateMarkerOperation{before.id, before.name, before.time,
                                    before.color, before.category};
    return true;
}

bool ApplySetClipLink(Document& candidate, SetClipLinkOperation& operation,
                      Operation& inverse, EditError& error,
                      std::string& message) {
    DocumentClip* first = candidate.FindClip(operation.first_clip_id);
    DocumentClip* second = candidate.FindClip(operation.second_clip_id);
    if (!first || !second || first == second) {
        Fail(EditError::UnknownClip,
             "SetClipLink requires two distinct existing clips", error,
             message);
        return false;
    }
    if ((!operation.first_group_id.empty() &&
         !IsValidUlid(operation.first_group_id)) ||
        (!operation.second_group_id.empty() &&
         !IsValidUlid(operation.second_group_id)) ||
        (operation.exact_first_result.has_value() !=
         operation.exact_second_result.has_value())) {
        Fail(EditError::InvalidOperation,
             "link group IDs must be empty or valid ULIDs", error, message);
        return false;
    }
    const SetClipLinkOperation::ExactState oldFirst{
        first->link_group_id, first->sync_anchor_clip_id,
        first->sync_reference_delta};
    const SetClipLinkOperation::ExactState oldSecond{
        second->link_group_id, second->sync_anchor_clip_id,
        second->sync_reference_delta};
    inverse =
        SetClipLinkOperation{first->id,          second->id, oldFirst.group_id,
                             oldSecond.group_id, oldFirst,   oldSecond};
    if (operation.exact_first_result) {
        const auto apply = [](DocumentClip& clip,
                              const SetClipLinkOperation::ExactState& state) {
            clip.link_group_id = state.group_id;
            clip.sync_anchor_clip_id = state.anchor_clip_id;
            clip.sync_reference_delta = state.reference_delta;
        };
        apply(*first, *operation.exact_first_result);
        apply(*second, *operation.exact_second_result);
    } else if (operation.first_group_id.empty() &&
               operation.second_group_id.empty()) {
        first->link_group_id.clear();
        first->sync_anchor_clip_id.clear();
        first->sync_reference_delta = {0, 1};
        second->link_group_id.clear();
        second->sync_anchor_clip_id.clear();
        second->sync_reference_delta = {0, 1};
    } else {
        if (operation.first_group_id != operation.second_group_id) {
            Fail(EditError::InvalidOperation,
                 "newly linked clips must share one group ID", error, message);
            return false;
        }
        const DocumentTrack* firstTrack = candidate.FindTrackForClip(first->id);
        DocumentClip* anchor =
            firstTrack && firstTrack->kind == "video" ? first : second;
        DocumentClip* member = anchor == first ? second : first;
        anchor->link_group_id = operation.first_group_id;
        anchor->sync_anchor_clip_id = anchor->id;
        anchor->sync_reference_delta = {0, 1};
        member->link_group_id = operation.first_group_id;
        member->sync_anchor_clip_id = anchor->id;
        member->sync_reference_delta = PhaseOf(*member).sub(PhaseOf(*anchor));
    }
    operation.exact_first_result = SetClipLinkOperation::ExactState{
        first->link_group_id, first->sync_anchor_clip_id,
        first->sync_reference_delta};
    operation.exact_second_result = SetClipLinkOperation::ExactState{
        second->link_group_id, second->sync_anchor_clip_id,
        second->sync_reference_delta};
    return ValidateResult(candidate, error, message);
}

bool ApplySplit(Document& candidate, SplitClipOperation& operation,
                Operation& inverse, EditError& error, std::string& message) {
    DocumentTrack* track = candidate.FindTrackForClip(operation.clip_id);
    if (!track) {
        Fail(EditError::UnknownClip,
             "unknown clip_id '" + operation.clip_id + "'", error, message);
        return false;
    }
    const auto found = std::find_if(
        track->clips.begin(), track->clips.end(),
        [&](const DocumentClip& clip) { return clip.id == operation.clip_id; });
    const size_t index =
        static_cast<size_t>(std::distance(track->clips.begin(), found));
    const DocumentClip original = *found;
    const RationalTime end = original.timeline_in.add(original.duration);
    if (operation.timeline_position.rate <= 0 ||
        operation.timeline_position <= original.timeline_in ||
        operation.timeline_position >= end) {
        Fail(EditError::InvalidTimelineIn,
             "split position must be strictly inside the clip", error, message);
        return false;
    }
    if (operation.right_clip_id.empty())
        operation.right_clip_id = GenerateUlid();
    if (!IsValidUlid(operation.right_clip_id) ||
        candidate.FindClip(operation.right_clip_id) ||
        candidate.FindSource(operation.right_clip_id) ||
        candidate.FindTrack(operation.right_clip_id)) {
        Fail(EditError::DuplicateId,
             "split right_clip_id is invalid or already exists: '" +
                 operation.right_clip_id + "'",
             error, message);
        return false;
    }

    const RationalTime leftDuration =
        operation.timeline_position.sub(original.timeline_in);
    DocumentClip left = original;
    left.duration = leftDuration;
    DocumentClip right{operation.right_clip_id, original.source_id,
                       original.source_in.add(leftDuration),
                       original.duration.sub(leftDuration),
                       operation.timeline_position};
    right.include_audio = original.include_audio;
    right.link_group_id = original.link_group_id;
    right.sync_anchor_clip_id = original.sync_anchor_clip_id;
    right.sync_reference_delta = original.sync_reference_delta;
    track->clips[index] = std::move(left);
    track->clips.insert(
        track->clips.begin() + static_cast<std::ptrdiff_t>(index + 1),
        std::move(right));
    if (!ValidateResult(candidate, error, message)) return false;
    inverse = JoinClipOperation{original.id, operation.right_clip_id,
                                TimesOf(original)};
    return true;
}

bool ApplySplitLinked(Document& candidate,
                      SplitLinkedClipsOperation& operation,
                      Operation& inverse, EditError& error,
                      std::string& message) {
    const auto snapshots = [&](const std::vector<Ulid>& ids) {
        std::vector<ExactTrackState> result;
        for (const Ulid& id : ids) {
            const DocumentTrack* track = candidate.FindTrackForClip(id);
            if (!track || std::any_of(result.begin(), result.end(),
                                      [&](const ExactTrackState& state) {
                                          return state.track_id == track->id;
                                      }))
                continue;
            result.push_back({track->id, track->clips});
        }
        return result;
    };
    if (!operation.exact_track_result.empty()) {
        std::vector<ExactTrackState> before;
        for (const ExactTrackState& state : operation.exact_track_result) {
            DocumentTrack* track = candidate.FindTrack(state.track_id);
            if (!track) {
                Fail(EditError::UnknownTrack,
                     "exact linked split references an unknown track", error,
                     message);
                return false;
            }
            before.push_back({track->id, track->clips});
        }
        for (const ExactTrackState& state : operation.exact_track_result)
            candidate.FindTrack(state.track_id)->clips = state.clips;
        if (!ValidateResult(candidate, error, message)) return false;
        inverse = SplitLinkedClipsOperation{
            operation.link_group_id, operation.clip_ids,
            operation.timeline_position, operation.left_group_id,
            operation.right_group_id, operation.right_clip_ids,
            std::move(before)};
        return true;
    }
    if (operation.link_group_id.empty() || operation.clip_ids.size() < 2) {
        Fail(EditError::InvalidOperation,
             "SplitLinkedClips requires at least two linked members", error,
             message);
        return false;
    }
    std::vector<Ulid> seen;
    for (const Ulid& id : operation.clip_ids) {
        const DocumentClip* clip = candidate.FindClip(id);
        if (!clip || clip->link_group_id != operation.link_group_id ||
            std::find(seen.begin(), seen.end(), id) != seen.end() ||
            operation.timeline_position <= clip->timeline_in ||
            operation.timeline_position >=
                clip->timeline_in.add(clip->duration)) {
            Fail(EditError::InvalidOperation,
                 "linked split members must be unique, share link_group_id, "
                 "and contain the cut position",
                 error, message);
            return false;
        }
        seen.push_back(id);
    }
    if (operation.left_group_id.empty()) operation.left_group_id = GenerateUlid();
    if (operation.right_group_id.empty())
        operation.right_group_id = GenerateUlid();
    if (!IsValidUlid(operation.left_group_id) ||
        !IsValidUlid(operation.right_group_id) ||
        operation.left_group_id == operation.right_group_id) {
        Fail(EditError::InvalidOperation,
             "linked split requires two distinct valid group IDs", error,
             message);
        return false;
    }
    if (operation.right_clip_ids.empty()) {
        for (size_t index = 0; index < operation.clip_ids.size(); ++index)
            operation.right_clip_ids.push_back(GenerateUlid());
    }
    if (operation.right_clip_ids.size() != operation.clip_ids.size()) {
        Fail(EditError::InvalidOperation,
             "linked split requires one right clip ID per member", error,
             message);
        return false;
    }
    std::vector<Ulid> generated;
    for (const Ulid& id : operation.right_clip_ids) {
        if (!IsValidUlid(id) || candidate.FindClip(id) ||
            candidate.FindTrack(id) || candidate.FindSource(id) ||
            std::find(generated.begin(), generated.end(), id) !=
                generated.end()) {
            Fail(EditError::DuplicateId,
                 "linked split right clip ID is invalid or already exists",
                 error, message);
            return false;
        }
        generated.push_back(id);
    }

    const std::vector<ExactTrackState> before = snapshots(operation.clip_ids);
    Ulid leftAnchor = operation.clip_ids.front();
    for (const Ulid& id : operation.clip_ids) {
        const DocumentClip* clip = candidate.FindClip(id);
        if (clip && clip->sync_anchor_clip_id == id) {
            leftAnchor = id;
            break;
        }
    }
    const auto anchorPosition = std::find(operation.clip_ids.begin(),
                                          operation.clip_ids.end(), leftAnchor);
    const size_t anchorIndex = static_cast<size_t>(
        std::distance(operation.clip_ids.begin(), anchorPosition));
    const Ulid rightAnchor = operation.right_clip_ids[anchorIndex];
    for (size_t index = 0; index < operation.clip_ids.size(); ++index) {
        SplitClipOperation split{operation.clip_ids[index],
                                 operation.timeline_position,
                                 operation.right_clip_ids[index]};
        Operation ignored = RemoveClipOperation{};
        if (!ApplySplit(candidate, split, ignored, error, message)) return false;
    }
    for (size_t index = 0; index < operation.clip_ids.size(); ++index) {
        DocumentClip* left = candidate.FindClip(operation.clip_ids[index]);
        DocumentClip* right = candidate.FindClip(operation.right_clip_ids[index]);
        left->link_group_id = operation.left_group_id;
        left->sync_anchor_clip_id = leftAnchor;
        right->link_group_id = operation.right_group_id;
        right->sync_anchor_clip_id = rightAnchor;
    }
    if (!ValidateResult(candidate, error, message)) return false;
    operation.exact_track_result = snapshots(operation.clip_ids);
    inverse = SplitLinkedClipsOperation{
        operation.link_group_id, operation.clip_ids, operation.timeline_position,
        operation.left_group_id, operation.right_group_id,
        operation.right_clip_ids, before};
    return true;
}

bool ApplyJoin(Document& candidate, JoinClipOperation& operation,
               Operation& inverse, EditError& error, std::string& message) {
    DocumentTrack* track = candidate.FindTrackForClip(operation.left_clip_id);
    if (!track) {
        Fail(EditError::UnknownClip,
             "unknown left_clip_id '" + operation.left_clip_id + "'", error,
             message);
        return false;
    }
    const auto left = std::find_if(track->clips.begin(), track->clips.end(),
                                   [&](const DocumentClip& clip) {
                                       return clip.id == operation.left_clip_id;
                                   });
    if (left == track->clips.end() || std::next(left) == track->clips.end() ||
        std::next(left)->id != operation.right_clip_id) {
        Fail(EditError::InvalidOperation,
             "join clips must be adjacent on the same track", error, message);
        return false;
    }
    const DocumentClip right = *std::next(left);
    if (left->source_id != right.source_id ||
        left->timeline_in.add(left->duration) != right.timeline_in ||
        left->source_in.add(left->duration) != right.source_in) {
        Fail(EditError::InvalidOperation,
             "join clips are not contiguous pieces of one source", error,
             message);
        return false;
    }
    const RationalTime splitPosition = right.timeline_in;
    left->source_in = operation.joined_times.source_in;
    left->duration = operation.joined_times.duration;
    left->timeline_in = operation.joined_times.timeline_in;
    track->clips.erase(std::next(left));
    if (!ValidateResult(candidate, error, message)) return false;
    inverse = SplitClipOperation{operation.left_clip_id, splitPosition,
                                 operation.right_clip_id};
    return true;
}

void WriteTime(std::ostringstream& output, const RationalTime& time) {
    output << "{\"value\":" << time.value << ",\"rate\":" << time.rate << "}";
}

void WriteString(std::ostringstream& output, const std::string& value) {
    output << '"';
    for (const char character : value) {
        if (character == '"' || character == '\\') output << '\\';
        output << character;
    }
    output << '"';
}

void WriteExactPositions(std::ostringstream& output,
                         const std::vector<ExactTimelinePosition>& positions) {
    output << '[';
    for (size_t index = 0; index < positions.size(); ++index) {
        if (index) output << ',';
        output << "{\"clip_id\":\"" << positions[index].clip_id
               << "\",\"timeline_in\":";
        WriteTime(output, positions[index].timeline_in);
        output << '}';
    }
    output << ']';
}

void WriteExactTracks(std::ostringstream& output,
                      const std::vector<ExactTrackState>& tracks) {
    output << '[';
    for (size_t trackIndex = 0; trackIndex < tracks.size(); ++trackIndex) {
        if (trackIndex) output << ',';
        output << "{\"track_id\":\"" << tracks[trackIndex].track_id
               << "\",\"clips\":[";
        for (size_t clipIndex = 0; clipIndex < tracks[trackIndex].clips.size();
             ++clipIndex) {
            if (clipIndex) output << ',';
            const DocumentClip& clip = tracks[trackIndex].clips[clipIndex];
            output << "{\"id\":\"" << clip.id << "\",\"source_id\":\""
                   << clip.source_id << "\",\"source_in\":";
            WriteTime(output, clip.source_in);
            output << ",\"duration\":";
            WriteTime(output, clip.duration);
            output << ",\"timeline_in\":";
            WriteTime(output, clip.timeline_in);
            output << ",\"include_audio\":"
                   << (clip.include_audio ? "true" : "false");
            if (!clip.link_group_id.empty())
                output << ",\"link_group_id\":\"" << clip.link_group_id << "\"";
            if (!clip.sync_anchor_clip_id.empty()) {
                output << ",\"sync_anchor_clip_id\":\""
                       << clip.sync_anchor_clip_id
                       << "\",\"sync_reference_delta\":";
                WriteTime(output, clip.sync_reference_delta);
            }
            output << '}';
        }
        output << "]}";
    }
    output << ']';
}

class Reader {
public:
    explicit Reader(const std::string& input) : input_(input) {}

    void Expect(const std::string& text) {
        Skip();
        if (input_.compare(position_, text.size(), text) != 0) {
            throw std::runtime_error("expected '" + text + "' at byte " +
                                     std::to_string(position_));
        }
        position_ += text.size();
    }

    bool Consume(const std::string& text) {
        Skip();
        if (input_.compare(position_, text.size(), text) != 0) return false;
        position_ += text.size();
        return true;
    }

    std::string String() {
        Skip();
        if (position_ >= input_.size() || input_[position_++] != '"') {
            throw std::runtime_error("expected string at byte " +
                                     std::to_string(position_));
        }
        std::string output;
        while (position_ < input_.size()) {
            const char character = input_[position_++];
            if (character == '"') return output;
            if (character == '\\') {
                if (position_ >= input_.size())
                    throw std::runtime_error("unterminated string escape");
                const char escaped = input_[position_++];
                if (escaped != '"' && escaped != '\\')
                    throw std::runtime_error("unsupported string escape");
                output.push_back(escaped);
            } else {
                output.push_back(character);
            }
        }
        throw std::runtime_error("unterminated string");
    }

    int64_t Integer() {
        Skip();
        const size_t start = position_;
        if (position_ < input_.size() && input_[position_] == '-') ++position_;
        while (position_ < input_.size() &&
               std::isdigit(static_cast<unsigned char>(input_[position_]))) {
            ++position_;
        }
        if (position_ == start ||
            (position_ == start + 1 && input_[start] == '-')) {
            throw std::runtime_error("expected integer at byte " +
                                     std::to_string(start));
        }
        char* end = nullptr;
        const std::string text = input_.substr(start, position_ - start);
        const long long value = std::strtoll(text.c_str(), &end, 10);
        if (!end || *end != '\0') throw std::runtime_error("invalid integer");
        return static_cast<int64_t>(value);
    }

    void Finish() {
        Skip();
        if (position_ != input_.size())
            throw std::runtime_error("unexpected trailing operation JSON");
    }

private:
    void Skip() {
        while (position_ < input_.size() &&
               std::isspace(static_cast<unsigned char>(input_[position_]))) {
            ++position_;
        }
    }
    const std::string& input_;
    size_t position_ = 0;
};

RationalTime ReadTime(Reader& reader) {
    reader.Expect("{\"value\":");
    const int64_t value = reader.Integer();
    reader.Expect(",\"rate\":");
    const int64_t rate = reader.Integer();
    reader.Expect("}");
    if (rate < std::numeric_limits<int32_t>::min() ||
        rate > std::numeric_limits<int32_t>::max()) {
        throw std::runtime_error("RationalTime rate outside int32_t range");
    }
    return {value, static_cast<int32_t>(rate)};
}

std::vector<ExactTimelinePosition> ReadExactPositions(Reader& reader) {
    std::vector<ExactTimelinePosition> positions;
    reader.Expect("[");
    if (reader.Consume("]")) return positions;
    while (true) {
        reader.Expect("{\"clip_id\":");
        const Ulid id = reader.String();
        reader.Expect(",\"timeline_in\":");
        const RationalTime timelineIn = ReadTime(reader);
        reader.Expect("}");
        positions.push_back({id, timelineIn});
        if (reader.Consume("]")) return positions;
        reader.Expect(",");
    }
}

std::vector<ExactTrackState> ReadExactTracks(Reader& reader) {
    std::vector<ExactTrackState> tracks;
    reader.Expect("[");
    if (reader.Consume("]")) return tracks;
    while (true) {
        ExactTrackState state;
        reader.Expect("{\"track_id\":");
        state.track_id = reader.String();
        reader.Expect(",\"clips\":[");
        if (!reader.Consume("]")) {
            while (true) {
                DocumentClip clip;
                reader.Expect("{\"id\":");
                clip.id = reader.String();
                reader.Expect(",\"source_id\":");
                clip.source_id = reader.String();
                reader.Expect(",\"source_in\":");
                clip.source_in = ReadTime(reader);
                reader.Expect(",\"duration\":");
                clip.duration = ReadTime(reader);
                reader.Expect(",\"timeline_in\":");
                clip.timeline_in = ReadTime(reader);
                if (reader.Consume(",\"include_audio\":false"))
                    clip.include_audio = false;
                else
                    reader.Consume(",\"include_audio\":true");
                if (reader.Consume(",\"link_group_id\":"))
                    clip.link_group_id = reader.String();
                if (reader.Consume(",\"sync_anchor_clip_id\":")) {
                    clip.sync_anchor_clip_id = reader.String();
                    reader.Expect(",\"sync_reference_delta\":");
                    clip.sync_reference_delta = ReadTime(reader);
                }
                reader.Expect("}");
                state.clips.push_back(std::move(clip));
                if (reader.Consume("]")) break;
                reader.Expect(",");
            }
        }
        reader.Expect("}");
        tracks.push_back(std::move(state));
        if (reader.Consume("]")) return tracks;
        reader.Expect(",");
    }
}

}  // namespace

const char* EditErrorName(EditError error) {
    switch (error) {
        case EditError::None:
            return "None";
        case EditError::UnknownTrack:
            return "UnknownTrack";
        case EditError::UnknownClip:
            return "UnknownClip";
        case EditError::UnknownSource:
            return "UnknownSource";
        case EditError::UnknownBin:
            return "UnknownBin";
        case EditError::UnknownMarker:
            return "UnknownMarker";
        case EditError::UnknownSequence:
            return "UnknownSequence";
        case EditError::UnknownMedia:
            return "UnknownMedia";
        case EditError::InvalidDuration:
            return "InvalidDuration";
        case EditError::InvalidTimelineIn:
            return "InvalidTimelineIn";
        case EditError::SourceOutOfBounds:
            return "SourceOutOfBounds";
        case EditError::Overlap:
            return "Overlap";
        case EditError::DuplicateId:
            return "DuplicateId";
        case EditError::ArithmeticError:
            return "ArithmeticError";
        case EditError::InvalidOperation:
            return "InvalidOperation";
        case EditError::ValidationFailed:
            return "ValidationFailed";
        case EditError::EmptyUndo:
            return "EmptyUndo";
        case EditError::EmptyRedo:
            return "EmptyRedo";
        case EditError::IoError:
            return "IoError";
        case EditError::ParseError:
            return "ParseError";
    }
    return "InvalidOperation";
}

bool ApplyOperation(Document& document, Operation& operation,
                    Operation& inverse, EditError& error,
                    std::string& message) {
    Document candidate = document;
    Operation normalized = operation;
    Operation generatedInverse = RemoveClipOperation{};
    try {
        bool applied = false;
        if (auto* insert = std::get_if<InsertClipOperation>(&normalized)) {
            applied = ApplyInsert(candidate, *insert, generatedInverse, error,
                                  message);
        } else if (auto* remove =
                       std::get_if<RemoveClipOperation>(&normalized)) {
            applied = ApplyRemove(candidate, *remove, generatedInverse, error,
                                  message);
        } else if (auto* clear =
                       std::get_if<ClearClipOperation>(&normalized)) {
            applied = ApplyClear(candidate, *clear, generatedInverse, error,
                                 message);
        } else if (auto* trim = std::get_if<TrimClipOperation>(&normalized)) {
            applied =
                ApplyTrim(candidate, *trim, generatedInverse, error, message);
        } else if (auto* move = std::get_if<MoveClipOperation>(&normalized)) {
            applied =
                ApplyMove(candidate, *move, generatedInverse, error, message);
        } else if (auto* linkedMove =
                       std::get_if<MoveLinkedClipsOperation>(&normalized)) {
            applied = ApplyMoveLinked(candidate, *linkedMove, generatedInverse,
                                      error, message);
        } else if (auto* linkedTrim =
                       std::get_if<TrimLinkedClipsOperation>(&normalized)) {
            applied = ApplyTrimLinked(candidate, *linkedTrim, generatedInverse,
                                      error, message);
        } else if (auto* linkedRemove =
                       std::get_if<RemoveLinkedClipsOperation>(&normalized)) {
            applied = ApplyRemoveLinked(candidate, *linkedRemove,
                                        generatedInverse, error, message);
        } else if (auto* linkedClear =
                       std::get_if<ClearLinkedClipsOperation>(&normalized)) {
            applied = ApplyClearLinked(candidate, *linkedClear,
                                       generatedInverse, error, message);
        } else if (auto* split = std::get_if<SplitClipOperation>(&normalized)) {
            applied =
                ApplySplit(candidate, *split, generatedInverse, error, message);
        } else if (auto* linkedSplit =
                       std::get_if<SplitLinkedClipsOperation>(&normalized)) {
            applied = ApplySplitLinked(candidate, *linkedSplit,
                                       generatedInverse, error, message);
        } else if (auto* gap = std::get_if<DeleteGapOperation>(&normalized)) {
            applied = ApplyDeleteGap(candidate, *gap, generatedInverse, error,
                                     message);
        } else if (auto* detach =
                       std::get_if<DetachAudioOperation>(&normalized)) {
            applied = ApplyDetachAudio(candidate, *detach, generatedInverse,
                                       error, message);
        } else if (auto* addTrack =
                       std::get_if<AddTrackOperation>(&normalized)) {
            applied = ApplyAddTrack(candidate, *addTrack, generatedInverse,
                                    error, message);
        } else if (auto* removeTrack =
                       std::get_if<RemoveTrackOperation>(&normalized)) {
            applied = ApplyRemoveTrack(candidate, *removeTrack,
                                       generatedInverse, error, message);
        } else if (auto* updateSequence =
                       std::get_if<UpdateSequenceOperation>(&normalized)) {
            applied = ApplyUpdateSequence(candidate, *updateSequence,
                                          generatedInverse, error, message);
        } else if (auto* addBin = std::get_if<AddBinOperation>(&normalized)) {
            applied = ApplyAddBin(candidate, *addBin, generatedInverse, error,
                                  message);
        } else if (auto* removeBin =
                       std::get_if<RemoveBinOperation>(&normalized)) {
            applied = ApplyRemoveBin(candidate, *removeBin, generatedInverse,
                                     error, message);
        } else if (auto* renameBin =
                       std::get_if<RenameBinOperation>(&normalized)) {
            applied = ApplyRenameBin(candidate, *renameBin, generatedInverse,
                                     error, message);
        } else if (auto* moveBin =
                       std::get_if<MoveBinOperation>(&normalized)) {
            applied = ApplyMoveBin(candidate, *moveBin, generatedInverse, error,
                                   message);
        } else if (auto* setMediaBin =
                       std::get_if<SetMediaBinOperation>(&normalized)) {
            applied = ApplySetMediaBin(candidate, *setMediaBin,
                                       generatedInverse, error, message);
        } else if (auto* addMarker =
                       std::get_if<AddMarkerOperation>(&normalized)) {
            applied = ApplyAddMarker(candidate, *addMarker, generatedInverse,
                                     error, message);
        } else if (auto* removeMarker =
                       std::get_if<RemoveMarkerOperation>(&normalized)) {
            applied = ApplyRemoveMarker(candidate, *removeMarker,
                                        generatedInverse, error, message);
        } else if (auto* updateMarker =
                       std::get_if<UpdateMarkerOperation>(&normalized)) {
            applied = ApplyUpdateMarker(candidate, *updateMarker,
                                        generatedInverse, error, message);
        } else if (auto* setClipLink =
                       std::get_if<SetClipLinkOperation>(&normalized)) {
            applied = ApplySetClipLink(candidate, *setClipLink,
                                       generatedInverse, error, message);
        } else if (auto* join = std::get_if<JoinClipOperation>(&normalized)) {
            applied =
                ApplyJoin(candidate, *join, generatedInverse, error, message);
        }
        if (!applied) return false;
    } catch (const std::exception& exception) {
        Fail(EditError::ArithmeticError, exception.what(), error, message);
        return false;
    }
    document = std::move(candidate);
    operation = std::move(normalized);
    inverse = std::move(generatedInverse);
    error = EditError::None;
    message.clear();
    return true;
}

std::string SerializeOperation(const Operation& operation) {
    std::ostringstream output;
    if (const auto* insert = std::get_if<InsertClipOperation>(&operation)) {
        output << "{\"type\":\"InsertClip\",\"track_id\":\"" << insert->track_id
               << "\",\"source_id\":\"" << insert->source_id
               << "\",\"source_in\":";
        WriteTime(output, insert->source_in);
        output << ",\"duration\":";
        WriteTime(output, insert->duration);
        output << ",\"timeline_in\":";
        WriteTime(output, insert->timeline_in);
        output << ",\"clip_id\":\"" << insert->clip_id
               << "\",\"exact_timeline\":";
        WriteExactPositions(output, insert->exact_timeline_result);
        output << '}';
    } else if (const auto* remove =
                   std::get_if<RemoveClipOperation>(&operation)) {
        output << "{\"type\":\"RemoveClip\",\"clip_id\":\"" << remove->clip_id
               << "\",\"exact_timeline\":";
        WriteExactPositions(output, remove->exact_timeline_result);
        output << '}';
    } else if (const auto* clear =
                   std::get_if<ClearClipOperation>(&operation)) {
        output << "{\"type\":\"ClearClip\",\"clip_id\":";
        WriteString(output, clear->clip_id);
        output << ",\"exact_tracks\":";
        WriteExactTracks(output, clear->exact_track_result);
        output << '}';
    } else if (const auto* trim = std::get_if<TrimClipOperation>(&operation)) {
        output << "{\"type\":\"TrimClip\",\"clip_id\":\"" << trim->clip_id
               << "\",\"edge\":\""
               << (trim->edge == TrimEdge::Head ? "Head" : "Tail")
               << "\",\"delta\":";
        WriteTime(output, trim->delta);
        output << ",\"exact_clip\":";
        if (!trim->exact_clip_result) {
            output << "null";
        } else {
            output << "{\"source_in\":";
            WriteTime(output, trim->exact_clip_result->source_in);
            output << ",\"duration\":";
            WriteTime(output, trim->exact_clip_result->duration);
            output << ",\"timeline_in\":";
            WriteTime(output, trim->exact_clip_result->timeline_in);
            output << '}';
        }
        output << '}';
    } else if (const auto* move = std::get_if<MoveClipOperation>(&operation)) {
        output << "{\"type\":\"MoveClip\",\"clip_id\":\"" << move->clip_id
               << "\",\"track_id\":\"" << move->track_id
               << "\",\"timeline_in\":";
        WriteTime(output, move->timeline_in);
        output << ",\"exact_tracks\":";
        WriteExactTracks(output, move->exact_track_result);
        output << '}';
    } else if (const auto* linkedMove =
                   std::get_if<MoveLinkedClipsOperation>(&operation)) {
        output << "{\"type\":\"MoveLinkedClips\",\"link_group_id\":\""
               << linkedMove->link_group_id << "\",\"moves\":[";
        for (size_t index = 0; index < linkedMove->moves.size(); ++index) {
            if (index) output << ',';
            const LinkedClipMove& move = linkedMove->moves[index];
            output << "{\"clip_id\":\"" << move.clip_id << "\",\"track_id\":\""
                   << move.track_id << "\",\"timeline_in\":";
            WriteTime(output, move.timeline_in);
            output << '}';
        }
        output << "],\"exact_tracks\":";
        WriteExactTracks(output, linkedMove->exact_track_result);
        output << '}';
    } else if (const auto* linkedTrim =
                   std::get_if<TrimLinkedClipsOperation>(&operation)) {
        output << "{\"type\":\"TrimLinkedClips\",\"link_group_id\":\""
               << linkedTrim->link_group_id << "\",\"trims\":[";
        for (size_t index = 0; index < linkedTrim->trims.size(); ++index) {
            if (index) output << ',';
            const LinkedClipTrim& trim = linkedTrim->trims[index];
            output << "{\"clip_id\":\"" << trim.clip_id << "\",\"edge\":\""
                   << (trim.edge == TrimEdge::Head ? "Head" : "Tail")
                   << "\",\"delta\":";
            WriteTime(output, trim.delta);
            output << '}';
        }
        output << "],\"exact_tracks\":";
        WriteExactTracks(output, linkedTrim->exact_track_result);
        output << '}';
    } else if (const auto* linkedRemove =
                   std::get_if<RemoveLinkedClipsOperation>(&operation)) {
        output << "{\"type\":\"RemoveLinkedClips\",\"link_group_id\":\""
               << linkedRemove->link_group_id << "\",\"clip_ids\":[";
        for (size_t index = 0; index < linkedRemove->clip_ids.size(); ++index) {
            if (index) output << ',';
            WriteString(output, linkedRemove->clip_ids[index]);
        }
        output << "],\"exact_tracks\":";
        WriteExactTracks(output, linkedRemove->exact_track_result);
        output << '}';
    } else if (const auto* linkedClear =
                   std::get_if<ClearLinkedClipsOperation>(&operation)) {
        output << "{\"type\":\"ClearLinkedClips\",\"link_group_id\":";
        WriteString(output, linkedClear->link_group_id);
        output << ",\"clip_ids\":[";
        for (size_t index = 0; index < linkedClear->clip_ids.size(); ++index) {
            if (index) output << ',';
            WriteString(output, linkedClear->clip_ids[index]);
        }
        output << "],\"exact_tracks\":";
        WriteExactTracks(output, linkedClear->exact_track_result);
        output << '}';
    } else if (const auto* split =
                   std::get_if<SplitClipOperation>(&operation)) {
        output << "{\"type\":\"SplitClip\",\"clip_id\":\"" << split->clip_id
               << "\",\"timeline_position\":";
        WriteTime(output, split->timeline_position);
        output << ",\"right_clip_id\":\"" << split->right_clip_id << "\"}";
    } else if (const auto* split =
                   std::get_if<SplitLinkedClipsOperation>(&operation)) {
        output << "{\"type\":\"SplitLinkedClips\",\"link_group_id\":";
        WriteString(output, split->link_group_id);
        output << ",\"clip_ids\":[";
        for (size_t index = 0; index < split->clip_ids.size(); ++index) {
            if (index) output << ',';
            WriteString(output, split->clip_ids[index]);
        }
        output << "],\"timeline_position\":";
        WriteTime(output, split->timeline_position);
        output << ",\"left_group_id\":";
        WriteString(output, split->left_group_id);
        output << ",\"right_group_id\":";
        WriteString(output, split->right_group_id);
        output << ",\"right_clip_ids\":[";
        for (size_t index = 0; index < split->right_clip_ids.size(); ++index) {
            if (index) output << ',';
            WriteString(output, split->right_clip_ids[index]);
        }
        output << "],\"exact_tracks\":";
        WriteExactTracks(output, split->exact_track_result);
        output << '}';
    } else if (const auto* gap = std::get_if<DeleteGapOperation>(&operation)) {
        output << "{\"type\":\"DeleteGap\",\"track_id\":\"" << gap->track_id
               << "\",\"gap_start\":";
        WriteTime(output, gap->gap_start);
        output << ",\"gap_duration\":";
        WriteTime(output, gap->gap_duration);
        if (!gap->linked_track_ids.empty()) {
            output << ",\"linked_track_ids\":[";
            for (size_t index = 0; index < gap->linked_track_ids.size(); ++index) {
                if (index) output << ',';
                WriteString(output, gap->linked_track_ids[index]);
            }
            output << ']';
        }
        output << ",\"exact_tracks\":";
        WriteExactTracks(output, gap->exact_track_result);
        output << '}';
    } else if (const auto* detach =
                   std::get_if<DetachAudioOperation>(&operation)) {
        output << "{\"type\":\"DetachAudio\",\"video_clip_id\":\""
               << detach->video_clip_id << "\",\"audio_track_id\":\""
               << detach->audio_track_id << "\",\"audio_clip_id\":\""
               << detach->audio_clip_id << "\",\"exact_tracks\":";
        WriteExactTracks(output, detach->exact_track_result);
        output << '}';
    } else if (const auto* addTrack =
                   std::get_if<AddTrackOperation>(&operation)) {
        output << "{\"type\":\"AddTrack\",\"track_id\":\"" << addTrack->track_id
               << "\",\"kind\":\"" << addTrack->kind
               << "\",\"index\":" << addTrack->index;
        if (!addTrack->clips.empty()) {
            output << ",\"exact_tracks\":";
            WriteExactTracks(output,
                             {{addTrack->track_id, addTrack->clips}});
        }
        output << '}';
    } else if (const auto* removeTrack =
                   std::get_if<RemoveTrackOperation>(&operation)) {
        output << "{\"type\":\"RemoveTrack\",\"track_id\":\""
               << removeTrack->track_id << "\"}";
    } else if (const auto* updateSequence =
                   std::get_if<UpdateSequenceOperation>(&operation)) {
        output << "{\"type\":\"UpdateSequence\",\"sequence_id\":\""
               << updateSequence->sequence_id << "\",\"name\":";
        WriteString(output, updateSequence->name);
        output << ",\"width\":" << updateSequence->width
               << ",\"height\":" << updateSequence->height
               << ",\"frame_rate\":{\"num\":"
               << updateSequence->frame_rate.num << ",\"den\":"
               << updateSequence->frame_rate.den << "}}";
    } else if (const auto* addBin = std::get_if<AddBinOperation>(&operation)) {
        output << "{\"type\":\"AddBin\",\"bin_id\":\"" << addBin->bin_id
               << "\",\"name\":";
        WriteString(output, addBin->name);
        output << ",\"parent_id\":\"" << addBin->parent_id << "\"";
        output << '}';
    } else if (const auto* removeBin =
                   std::get_if<RemoveBinOperation>(&operation)) {
        output << "{\"type\":\"RemoveBin\",\"bin_id\":\"" << removeBin->bin_id
               << "\",\"name\":";
        WriteString(output, removeBin->name);
        output << ",\"parent_id\":\"" << removeBin->parent_id << "\"";
        output << '}';
    } else if (const auto* renameBin =
                   std::get_if<RenameBinOperation>(&operation)) {
        output << "{\"type\":\"RenameBin\",\"bin_id\":\"" << renameBin->bin_id
               << "\",\"name\":";
        WriteString(output, renameBin->name);
        output << '}';
    } else if (const auto* moveBin =
                   std::get_if<MoveBinOperation>(&operation)) {
        output << "{\"type\":\"MoveBin\",\"bin_id\":\"" << moveBin->bin_id
               << "\",\"parent_id\":\"" << moveBin->parent_id << "\"}";
    } else if (const auto* setMediaBin =
                   std::get_if<SetMediaBinOperation>(&operation)) {
        output << "{\"type\":\"SetMediaBin\",\"media_id\":\""
               << setMediaBin->media_id << "\",\"bin_id\":\""
               << setMediaBin->bin_id << "\"}";
    } else if (const auto* addMarker =
                   std::get_if<AddMarkerOperation>(&operation)) {
        output << "{\"type\":\"AddMarker\",\"marker\":{\"id\":\""
               << addMarker->marker.id << "\",\"name\":";
        WriteString(output, addMarker->marker.name);
        output << ",\"time\":";
        WriteTime(output, addMarker->marker.time);
        output << ",\"color\":";
        WriteString(output, addMarker->marker.color);
        output << ",\"category\":";
        WriteString(output, addMarker->marker.category);
        output << "},\"insertion_index\":" << addMarker->insertion_index
               << '}';
    } else if (const auto* removeMarker =
                   std::get_if<RemoveMarkerOperation>(&operation)) {
        output << "{\"type\":\"RemoveMarker\",\"marker_id\":\""
               << removeMarker->marker_id << "\"}";
    } else if (const auto* updateMarker =
                   std::get_if<UpdateMarkerOperation>(&operation)) {
        output << "{\"type\":\"UpdateMarker\",\"marker_id\":\""
               << updateMarker->marker_id << "\",\"name\":";
        WriteString(output, updateMarker->name);
        output << ",\"time\":";
        WriteTime(output, updateMarker->time);
        output << ",\"color\":";
        WriteString(output, updateMarker->color);
        output << ",\"category\":";
        WriteString(output, updateMarker->category);
        output << '}';
    } else if (const auto* setClipLink =
                   std::get_if<SetClipLinkOperation>(&operation)) {
        output << "{\"type\":\"SetClipLink\",\"first_clip_id\":\""
               << setClipLink->first_clip_id << "\",\"second_clip_id\":\""
               << setClipLink->second_clip_id << "\",\"first_group_id\":\""
               << setClipLink->first_group_id << "\",\"second_group_id\":\""
               << setClipLink->second_group_id << "\",\"exact_first\":";
        const auto writeState = [&](const auto& state) {
            if (!state) {
                output << "null";
                return;
            }
            output << "{\"group_id\":\"" << state->group_id
                   << "\",\"anchor_clip_id\":\"" << state->anchor_clip_id
                   << "\",\"reference_delta\":";
            WriteTime(output, state->reference_delta);
            output << '}';
        };
        writeState(setClipLink->exact_first_result);
        output << ",\"exact_second\":";
        writeState(setClipLink->exact_second_result);
        output << '}';
    } else {
        const auto& join = std::get<JoinClipOperation>(operation);
        output << "{\"type\":\"JoinClip\",\"left_clip_id\":\""
               << join.left_clip_id << "\",\"right_clip_id\":\""
               << join.right_clip_id << "\",\"joined_times\":{\"source_in\":";
        WriteTime(output, join.joined_times.source_in);
        output << ",\"duration\":";
        WriteTime(output, join.joined_times.duration);
        output << ",\"timeline_in\":";
        WriteTime(output, join.joined_times.timeline_in);
        output << "}}";
    }
    return output.str();
}

bool DeserializeOperation(const std::string& json, Operation& operation,
                          EditError& error, std::string& message) {
    try {
        Reader reader(json);
        reader.Expect("{\"type\":");
        const std::string type = reader.String();
        if (type == "InsertClip") {
            reader.Expect(",\"track_id\":");
            InsertClipOperation value;
            value.track_id = reader.String();
            reader.Expect(",\"source_id\":");
            value.source_id = reader.String();
            reader.Expect(",\"source_in\":");
            value.source_in = ReadTime(reader);
            reader.Expect(",\"duration\":");
            value.duration = ReadTime(reader);
            reader.Expect(",\"timeline_in\":");
            value.timeline_in = ReadTime(reader);
            reader.Expect(",\"clip_id\":");
            value.clip_id = reader.String();
            reader.Expect(",\"exact_timeline\":");
            value.exact_timeline_result = ReadExactPositions(reader);
            reader.Expect("}");
            operation = std::move(value);
        } else if (type == "RemoveClip") {
            reader.Expect(",\"clip_id\":");
            RemoveClipOperation value;
            value.clip_id = reader.String();
            reader.Expect(",\"exact_timeline\":");
            value.exact_timeline_result = ReadExactPositions(reader);
            reader.Expect("}");
            operation = std::move(value);
        } else if (type == "ClearClip") {
            ClearClipOperation value;
            reader.Expect(",\"clip_id\":");
            value.clip_id = reader.String();
            reader.Expect(",\"exact_tracks\":");
            value.exact_track_result = ReadExactTracks(reader);
            reader.Expect("}");
            operation = std::move(value);
        } else if (type == "TrimClip") {
            reader.Expect(",\"clip_id\":");
            TrimClipOperation value;
            value.clip_id = reader.String();
            reader.Expect(",\"edge\":");
            const std::string edge = reader.String();
            if (edge == "Head")
                value.edge = TrimEdge::Head;
            else if (edge == "Tail")
                value.edge = TrimEdge::Tail;
            else
                throw std::runtime_error("unknown trim edge '" + edge + "'");
            reader.Expect(",\"delta\":");
            value.delta = ReadTime(reader);
            reader.Expect(",\"exact_clip\":");
            if (!reader.Consume("null")) {
                reader.Expect("{\"source_in\":");
                ExactClipTimes exact;
                exact.source_in = ReadTime(reader);
                reader.Expect(",\"duration\":");
                exact.duration = ReadTime(reader);
                reader.Expect(",\"timeline_in\":");
                exact.timeline_in = ReadTime(reader);
                reader.Expect("}");
                value.exact_clip_result = exact;
            }
            reader.Expect("}");
            operation = std::move(value);
        } else if (type == "MoveClip") {
            reader.Expect(",\"clip_id\":");
            MoveClipOperation value;
            value.clip_id = reader.String();
            reader.Expect(",\"track_id\":");
            value.track_id = reader.String();
            reader.Expect(",\"timeline_in\":");
            value.timeline_in = ReadTime(reader);
            if (!reader.Consume("}")) {
                reader.Expect(",\"exact_tracks\":");
                value.exact_track_result = ReadExactTracks(reader);
                reader.Expect("}");
            }
            operation = std::move(value);
        } else if (type == "MoveLinkedClips") {
            reader.Expect(",\"link_group_id\":");
            MoveLinkedClipsOperation value;
            value.link_group_id = reader.String();
            reader.Expect(",\"moves\":[");
            if (!reader.Consume("]")) {
                while (true) {
                    LinkedClipMove move;
                    reader.Expect("{\"clip_id\":");
                    move.clip_id = reader.String();
                    reader.Expect(",\"track_id\":");
                    move.track_id = reader.String();
                    reader.Expect(",\"timeline_in\":");
                    move.timeline_in = ReadTime(reader);
                    reader.Expect("}");
                    value.moves.push_back(std::move(move));
                    if (reader.Consume("]")) break;
                    reader.Expect(",");
                }
            }
            reader.Expect(",\"exact_tracks\":");
            value.exact_track_result = ReadExactTracks(reader);
            reader.Expect("}");
            operation = std::move(value);
        } else if (type == "TrimLinkedClips") {
            reader.Expect(",\"link_group_id\":");
            TrimLinkedClipsOperation value;
            value.link_group_id = reader.String();
            reader.Expect(",\"trims\":[");
            if (!reader.Consume("]")) {
                while (true) {
                    LinkedClipTrim trim;
                    reader.Expect("{\"clip_id\":");
                    trim.clip_id = reader.String();
                    reader.Expect(",\"edge\":");
                    const std::string edge = reader.String();
                    if (edge != "Head" && edge != "Tail")
                        throw std::runtime_error("invalid linked trim edge");
                    trim.edge =
                        edge == "Head" ? TrimEdge::Head : TrimEdge::Tail;
                    reader.Expect(",\"delta\":");
                    trim.delta = ReadTime(reader);
                    reader.Expect("}");
                    value.trims.push_back(std::move(trim));
                    if (reader.Consume("]")) break;
                    reader.Expect(",");
                }
            }
            reader.Expect(",\"exact_tracks\":");
            value.exact_track_result = ReadExactTracks(reader);
            reader.Expect("}");
            operation = std::move(value);
        } else if (type == "RemoveLinkedClips") {
            reader.Expect(",\"link_group_id\":");
            RemoveLinkedClipsOperation value;
            value.link_group_id = reader.String();
            reader.Expect(",\"clip_ids\":[");
            if (!reader.Consume("]")) {
                while (true) {
                    value.clip_ids.push_back(reader.String());
                    if (reader.Consume("]")) break;
                    reader.Expect(",");
                }
            }
            reader.Expect(",\"exact_tracks\":");
            value.exact_track_result = ReadExactTracks(reader);
            reader.Expect("}");
            operation = std::move(value);
        } else if (type == "ClearLinkedClips") {
            ClearLinkedClipsOperation value;
            reader.Expect(",\"link_group_id\":");
            value.link_group_id = reader.String();
            reader.Expect(",\"clip_ids\":[");
            if (!reader.Consume("]")) {
                while (true) {
                    value.clip_ids.push_back(reader.String());
                    if (reader.Consume("]")) break;
                    reader.Expect(",");
                }
            }
            reader.Expect(",\"exact_tracks\":");
            value.exact_track_result = ReadExactTracks(reader);
            reader.Expect("}");
            operation = std::move(value);
        } else if (type == "SplitClip") {
            reader.Expect(",\"clip_id\":");
            SplitClipOperation value;
            value.clip_id = reader.String();
            reader.Expect(",\"timeline_position\":");
            value.timeline_position = ReadTime(reader);
            reader.Expect(",\"right_clip_id\":");
            value.right_clip_id = reader.String();
            reader.Expect("}");
            operation = std::move(value);
        } else if (type == "SplitLinkedClips") {
            SplitLinkedClipsOperation value;
            reader.Expect(",\"link_group_id\":");
            value.link_group_id = reader.String();
            reader.Expect(",\"clip_ids\":[");
            if (!reader.Consume("]")) {
                while (true) {
                    value.clip_ids.push_back(reader.String());
                    if (reader.Consume("]")) break;
                    reader.Expect(",");
                }
            }
            reader.Expect(",\"timeline_position\":");
            value.timeline_position = ReadTime(reader);
            reader.Expect(",\"left_group_id\":");
            value.left_group_id = reader.String();
            reader.Expect(",\"right_group_id\":");
            value.right_group_id = reader.String();
            reader.Expect(",\"right_clip_ids\":[");
            if (!reader.Consume("]")) {
                while (true) {
                    value.right_clip_ids.push_back(reader.String());
                    if (reader.Consume("]")) break;
                    reader.Expect(",");
                }
            }
            reader.Expect(",\"exact_tracks\":");
            value.exact_track_result = ReadExactTracks(reader);
            reader.Expect("}");
            operation = std::move(value);
        } else if (type == "DeleteGap") {
            reader.Expect(",\"track_id\":");
            DeleteGapOperation value;
            value.track_id = reader.String();
            reader.Expect(",\"gap_start\":");
            value.gap_start = ReadTime(reader);
            reader.Expect(",\"gap_duration\":");
            value.gap_duration = ReadTime(reader);
            if (reader.Consume(",\"linked_track_ids\":[")) {
                if (!reader.Consume("]")) {
                    while (true) {
                        value.linked_track_ids.push_back(reader.String());
                        if (reader.Consume("]")) break;
                        reader.Expect(",");
                    }
                }
            }
            reader.Expect(",\"exact_tracks\":");
            value.exact_track_result = ReadExactTracks(reader);
            reader.Expect("}");
            operation = std::move(value);
        } else if (type == "DetachAudio") {
            reader.Expect(",\"video_clip_id\":");
            DetachAudioOperation value;
            value.video_clip_id = reader.String();
            reader.Expect(",\"audio_track_id\":");
            value.audio_track_id = reader.String();
            reader.Expect(",\"audio_clip_id\":");
            value.audio_clip_id = reader.String();
            reader.Expect(",\"exact_tracks\":");
            value.exact_track_result = ReadExactTracks(reader);
            reader.Expect("}");
            operation = std::move(value);
        } else if (type == "AddTrack") {
            reader.Expect(",\"track_id\":");
            AddTrackOperation value;
            value.track_id = reader.String();
            reader.Expect(",\"kind\":");
            value.kind = reader.String();
            reader.Expect(",\"index\":");
            const int64_t index = reader.Integer();
            if (index < std::numeric_limits<int32_t>::min() ||
                index > std::numeric_limits<int32_t>::max())
                throw std::runtime_error("track index outside int32_t range");
            value.index = static_cast<int32_t>(index);
            if (!reader.Consume("}")) {
                reader.Expect(",\"exact_tracks\":");
                const auto tracks = ReadExactTracks(reader);
                if (tracks.size() != 1 || tracks.front().track_id != value.track_id)
                    throw std::runtime_error(
                        "AddTrack exact state must match track_id");
                value.clips = tracks.front().clips;
                reader.Expect("}");
            }
            operation = std::move(value);
        } else if (type == "RemoveTrack") {
            reader.Expect(",\"track_id\":");
            RemoveTrackOperation value;
            value.track_id = reader.String();
            reader.Expect("}");
            operation = std::move(value);
        } else if (type == "UpdateSequence") {
            UpdateSequenceOperation value;
            reader.Expect(",\"sequence_id\":");
            value.sequence_id = reader.String();
            reader.Expect(",\"name\":");
            value.name = reader.String();
            reader.Expect(",\"width\":");
            const int64_t width = reader.Integer();
            reader.Expect(",\"height\":");
            const int64_t height = reader.Integer();
            if (width < std::numeric_limits<int32_t>::min() ||
                width > std::numeric_limits<int32_t>::max() ||
                height < std::numeric_limits<int32_t>::min() ||
                height > std::numeric_limits<int32_t>::max())
                throw std::runtime_error(
                    "sequence dimensions outside int32_t range");
            value.width = static_cast<int32_t>(width);
            value.height = static_cast<int32_t>(height);
            reader.Expect(",\"frame_rate\":{\"num\":");
            const int64_t rateNum = reader.Integer();
            reader.Expect(",\"den\":");
            const int64_t rateDen = reader.Integer();
            if (rateNum < std::numeric_limits<int32_t>::min() ||
                rateNum > std::numeric_limits<int32_t>::max() ||
                rateDen < std::numeric_limits<int32_t>::min() ||
                rateDen > std::numeric_limits<int32_t>::max())
                throw std::runtime_error(
                    "sequence frame rate outside int32_t range");
            value.frame_rate = {static_cast<int32_t>(rateNum),
                                static_cast<int32_t>(rateDen)};
            reader.Expect("}}");
            operation = std::move(value);
        } else if (type == "AddBin") {
            reader.Expect(",\"bin_id\":");
            AddBinOperation value;
            value.bin_id = reader.String();
            reader.Expect(",\"name\":");
            value.name = reader.String();
            if (!reader.Consume("}")) {
                reader.Expect(",\"parent_id\":");
                value.parent_id = reader.String();
                reader.Expect("}");
            }
            operation = std::move(value);
        } else if (type == "RemoveBin") {
            reader.Expect(",\"bin_id\":");
            RemoveBinOperation value;
            value.bin_id = reader.String();
            reader.Expect(",\"name\":");
            value.name = reader.String();
            if (!reader.Consume("}")) {
                reader.Expect(",\"parent_id\":");
                value.parent_id = reader.String();
                reader.Expect("}");
            }
            operation = std::move(value);
        } else if (type == "RenameBin") {
            reader.Expect(",\"bin_id\":");
            RenameBinOperation value;
            value.bin_id = reader.String();
            reader.Expect(",\"name\":");
            value.name = reader.String();
            reader.Expect("}");
            operation = std::move(value);
        } else if (type == "MoveBin") {
            reader.Expect(",\"bin_id\":");
            MoveBinOperation value;
            value.bin_id = reader.String();
            reader.Expect(",\"parent_id\":");
            value.parent_id = reader.String();
            reader.Expect("}");
            operation = std::move(value);
        } else if (type == "SetMediaBin") {
            reader.Expect(",\"media_id\":");
            SetMediaBinOperation value;
            value.media_id = reader.String();
            reader.Expect(",\"bin_id\":");
            value.bin_id = reader.String();
            reader.Expect("}");
            operation = std::move(value);
        } else if (type == "AddMarker") {
            AddMarkerOperation value;
            reader.Expect(",\"marker\":{\"id\":");
            value.marker.id = reader.String();
            reader.Expect(",\"name\":");
            value.marker.name = reader.String();
            reader.Expect(",\"time\":");
            value.marker.time = ReadTime(reader);
            reader.Expect(",\"color\":");
            value.marker.color = reader.String();
            reader.Expect(",\"category\":");
            value.marker.category = reader.String();
            reader.Expect("},\"insertion_index\":");
            value.insertion_index = reader.Integer();
            reader.Expect("}");
            operation = std::move(value);
        } else if (type == "RemoveMarker") {
            RemoveMarkerOperation value;
            reader.Expect(",\"marker_id\":");
            value.marker_id = reader.String();
            reader.Expect("}");
            operation = std::move(value);
        } else if (type == "UpdateMarker") {
            UpdateMarkerOperation value;
            reader.Expect(",\"marker_id\":");
            value.marker_id = reader.String();
            reader.Expect(",\"name\":");
            value.name = reader.String();
            reader.Expect(",\"time\":");
            value.time = ReadTime(reader);
            reader.Expect(",\"color\":");
            value.color = reader.String();
            reader.Expect(",\"category\":");
            value.category = reader.String();
            reader.Expect("}");
            operation = std::move(value);
        } else if (type == "SetClipLink") {
            reader.Expect(",\"first_clip_id\":");
            SetClipLinkOperation value;
            value.first_clip_id = reader.String();
            reader.Expect(",\"second_clip_id\":");
            value.second_clip_id = reader.String();
            reader.Expect(",\"first_group_id\":");
            value.first_group_id = reader.String();
            reader.Expect(",\"second_group_id\":");
            value.second_group_id = reader.String();
            if (!reader.Consume("}")) {
                const auto readState =
                    [&]() -> std::optional<SetClipLinkOperation::ExactState> {
                    if (reader.Consume("null")) return std::nullopt;
                    SetClipLinkOperation::ExactState state;
                    reader.Expect("{\"group_id\":");
                    state.group_id = reader.String();
                    reader.Expect(",\"anchor_clip_id\":");
                    state.anchor_clip_id = reader.String();
                    reader.Expect(",\"reference_delta\":");
                    state.reference_delta = ReadTime(reader);
                    reader.Expect("}");
                    return state;
                };
                reader.Expect(",\"exact_first\":");
                value.exact_first_result = readState();
                reader.Expect(",\"exact_second\":");
                value.exact_second_result = readState();
                reader.Expect("}");
            }
            operation = std::move(value);
        } else if (type == "JoinClip") {
            reader.Expect(",\"left_clip_id\":");
            JoinClipOperation value;
            value.left_clip_id = reader.String();
            reader.Expect(",\"right_clip_id\":");
            value.right_clip_id = reader.String();
            reader.Expect(",\"joined_times\":{\"source_in\":");
            value.joined_times.source_in = ReadTime(reader);
            reader.Expect(",\"duration\":");
            value.joined_times.duration = ReadTime(reader);
            reader.Expect(",\"timeline_in\":");
            value.joined_times.timeline_in = ReadTime(reader);
            reader.Expect("}}");
            operation = std::move(value);
        } else {
            throw std::runtime_error("unknown operation type '" + type + "'");
        }
        reader.Finish();
        error = EditError::None;
        message.clear();
        return true;
    } catch (const std::exception& exception) {
        error = EditError::ParseError;
        message = exception.what();
        return false;
    }
}
