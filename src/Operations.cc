#include "Operations.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <type_traits>
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

const DocumentTrack* LockedTrackTouchedBy(const Document& document,
                                          const Operation& operation) {
    std::vector<Ulid> trackIds;
    const auto addTrack = [&](const Ulid& id) {
        if (!id.empty() &&
            std::find(trackIds.begin(), trackIds.end(), id) == trackIds.end())
            trackIds.push_back(id);
    };
    const auto addClipTrack = [&](const Ulid& clipId) {
        if (const DocumentTrack* track = document.FindTrackForClip(clipId))
            addTrack(track->id);
    };
    const auto addExactTracks =
        [&](const std::vector<ExactTrackState>& states) {
            for (const ExactTrackState& state : states)
                addTrack(state.track_id);
        };
    const auto addExactPositions =
        [&](const std::vector<ExactTimelinePosition>& positions) {
            for (const ExactTimelinePosition& position : positions)
                addClipTrack(position.clip_id);
        };
    std::visit(
        [&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, InsertClipOperation>) {
                addTrack(value.track_id);
                addExactPositions(value.exact_timeline_result);
            } else if constexpr (std::is_same_v<T, RemoveClipOperation> ||
                                 std::is_same_v<T, TrimClipOperation> ||
                                 std::is_same_v<T, SplitClipOperation>) {
                addClipTrack(value.clip_id);
                if constexpr (std::is_same_v<T, RemoveClipOperation>)
                    addExactPositions(value.exact_timeline_result);
            } else if constexpr (std::is_same_v<T, ClearClipOperation>) {
                addClipTrack(value.clip_id);
                addExactTracks(value.exact_track_result);
            } else if constexpr (std::is_same_v<T, ClearClipsOperation>) {
                for (const Ulid& clipId : value.clip_ids) addClipTrack(clipId);
                addExactTracks(value.exact_track_result);
            } else if constexpr (std::is_same_v<T, PasteClipsOperation>) {
                for (const PastedClip& clip : value.clips)
                    addTrack(clip.track_id);
                addExactTracks(value.exact_track_result);
            } else if constexpr (std::is_same_v<T, MoveClipOperation>) {
                addClipTrack(value.clip_id);
                addTrack(value.track_id);
                addExactTracks(value.exact_track_result);
            } else if constexpr (std::is_same_v<T, MoveClipsOperation>) {
                for (const LinkedClipMove& move : value.moves) {
                    addClipTrack(move.clip_id);
                    addTrack(move.track_id);
                }
                addExactTracks(value.exact_track_result);
            } else if constexpr (std::is_same_v<T, MoveLinkedClipsOperation>) {
                for (const LinkedClipMove& move : value.moves) {
                    addClipTrack(move.clip_id);
                    addTrack(move.track_id);
                }
                addExactTracks(value.exact_track_result);
            } else if constexpr (std::is_same_v<T, TrimLinkedClipsOperation>) {
                for (const LinkedClipTrim& trim : value.trims)
                    addClipTrack(trim.clip_id);
                addExactTracks(value.exact_track_result);
            } else if constexpr (std::is_same_v<T, RippleTrimOperation>) {
                addClipTrack(value.clip_id);
                for (const Ulid& clipId : value.linked_clip_ids)
                    addClipTrack(clipId);
                for (const Ulid& trackId : value.sync_track_ids)
                    addTrack(trackId);
                addExactTracks(value.exact_track_result);
            } else if constexpr (std::is_same_v<T, RollEditOperation>) {
                for (const RollEditPair& pair : value.pairs) {
                    addClipTrack(pair.left_clip_id);
                    addClipTrack(pair.right_clip_id);
                }
                addExactTracks(value.exact_track_result);
            } else if constexpr (std::is_same_v<T, SlipEditOperation>) {
                for (const Ulid& clipId : value.clip_ids) addClipTrack(clipId);
                addExactTracks(value.exact_track_result);
            } else if constexpr (std::is_same_v<T,
                                                RemoveLinkedClipsOperation> ||
                                 std::is_same_v<T, ClearLinkedClipsOperation>) {
                for (const Ulid& clipId : value.clip_ids) addClipTrack(clipId);
                addExactTracks(value.exact_track_result);
            } else if constexpr (std::is_same_v<T, DeleteGapOperation>) {
                addTrack(value.track_id);
                for (const Ulid& trackId : value.linked_track_ids)
                    addTrack(trackId);
                addExactTracks(value.exact_track_result);
            } else if constexpr (std::is_same_v<T, DetachAudioOperation>) {
                addClipTrack(value.video_clip_id);
                addTrack(value.audio_track_id);
                addExactTracks(value.exact_track_result);
            } else if constexpr (std::is_same_v<T, RemoveTrackOperation>) {
                addTrack(value.track_id);
            } else if constexpr (std::is_same_v<T, SetClipLinkOperation>) {
                addClipTrack(value.first_clip_id);
                addClipTrack(value.second_clip_id);
            } else if constexpr (std::is_same_v<T, AddTransitionOperation>) {
                addTrack(value.transition.track_id);
            } else if constexpr (std::is_same_v<T, RemoveTransitionOperation> ||
                                 std::is_same_v<T, UpdateTransitionOperation>) {
                if (const DocumentTransition* transition =
                        document.FindTransition(value.transition_id))
                    addTrack(transition->track_id);
            } else if constexpr (std::is_same_v<T, SplitLinkedClipsOperation>) {
                for (const Ulid& clipId : value.clip_ids) addClipTrack(clipId);
                addExactTracks(value.exact_track_result);
            } else if constexpr (std::is_same_v<T, JoinClipOperation>) {
                addClipTrack(value.left_clip_id);
                addClipTrack(value.right_clip_id);
            } else if constexpr (std::is_same_v<T, SetClipEffectsOperation> ||
                                 std::is_same_v<T, SetClipOpacityOperation> ||
                                 std::is_same_v<T, SetClipCaptionOperation>) {
                addClipTrack(value.clip_id);
            } else if constexpr (std::is_same_v<
                                     T, SplitClipAtPositionsOperation>) {
                addClipTrack(value.clip_id);
                addExactTracks(value.exact_track_result);
            } else if constexpr (std::is_same_v<T, RemoveWordsOperation>) {
                addClipTrack(value.clip_id);
                for (const Ulid& trackId : value.sync_track_ids)
                    addTrack(trackId);
                addExactTracks(value.exact_track_result);
            }
        },
        operation);
    for (const Ulid& trackId : trackIds) {
        const DocumentTrack* track = document.FindTrack(trackId);
        if (track && track->locked) return track;
    }
    return nullptr;
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
    DocumentClip inserted{operation.clip_id, operation.source_id,
                          operation.source_in, operation.duration,
                          operation.timeline_in};
    inserted.include_audio = false;
    track->clips.insert(
        track->clips.begin() + static_cast<std::ptrdiff_t>(insertionIndex),
        std::move(inserted));
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
    track->clips.erase(std::remove_if(track->clips.begin(), track->clips.end(),
                                      [&](const DocumentClip& clip) {
                                          return clip.id == operation.clip_id;
                                      }),
                       track->clips.end());
    if (!ValidateResult(candidate, error, message)) return false;
    operation.exact_track_result = {{track->id, track->clips}};
    inverse = ClearClipOperation{operation.clip_id, {before}};
    return true;
}

bool ApplyClearClips(Document& candidate, ClearClipsOperation& operation,
                     Operation& inverse, EditError& error,
                     std::string& message) {
    const auto snapshots = [&](const std::vector<Ulid>& trackIds) {
        std::vector<ExactTrackState> result;
        for (const Ulid& trackId : trackIds) {
            if (std::any_of(result.begin(), result.end(),
                            [&](const ExactTrackState& state) {
                                return state.track_id == trackId;
                            }))
                continue;
            const DocumentTrack* track = candidate.FindTrack(trackId);
            if (track) result.push_back({trackId, track->clips});
        }
        return result;
    };
    if (!operation.exact_track_result.empty()) {
        std::vector<Ulid> trackIds;
        for (const ExactTrackState& state : operation.exact_track_result)
            trackIds.push_back(state.track_id);
        const std::vector<ExactTrackState> before = snapshots(trackIds);
        if (before.size() != operation.exact_track_result.size()) {
            Fail(EditError::UnknownTrack,
                 "exact multi-clear references an unknown track", error,
                 message);
            return false;
        }
        for (const ExactTrackState& state : operation.exact_track_result)
            candidate.FindTrack(state.track_id)->clips = state.clips;
        if (!ValidateResult(candidate, error, message)) return false;
        inverse = ClearClipsOperation{operation.clip_ids, before};
        return true;
    }
    if (operation.clip_ids.empty()) {
        Fail(EditError::InvalidOperation,
             "ClearClips requires at least one clip", error, message);
        return false;
    }
    std::vector<Ulid> trackIds;
    std::vector<Ulid> seen;
    for (const Ulid& id : operation.clip_ids) {
        const DocumentTrack* track = candidate.FindTrackForClip(id);
        if (!track) {
            Fail(EditError::UnknownClip,
                 "unknown multi-clear clip_id '" + id + "'", error, message);
            return false;
        }
        if (std::find(seen.begin(), seen.end(), id) != seen.end()) {
            Fail(EditError::InvalidOperation,
                 "ClearClips members must be unique", error, message);
            return false;
        }
        seen.push_back(id);
        trackIds.push_back(track->id);
    }
    const std::vector<ExactTrackState> before = snapshots(trackIds);
    for (const Ulid& id : operation.clip_ids) {
        DocumentTrack* track = candidate.FindTrackForClip(id);
        track->clips.erase(
            std::remove_if(
                track->clips.begin(), track->clips.end(),
                [&](const DocumentClip& clip) { return clip.id == id; }),
            track->clips.end());
    }
    if (!ValidateResult(candidate, error, message)) return false;
    operation.exact_track_result = snapshots(trackIds);
    inverse = ClearClipsOperation{operation.clip_ids, before};
    return true;
}

bool ApplyPasteClips(Document& candidate, PasteClipsOperation& operation,
                     Operation& inverse, EditError& error,
                     std::string& message) {
    const auto snapshots = [&](const std::vector<Ulid>& trackIds) {
        std::vector<ExactTrackState> result;
        for (const Ulid& id : trackIds) {
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
        std::vector<Ulid> trackIds;
        for (const ExactTrackState& state : operation.exact_track_result)
            trackIds.push_back(state.track_id);
        const std::vector<ExactTrackState> before = snapshots(trackIds);
        if (before.size() != operation.exact_track_result.size()) {
            Fail(EditError::UnknownTrack,
                 "exact paste state references an unknown track", error,
                 message);
            return false;
        }
        for (const ExactTrackState& state : operation.exact_track_result)
            candidate.FindTrack(state.track_id)->clips = state.clips;
        if (!ValidateResult(candidate, error, message)) return false;
        inverse = PasteClipsOperation{{}, before, operation.overwrite};
        return true;
    }
    if (operation.clips.empty()) {
        Fail(EditError::InvalidOperation, "paste selection is empty", error,
             message);
        return false;
    }

    std::vector<Ulid> trackIds;
    std::vector<Ulid> copiedIds;
    std::vector<Ulid> pastedIds;
    std::map<Ulid, size_t> linkCounts;
    std::map<Ulid, Ulid> pastedByCopiedId;
    std::map<Ulid, Ulid> pastedLinkGroups;
    for (PastedClip& item : operation.clips) {
        DocumentTrack* track = candidate.FindTrack(item.track_id);
        if (!track) {
            Fail(EditError::UnknownTrack,
                 "paste references unknown track_id '" + item.track_id + "'",
                 error, message);
            return false;
        }
        const DocumentSource* source = candidate.FindSource(item.source_id);
        if (!source) {
            Fail(EditError::UnknownSource,
                 "paste references unknown source_id '" + item.source_id + "'",
                 error, message);
            return false;
        }
        if (!IsValidUlid(item.copied_clip_id) ||
            std::find(copiedIds.begin(), copiedIds.end(),
                      item.copied_clip_id) != copiedIds.end()) {
            Fail(EditError::DuplicateId,
                 "pasted copied_clip_id is invalid or duplicated", error,
                 message);
            return false;
        }
        if (item.timeline_in.rate <= 0 || item.timeline_in.value < 0) {
            Fail(EditError::InvalidTimelineIn,
                 "pasted timeline_in must be non-negative", error, message);
            return false;
        }
        if (!ValidateSourceRange(*source, item.source_in, item.duration, error,
                                 message))
            return false;
        if (item.clip_id.empty()) item.clip_id = GenerateUlid();
        if (!IsValidUlid(item.clip_id) || candidate.FindClip(item.clip_id) ||
            candidate.FindTrack(item.clip_id) ||
            candidate.FindSource(item.clip_id) ||
            std::find(pastedIds.begin(), pastedIds.end(), item.clip_id) !=
                pastedIds.end()) {
            Fail(EditError::DuplicateId,
                 "pasted clip_id is invalid or already exists", error, message);
            return false;
        }
        copiedIds.push_back(item.copied_clip_id);
        pastedIds.push_back(item.clip_id);
        pastedByCopiedId[item.copied_clip_id] = item.clip_id;
        trackIds.push_back(item.track_id);
        if (!item.copied_link_group_id.empty())
            ++linkCounts[item.copied_link_group_id];
    }

    for (PastedClip& item : operation.clips) {
        const bool linked = !item.copied_link_group_id.empty() &&
                            linkCounts[item.copied_link_group_id] > 1;
        if (!linked) {
            item.link_group_id.clear();
            item.sync_anchor_clip_id.clear();
            item.sync_reference_delta = {0, 1};
            continue;
        }
        Ulid& group = pastedLinkGroups[item.copied_link_group_id];
        if (group.empty()) {
            group = item.link_group_id.empty() ? GenerateUlid()
                                               : item.link_group_id;
        } else if (!item.link_group_id.empty() && item.link_group_id != group) {
            Fail(EditError::InvalidOperation,
                 "pasted linked clips disagree on their new link group", error,
                 message);
            return false;
        }
        item.link_group_id = group;
        const auto anchor =
            pastedByCopiedId.find(item.copied_sync_anchor_clip_id);
        if (anchor == pastedByCopiedId.end()) {
            Fail(EditError::InvalidOperation,
                 "pasted linked selection is missing its sync anchor", error,
                 message);
            return false;
        }
        item.sync_anchor_clip_id = anchor->second;
    }

    for (size_t index = 0; index < operation.clips.size(); ++index) {
        const PastedClip& item = operation.clips[index];
        const RationalTime end = item.timeline_in.add(item.duration);
        const DocumentTrack* track = candidate.FindTrack(item.track_id);
        if (!operation.overwrite)
            for (const DocumentClip& existing : track->clips) {
                if (existing.timeline_in < end &&
                    existing.timeline_in.add(existing.duration) >
                        item.timeline_in) {
                    Fail(EditError::Overlap,
                         "paste overlaps clip_id '" + existing.id + "'", error,
                         message);
                    return false;
                }
            }
        for (size_t other = 0; other < index; ++other) {
            const PastedClip& previous = operation.clips[other];
            if (previous.track_id != item.track_id) continue;
            if (previous.timeline_in < end &&
                previous.timeline_in.add(previous.duration) >
                    item.timeline_in) {
                Fail(EditError::Overlap, "pasted clips overlap each other",
                     error, message);
                return false;
            }
        }
    }

    const std::vector<ExactTrackState> before = snapshots(trackIds);
    if (operation.overwrite) {
        for (const PastedClip& item : operation.clips) {
            DocumentTrack* track = candidate.FindTrack(item.track_id);
            const RationalTime pasteEnd = item.timeline_in.add(item.duration);
            std::vector<DocumentClip> survivors;
            survivors.reserve(track->clips.size() + 1);
            for (const DocumentClip& existing : track->clips) {
                const RationalTime existingEnd =
                    existing.timeline_in.add(existing.duration);
                if (existingEnd <= item.timeline_in ||
                    existing.timeline_in >= pasteEnd) {
                    survivors.push_back(existing);
                    continue;
                }
                const bool keepLeft = existing.timeline_in < item.timeline_in;
                const bool keepRight = existingEnd > pasteEnd;
                if (keepLeft) {
                    DocumentClip left = existing;
                    left.duration = item.timeline_in.sub(existing.timeline_in);
                    survivors.push_back(std::move(left));
                }
                if (keepRight) {
                    DocumentClip right = existing;
                    if (keepLeft) right.id = GenerateUlid();
                    const RationalTime sourceOffset =
                        pasteEnd.sub(existing.timeline_in);
                    right.source_in = existing.source_in.add(sourceOffset);
                    right.duration = existingEnd.sub(pasteEnd);
                    right.timeline_in = pasteEnd;
                    survivors.push_back(std::move(right));
                }
            }
            track->clips = std::move(survivors);
        }
    }
    for (const PastedClip& item : operation.clips) {
        DocumentClip clip{item.clip_id, item.source_id, item.source_in,
                          item.duration, item.timeline_in};
        clip.include_audio = false;
        clip.link_group_id = item.link_group_id;
        clip.sync_anchor_clip_id = item.sync_anchor_clip_id;
        clip.sync_reference_delta = item.sync_reference_delta;
        candidate.FindTrack(item.track_id)->clips.push_back(std::move(clip));
    }
    for (const Ulid& id : trackIds) {
        DocumentTrack* track = candidate.FindTrack(id);
        std::stable_sort(
            track->clips.begin(), track->clips.end(),
            [](const DocumentClip& left, const DocumentClip& right) {
                return left.timeline_in < right.timeline_in;
            });
    }
    if (!ValidateResult(candidate, error, message)) return false;
    operation.exact_track_result = snapshots(trackIds);
    inverse = PasteClipsOperation{{}, before, operation.overwrite};
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
    const DocumentTrack* track = candidate.FindTrackForClip(clip->id);
    const bool captionClip = track && track->kind == "caption";
    if (!source && !captionClip) {
        Fail(EditError::UnknownSource,
             "clip references unknown source_id '" + clip->source_id + "'",
             error, message);
        return false;
    }
    const ExactClipTimes before = TimesOf(*clip);
    if (operation.edge == TrimEdge::Head) {
        if (!captionClip)
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
    if (source && !ValidateSourceRange(*source, clip->source_in, clip->duration,
                                       error, message)) {
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

bool ApplyMoveClips(Document& candidate, MoveClipsOperation& operation,
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
                 "exact multi-move references an unknown track", error,
                 message);
            return false;
        }
        for (const ExactTrackState& state : operation.exact_track_result)
            candidate.FindTrack(state.track_id)->clips = state.clips;
        if (!ValidateResult(candidate, error, message)) return false;
        inverse = MoveClipsOperation{operation.moves, before};
        return true;
    }
    if (operation.moves.size() < 2) {
        Fail(EditError::InvalidOperation,
             "MoveClips requires at least two members", error, message);
        return false;
    }
    struct OriginalMove {
        DocumentClip clip;
        Ulid source_track_id;
        LinkedClipMove destination;
    };
    std::vector<OriginalMove> originals;
    std::vector<LinkedClipMove> inverseMoves;
    std::vector<Ulid> affectedTracks;
    std::vector<Ulid> seenClips;
    for (const LinkedClipMove& move : operation.moves) {
        const DocumentClip* clip = candidate.FindClip(move.clip_id);
        const DocumentTrack* source = candidate.FindTrackForClip(move.clip_id);
        const DocumentTrack* target = candidate.FindTrack(move.track_id);
        if (!clip || !source || !target) {
            Fail(!target ? EditError::UnknownTrack : EditError::UnknownClip,
                 "multi-move references an unknown clip or track", error,
                 message);
            return false;
        }
        if (source->kind != target->kind || move.timeline_in.rate <= 0 ||
            move.timeline_in.value < 0 ||
            std::find(seenClips.begin(), seenClips.end(), move.clip_id) !=
                seenClips.end()) {
            Fail(EditError::InvalidOperation,
                 "multi-move members must be unique and keep their track kind",
                 error, message);
            return false;
        }
        seenClips.push_back(move.clip_id);
        affectedTracks.push_back(source->id);
        affectedTracks.push_back(target->id);
        originals.push_back({*clip, source->id, move});
        inverseMoves.push_back({clip->id, source->id, clip->timeline_in});
    }
    for (size_t left = 0; left < originals.size(); ++left) {
        const RationalTime leftEnd =
            originals[left].destination.timeline_in.add(
                originals[left].clip.duration);
        for (size_t right = left + 1; right < originals.size(); ++right) {
            if (originals[left].destination.track_id !=
                originals[right].destination.track_id)
                continue;
            const RationalTime rightEnd =
                originals[right].destination.timeline_in.add(
                    originals[right].clip.duration);
            if (originals[left].destination.timeline_in < rightEnd &&
                originals[right].destination.timeline_in < leftEnd) {
                Fail(EditError::Overlap,
                     "multi-move destinations overlap each other", error,
                     message);
                return false;
            }
        }
    }
    const std::vector<ExactTrackState> before = snapshots(affectedTracks);
    for (const OriginalMove& original : originals) {
        DocumentTrack* source = candidate.FindTrack(original.source_track_id);
        source->clips.erase(
            std::remove_if(source->clips.begin(), source->clips.end(),
                           [&](const DocumentClip& clip) {
                               return clip.id == original.clip.id;
                           }),
            source->clips.end());
    }
    for (const OriginalMove& original : originals) {
        DocumentTrack* source = candidate.FindTrack(original.source_track_id);
        source->clips.push_back(original.clip);
        MoveClipOperation single{original.clip.id,
                                 original.destination.track_id,
                                 original.destination.timeline_in,
                                 {}};
        Operation ignored = RemoveClipOperation{};
        if (!ApplyMove(candidate, single, ignored, error, message))
            return false;
    }
    operation.exact_track_result = snapshots(affectedTracks);
    inverse = MoveClipsOperation{std::move(inverseMoves), before};
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

bool ApplyRippleTrim(Document& candidate, RippleTrimOperation& operation,
                     Operation& inverse, EditError& error,
                     std::string& message) {
    if (operation.delta.rate <= 0) {
        Fail(EditError::ArithmeticError,
             "ripple trim delta rate must be positive", error, message);
        return false;
    }
    std::vector<Ulid> trackIds;
    const auto addTrack = [&](const Ulid& id) {
        if (!id.empty() &&
            std::find(trackIds.begin(), trackIds.end(), id) == trackIds.end())
            trackIds.push_back(id);
    };
    const auto snapshots = [&](const std::vector<Ulid>& ids) {
        std::vector<ExactTrackState> states;
        for (const Ulid& id : ids) {
            const DocumentTrack* track = candidate.FindTrack(id);
            if (track) states.push_back({id, track->clips});
        }
        return states;
    };
    if (!operation.exact_track_result.empty()) {
        for (const ExactTrackState& state : operation.exact_track_result)
            addTrack(state.track_id);
        const std::vector<ExactTrackState> before = snapshots(trackIds);
        if (before.size() != operation.exact_track_result.size()) {
            Fail(EditError::UnknownTrack,
                 "exact ripple trim references an unknown track", error,
                 message);
            return false;
        }
        for (const ExactTrackState& state : operation.exact_track_result)
            candidate.FindTrack(state.track_id)->clips = state.clips;
        if (!ValidateResult(candidate, error, message)) return false;
        inverse = RippleTrimOperation{
            operation.clip_id,        operation.edge,
            operation.delta,          operation.linked_clip_ids,
            operation.sync_track_ids, before};
        return true;
    }

    DocumentClip* anchor = candidate.FindClip(operation.clip_id);
    const DocumentTrack* anchorTrack =
        candidate.FindTrackForClip(operation.clip_id);
    if (!anchor || !anchorTrack) {
        Fail(EditError::UnknownClip, "unknown ripple trim clip", error,
             message);
        return false;
    }
    std::vector<Ulid> trimIds{operation.clip_id};
    for (const Ulid& id : operation.linked_clip_ids)
        if (std::find(trimIds.begin(), trimIds.end(), id) == trimIds.end())
            trimIds.push_back(id);
    for (const Ulid& id : trimIds) {
        const DocumentClip* clip = candidate.FindClip(id);
        const DocumentTrack* track = candidate.FindTrackForClip(id);
        if (!clip || !track ||
            (id != operation.clip_id &&
             (anchor->link_group_id.empty() ||
              clip->link_group_id != anchor->link_group_id))) {
            Fail(EditError::InvalidOperation,
                 "ripple trim linked clips must share one group", error,
                 message);
            return false;
        }
        addTrack(track->id);
    }
    for (const Ulid& id : operation.sync_track_ids) {
        if (!candidate.FindTrack(id)) {
            Fail(EditError::UnknownTrack,
                 "unknown ripple sync track_id '" + id + "'", error, message);
            return false;
        }
        addTrack(id);
    }
    const std::vector<ExactTrackState> before = snapshots(trackIds);
    const RationalTime primaryBoundary =
        operation.edge == TrimEdge::Head
            ? anchor->timeline_in
            : anchor->timeline_in.add(anchor->duration);
    RationalTime sequenceDelta = operation.delta;
    if (operation.edge == TrimEdge::Head &&
        !Negate(operation.delta, sequenceDelta)) {
        Fail(EditError::ArithmeticError, "ripple delta cannot be negated",
             error, message);
        return false;
    }
    std::map<Ulid, RationalTime> followingBoundary;
    for (const Ulid& id : trimIds) {
        DocumentClip* clip = candidate.FindClip(id);
        const DocumentTrack* track = candidate.FindTrackForClip(id);
        const RationalTime originalEnd = clip->timeline_in.add(clip->duration);
        followingBoundary[track->id] = originalEnd;
        if (operation.edge == TrimEdge::Head) {
            clip->source_in = clip->source_in.add(operation.delta);
            clip->duration = clip->duration.sub(operation.delta);
        } else {
            clip->duration = clip->duration.add(operation.delta);
        }
        const DocumentSource* source = candidate.FindSource(clip->source_id);
        if (clip->duration.value <= 0) {
            Fail(EditError::InvalidDuration,
                 "ripple trim would make duration non-positive", error,
                 message);
            return false;
        }
        if (!source) {
            Fail(EditError::UnknownSource,
                 "ripple trim clip references an unknown source", error,
                 message);
            return false;
        }
        if (!ValidateSourceRange(*source, clip->source_in, clip->duration,
                                 error, message)) {
            return false;
        }
    }
    for (const auto& boundary : followingBoundary) {
        DocumentTrack* track = candidate.FindTrack(boundary.first);
        for (DocumentClip& clip : track->clips) {
            if (std::find(trimIds.begin(), trimIds.end(), clip.id) !=
                trimIds.end())
                continue;
            if (clip.timeline_in >= boundary.second)
                clip.timeline_in = clip.timeline_in.add(sequenceDelta);
        }
    }
    for (const Ulid& id : operation.sync_track_ids) {
        if (followingBoundary.count(id)) continue;
        DocumentTrack* track = candidate.FindTrack(id);
        for (DocumentClip& clip : track->clips)
            if (clip.timeline_in >= primaryBoundary)
                clip.timeline_in = clip.timeline_in.add(sequenceDelta);
    }
    if (!ValidateResult(candidate, error, message)) return false;
    operation.exact_track_result = snapshots(trackIds);
    inverse = RippleTrimOperation{
        operation.clip_id,         operation.edge,           operation.delta,
        operation.linked_clip_ids, operation.sync_track_ids, before};
    return true;
}

bool ApplyRollEdit(Document& candidate, RollEditOperation& operation,
                   Operation& inverse, EditError& error, std::string& message) {
    if (operation.delta.rate <= 0 || operation.pairs.empty()) {
        Fail(EditError::InvalidOperation,
             "roll edit requires pairs and a positive delta rate", error,
             message);
        return false;
    }
    std::vector<Ulid> trackIds;
    const auto addTrack = [&](const Ulid& id) {
        if (!id.empty() &&
            std::find(trackIds.begin(), trackIds.end(), id) == trackIds.end())
            trackIds.push_back(id);
    };
    const auto snapshots = [&](const std::vector<Ulid>& ids) {
        std::vector<ExactTrackState> states;
        for (const Ulid& id : ids) {
            const DocumentTrack* track = candidate.FindTrack(id);
            if (track) states.push_back({id, track->clips});
        }
        return states;
    };
    if (!operation.exact_track_result.empty()) {
        for (const ExactTrackState& state : operation.exact_track_result)
            addTrack(state.track_id);
        const std::vector<ExactTrackState> before = snapshots(trackIds);
        if (before.size() != operation.exact_track_result.size()) {
            Fail(EditError::UnknownTrack,
                 "exact roll edit references an unknown track", error, message);
            return false;
        }
        for (const ExactTrackState& state : operation.exact_track_result)
            candidate.FindTrack(state.track_id)->clips = state.clips;
        if (!ValidateResult(candidate, error, message)) return false;
        inverse = RollEditOperation{operation.pairs, operation.delta, before};
        return true;
    }
    std::vector<Ulid> seenClips;
    for (const RollEditPair& pair : operation.pairs) {
        DocumentClip* left = candidate.FindClip(pair.left_clip_id);
        DocumentClip* right = candidate.FindClip(pair.right_clip_id);
        const DocumentTrack* leftTrack =
            candidate.FindTrackForClip(pair.left_clip_id);
        const DocumentTrack* rightTrack =
            candidate.FindTrackForClip(pair.right_clip_id);
        if (!left || !right || !leftTrack || !rightTrack ||
            leftTrack->id != rightTrack->id ||
            left->timeline_in.add(left->duration) != right->timeline_in ||
            std::find(seenClips.begin(), seenClips.end(), left->id) !=
                seenClips.end() ||
            std::find(seenClips.begin(), seenClips.end(), right->id) !=
                seenClips.end()) {
            Fail(EditError::InvalidOperation,
                 "roll edit requires unique contiguous clip pairs", error,
                 message);
            return false;
        }
        seenClips.push_back(left->id);
        seenClips.push_back(right->id);
        addTrack(leftTrack->id);
    }
    const std::vector<ExactTrackState> before = snapshots(trackIds);
    for (const RollEditPair& pair : operation.pairs) {
        DocumentClip* left = candidate.FindClip(pair.left_clip_id);
        DocumentClip* right = candidate.FindClip(pair.right_clip_id);
        left->duration = left->duration.add(operation.delta);
        right->source_in = right->source_in.add(operation.delta);
        right->duration = right->duration.sub(operation.delta);
        right->timeline_in = right->timeline_in.add(operation.delta);
        if (left->duration.value <= 0 || right->duration.value <= 0) {
            Fail(EditError::InvalidDuration,
                 "roll edit would make a clip non-positive", error, message);
            return false;
        }
        const DocumentSource* leftSource =
            candidate.FindSource(left->source_id);
        const DocumentSource* rightSource =
            candidate.FindSource(right->source_id);
        if (!leftSource || !rightSource ||
            !ValidateSourceRange(*leftSource, left->source_in, left->duration,
                                 error, message) ||
            !ValidateSourceRange(*rightSource, right->source_in,
                                 right->duration, error, message))
            return false;
    }
    if (!ValidateResult(candidate, error, message)) return false;
    operation.exact_track_result = snapshots(trackIds);
    inverse = RollEditOperation{operation.pairs, operation.delta, before};
    return true;
}

bool ApplySlipEdit(Document& candidate, SlipEditOperation& operation,
                   Operation& inverse, EditError& error, std::string& message) {
    if (operation.delta.rate <= 0 || operation.clip_ids.empty()) {
        Fail(EditError::InvalidOperation,
             "slip edit requires clips and a positive delta rate", error,
             message);
        return false;
    }
    std::vector<Ulid> trackIds;
    const auto addTrack = [&](const Ulid& id) {
        if (!id.empty() &&
            std::find(trackIds.begin(), trackIds.end(), id) == trackIds.end())
            trackIds.push_back(id);
    };
    const auto snapshots = [&](const std::vector<Ulid>& ids) {
        std::vector<ExactTrackState> states;
        for (const Ulid& id : ids) {
            const DocumentTrack* track = candidate.FindTrack(id);
            if (track) states.push_back({id, track->clips});
        }
        return states;
    };
    if (!operation.exact_track_result.empty()) {
        for (const ExactTrackState& state : operation.exact_track_result)
            addTrack(state.track_id);
        const std::vector<ExactTrackState> before = snapshots(trackIds);
        if (before.size() != operation.exact_track_result.size()) {
            Fail(EditError::UnknownTrack,
                 "exact slip edit references an unknown track", error, message);
            return false;
        }
        for (const ExactTrackState& state : operation.exact_track_result)
            candidate.FindTrack(state.track_id)->clips = state.clips;
        if (!ValidateResult(candidate, error, message)) return false;
        inverse =
            SlipEditOperation{operation.clip_ids, operation.delta, before};
        return true;
    }

    std::vector<Ulid> seen;
    Ulid linkGroup;
    for (const Ulid& id : operation.clip_ids) {
        DocumentClip* clip = candidate.FindClip(id);
        const DocumentTrack* track = candidate.FindTrackForClip(id);
        if (!clip || !track ||
            std::find(seen.begin(), seen.end(), id) != seen.end()) {
            Fail(EditError::InvalidOperation,
                 "slip edit clip IDs must exist and be unique", error, message);
            return false;
        }
        if (operation.clip_ids.size() > 1) {
            if (clip->link_group_id.empty()) {
                Fail(EditError::InvalidOperation,
                     "multi-clip slip requires one linked group", error,
                     message);
                return false;
            }
            if (linkGroup.empty()) linkGroup = clip->link_group_id;
            if (clip->link_group_id != linkGroup) {
                Fail(EditError::InvalidOperation,
                     "multi-clip slip members must share one linked group",
                     error, message);
                return false;
            }
        }
        seen.push_back(id);
        addTrack(track->id);
    }
    const std::vector<ExactTrackState> before = snapshots(trackIds);
    for (const Ulid& id : operation.clip_ids) {
        DocumentClip* clip = candidate.FindClip(id);
        const DocumentSource* source = candidate.FindSource(clip->source_id);
        if (!source) {
            Fail(EditError::UnknownSource,
                 "slip edit clip references an unknown source", error, message);
            return false;
        }
        clip->source_in = clip->source_in.add(operation.delta);
        if (!ValidateSourceRange(*source, clip->source_in, clip->duration,
                                 error, message))
            return false;
    }
    if (!ValidateResult(candidate, error, message)) return false;
    operation.exact_track_result = snapshots(trackIds);
    inverse = SlipEditOperation{operation.clip_ids, operation.delta, before};
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

bool ApplyClearLinked(Document& candidate, ClearLinkedClipsOperation& operation,
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
            std::remove_if(
                track->clips.begin(), track->clips.end(),
                [&](const DocumentClip& clip) { return clip.id == id; }),
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
        inverse = DeleteGapOperation{operation.track_id, operation.gap_start,
                                     operation.gap_duration, before,
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
    inverse = DeleteGapOperation{operation.track_id, operation.gap_start,
                                 operation.gap_duration, before,
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
    if (operation.kind != "video" && operation.kind != "audio" &&
        operation.kind != "caption") {
        Fail(EditError::InvalidOperation,
             "track kind must be 'video', 'audio' or 'caption'", error,
             message);
        return false;
    }
    if (operation.index < 0 ||
        std::any_of(
            candidate.sequence.tracks.begin(), candidate.sequence.tracks.end(),
            [&](const DocumentTrack& track) {
                return track.index == operation.index;
            })) {
        Fail(EditError::InvalidOperation,
             "track index must be non-negative and unique", error, message);
        return false;
    }
    candidate.sequence.tracks.push_back(
        {operation.track_id, operation.kind, operation.index, operation.clips,
         operation.locked, operation.sync_lock, operation.visible,
         operation.muted, operation.solo});
    if (!ValidateResult(candidate, error, message)) return false;
    inverse = RemoveTrackOperation{operation.track_id};
    return true;
}

bool ApplyRemoveTrack(Document& candidate, RemoveTrackOperation& operation,
                      Operation& inverse, EditError& error,
                      std::string& message) {
    const auto found = std::find_if(candidate.sequence.tracks.begin(),
                                    candidate.sequence.tracks.end(),
                                    [&](const DocumentTrack& track) {
                                        return track.id == operation.track_id;
                                    });
    if (found == candidate.sequence.tracks.end()) {
        Fail(EditError::UnknownTrack,
             "unknown track_id '" + operation.track_id + "'", error, message);
        return false;
    }
    inverse = AddTrackOperation{found->id,      found->kind,   found->index,
                                found->clips,   found->locked, found->sync_lock,
                                found->visible, found->muted,  found->solo};
    candidate.sequence.tracks.erase(found);
    return ValidateResult(candidate, error, message);
}

bool ApplySetTrackLock(Document& candidate, SetTrackLockOperation& operation,
                       Operation& inverse, EditError& error,
                       std::string& message) {
    DocumentTrack* track = candidate.FindTrack(operation.track_id);
    if (!track) {
        Fail(EditError::UnknownTrack,
             "unknown track_id '" + operation.track_id + "'", error, message);
        return false;
    }
    inverse = SetTrackLockOperation{track->id, track->locked};
    track->locked = operation.locked;
    return ValidateResult(candidate, error, message);
}

bool ApplySetTrackSyncLock(Document& candidate,
                           SetTrackSyncLockOperation& operation,
                           Operation& inverse, EditError& error,
                           std::string& message) {
    DocumentTrack* track = candidate.FindTrack(operation.track_id);
    if (!track) {
        Fail(EditError::UnknownTrack,
             "unknown track_id '" + operation.track_id + "'", error, message);
        return false;
    }
    inverse = SetTrackSyncLockOperation{track->id, track->sync_lock};
    track->sync_lock = operation.sync_lock;
    return ValidateResult(candidate, error, message);
}

bool ApplySetTrackOutput(Document& candidate,
                         SetTrackOutputOperation& operation, Operation& inverse,
                         EditError& error, std::string& message) {
    DocumentTrack* track = candidate.FindTrack(operation.track_id);
    if (!track) {
        Fail(EditError::UnknownTrack,
             "unknown track_id '" + operation.track_id + "'", error, message);
        return false;
    }
    inverse = SetTrackOutputOperation{track->id, track->visible, track->muted,
                                      track->solo};
    track->visible = operation.visible;
    track->muted = operation.muted;
    track->solo = operation.solo;
    return ValidateResult(candidate, error, message);
}

bool ApplyUpdateSequence(Document& candidate,
                         UpdateSequenceOperation& operation, Operation& inverse,
                         EditError& error, std::string& message) {
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

bool ApplySetColorManagement(Document& candidate,
                             SetColorManagementOperation& operation,
                             Operation& inverse, EditError& error,
                             std::string& message) {
    inverse = SetColorManagementOperation{candidate.color_management};
    candidate.color_management = operation.settings;
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
                  Operation& inverse, EditError& error, std::string& message) {
    DocumentBin* bin = candidate.FindBin(operation.bin_id);
    if (!bin) {
        Fail(EditError::UnknownBin, "unknown bin_id '" + operation.bin_id + "'",
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
    const bool collision = id == candidate.sequence.id ||
                           candidate.FindMarker(id) || candidate.FindBin(id) ||
                           candidate.FindLibraryMedia(id) ||
                           candidate.FindSource(id) ||
                           candidate.FindTrack(id) || candidate.FindClip(id);
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
    const auto found = std::find_if(candidate.sequence.markers.begin(),
                                    candidate.sequence.markers.end(),
                                    [&](const DocumentMarker& marker) {
                                        return marker.id == operation.marker_id;
                                    });
    if (found == candidate.sequence.markers.end()) {
        Fail(EditError::UnknownMarker,
             "unknown marker_id '" + operation.marker_id + "'", error, message);
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
             "unknown marker_id '" + operation.marker_id + "'", error, message);
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

bool ApplyAddCaptionStyle(Document& candidate,
                          AddCaptionStyleOperation& operation,
                          Operation& inverse, EditError& error,
                          std::string& message) {
    if (operation.style.id.empty()) operation.style.id = GenerateUlid();
    const Ulid& id = operation.style.id;
    const bool collision =
        id == candidate.sequence.id || candidate.FindCaptionStyle(id) ||
        candidate.FindMulticamGroup(id) || candidate.FindMarker(id) ||
        candidate.FindTransition(id) || candidate.FindBin(id) ||
        candidate.FindLibraryMedia(id) || candidate.FindSource(id) ||
        candidate.FindTrack(id) || candidate.FindClip(id);
    if (!IsValidUlid(id) || collision) {
        Fail(EditError::DuplicateId,
             "caption style id is invalid or already exists: '" + id + "'",
             error, message);
        return false;
    }
    if (operation.insertion_index < -1 ||
        (operation.insertion_index >= 0 &&
         static_cast<uint64_t>(operation.insertion_index) >
             candidate.sequence.caption_styles.size())) {
        Fail(EditError::InvalidOperation,
             "caption style insertion_index is outside the style list", error,
             message);
        return false;
    }
    if (operation.insertion_index < 0)
        operation.insertion_index =
            static_cast<int64_t>(candidate.sequence.caption_styles.size());
    candidate.sequence.caption_styles.insert(
        candidate.sequence.caption_styles.begin() + operation.insertion_index,
        operation.style);
    if (!ValidateResult(candidate, error, message)) return false;
    inverse = RemoveCaptionStyleOperation{id};
    return true;
}

bool ApplyRemoveCaptionStyle(Document& candidate,
                             RemoveCaptionStyleOperation& operation,
                             Operation& inverse, EditError& error,
                             std::string& message) {
    auto& styles = candidate.sequence.caption_styles;
    const auto found = std::find_if(styles.begin(), styles.end(),
                                    [&](const CaptionStyle& style) {
                                        return style.id == operation.style_id;
                                    });
    if (found == styles.end()) {
        Fail(EditError::UnknownCaptionStyle,
             "unknown caption style_id '" + operation.style_id + "'", error,
             message);
        return false;
    }
    const bool inUse = std::any_of(
        candidate.sequence.tracks.begin(), candidate.sequence.tracks.end(),
        [&](const DocumentTrack& track) {
            return std::any_of(track.clips.begin(), track.clips.end(),
                               [&](const DocumentClip& clip) {
                                   return clip.caption_group_id ==
                                          operation.style_id;
                               });
        });
    if (inUse) {
        Fail(EditError::InvalidOperation,
             "caption style_id '" + operation.style_id +
                 "' is still referenced by a clip",
             error, message);
        return false;
    }
    const int64_t index =
        static_cast<int64_t>(std::distance(styles.begin(), found));
    const CaptionStyle removed = *found;
    styles.erase(found);
    if (!ValidateResult(candidate, error, message)) return false;
    inverse = AddCaptionStyleOperation{removed, index};
    return true;
}

bool ApplySetClipCaption(Document& candidate,
                         SetClipCaptionOperation& operation, Operation& inverse,
                         EditError& error, std::string& message) {
    DocumentClip* clip = candidate.FindClip(operation.clip_id);
    if (!clip) {
        Fail(EditError::UnknownClip,
             "unknown clip_id '" + operation.clip_id + "'", error, message);
        return false;
    }
    const Ulid beforeGroup = clip->caption_group_id;
    const std::string beforeText = clip->caption_text;
    clip->caption_group_id = operation.caption_group_id;
    clip->caption_text = operation.caption_text;
    if (!ValidateResult(candidate, error, message)) return false;
    inverse = SetClipCaptionOperation{clip->id, beforeGroup, beforeText};
    return true;
}

// F1.5 ("Multicam"). Registers a new multicam group and its angles. Angle
// clip existence, video-track membership, no-clip-reused-within-group and
// active_angle_id membership are all schema-level invariants already
// enforced by Document::Validate (see MulticamGroup validation), so this
// only owns the operation's own precondition: a fresh, collision-free group
// id and a well-formed insertion_index, mirroring
// ApplyAddTransition/ApplyAddCaptionStyle.
bool ApplyAddMulticamGroup(Document& candidate,
                           AddMulticamGroupOperation& operation,
                           Operation& inverse, EditError& error,
                           std::string& message) {
    if (operation.group.id.empty()) operation.group.id = GenerateUlid();
    const Ulid& id = operation.group.id;
    const bool collision =
        id == candidate.sequence.id || candidate.FindMulticamGroup(id) ||
        candidate.FindCaptionStyle(id) || candidate.FindMarker(id) ||
        candidate.FindTransition(id) || candidate.FindBin(id) ||
        candidate.FindLibraryMedia(id) || candidate.FindSource(id) ||
        candidate.FindTrack(id) || candidate.FindClip(id);
    if (!IsValidUlid(id) || collision) {
        Fail(EditError::DuplicateId,
             "multicam group id is invalid or already exists: '" + id + "'",
             error, message);
        return false;
    }
    for (MulticamAngle& angle : operation.group.angles) {
        if (angle.id.empty()) angle.id = GenerateUlid();
    }
    if (operation.insertion_index < -1 ||
        (operation.insertion_index >= 0 &&
         static_cast<uint64_t>(operation.insertion_index) >
             candidate.sequence.multicam_groups.size())) {
        Fail(EditError::InvalidOperation,
             "multicam group insertion_index is outside the group list", error,
             message);
        return false;
    }
    if (operation.insertion_index < 0)
        operation.insertion_index =
            static_cast<int64_t>(candidate.sequence.multicam_groups.size());
    candidate.sequence.multicam_groups.insert(
        candidate.sequence.multicam_groups.begin() + operation.insertion_index,
        operation.group);
    if (!ValidateResult(candidate, error, message)) return false;
    inverse = RemoveMulticamGroupOperation{id};
    return true;
}

// Exact inverse of ApplyAddMulticamGroup. No other schema field references a
// multicam group by id, so unlike caption styles there is no "still in use"
// precondition to check here.
bool ApplyRemoveMulticamGroup(Document& candidate,
                              RemoveMulticamGroupOperation& operation,
                              Operation& inverse, EditError& error,
                              std::string& message) {
    auto& groups = candidate.sequence.multicam_groups;
    const auto found = std::find_if(
        groups.begin(), groups.end(),
        [&](const auto& group) { return group.id == operation.group_id; });
    if (found == groups.end()) {
        Fail(EditError::UnknownMulticamGroup,
             "unknown multicam group_id '" + operation.group_id + "'", error,
             message);
        return false;
    }
    const int64_t index =
        static_cast<int64_t>(std::distance(groups.begin(), found));
    const MulticamGroup removed = *found;
    groups.erase(found);
    if (!ValidateResult(candidate, error, message)) return false;
    inverse = AddMulticamGroupOperation{removed, index};
    return true;
}

// Changes the active angle of an existing group. The group itself must
// exist (EditError::UnknownMulticamGroup); a non-empty active_angle_id must
// name one of that group's own angles (EditError::UnknownMulticamAngle) —
// Document::Validate would also catch a dangling active_angle_id, but only
// as a generic ValidationFailed, so this checks it directly for an exact
// error the way UpdateMarker/SetClipLink check their own references.
bool ApplySetMulticamActiveAngle(Document& candidate,
                                 SetMulticamActiveAngleOperation& operation,
                                 Operation& inverse, EditError& error,
                                 std::string& message) {
    MulticamGroup* group = candidate.FindMulticamGroup(operation.group_id);
    if (!group) {
        Fail(EditError::UnknownMulticamGroup,
             "unknown multicam group_id '" + operation.group_id + "'", error,
             message);
        return false;
    }
    if (!operation.active_angle_id.empty() &&
        std::none_of(group->angles.begin(), group->angles.end(),
                     [&](const MulticamAngle& angle) {
                         return angle.id == operation.active_angle_id;
                     })) {
        Fail(EditError::UnknownMulticamAngle,
             "active_angle_id '" + operation.active_angle_id +
                 "' is not an angle of multicam group '" + operation.group_id +
                 "'",
             error, message);
        return false;
    }
    const Ulid before = group->active_angle_id;
    group->active_angle_id = operation.active_angle_id;
    if (!ValidateResult(candidate, error, message)) return false;
    inverse = SetMulticamActiveAngleOperation{group->id, before};
    return true;
}

bool ApplyAddTransition(Document& candidate, AddTransitionOperation& operation,
                        Operation& inverse, EditError& error,
                        std::string& message) {
    if (operation.transition.id.empty())
        operation.transition.id = GenerateUlid();
    const Ulid& id = operation.transition.id;
    const bool collision =
        id == candidate.sequence.id || candidate.FindTransition(id) ||
        candidate.FindMarker(id) || candidate.FindBin(id) ||
        candidate.FindLibraryMedia(id) || candidate.FindSource(id) ||
        candidate.FindTrack(id) || candidate.FindClip(id);
    if (!IsValidUlid(id) || collision) {
        Fail(EditError::DuplicateId,
             "transition_id is invalid or already exists: '" + id + "'", error,
             message);
        return false;
    }
    if (operation.insertion_index < -1 ||
        (operation.insertion_index >= 0 &&
         static_cast<uint64_t>(operation.insertion_index) >
             candidate.sequence.transitions.size())) {
        Fail(EditError::InvalidOperation,
             "transition insertion_index is outside the transition list", error,
             message);
        return false;
    }
    if (operation.insertion_index < 0)
        operation.insertion_index =
            static_cast<int64_t>(candidate.sequence.transitions.size());
    candidate.sequence.transitions.insert(
        candidate.sequence.transitions.begin() + operation.insertion_index,
        operation.transition);
    if (!ValidateResult(candidate, error, message)) return false;
    inverse = RemoveTransitionOperation{id};
    return true;
}

bool ApplyRemoveTransition(Document& candidate,
                           RemoveTransitionOperation& operation,
                           Operation& inverse, EditError& error,
                           std::string& message) {
    const auto found = std::find_if(
        candidate.sequence.transitions.begin(),
        candidate.sequence.transitions.end(), [&](const auto& transition) {
            return transition.id == operation.transition_id;
        });
    if (found == candidate.sequence.transitions.end()) {
        Fail(EditError::UnknownTransition,
             "unknown transition_id '" + operation.transition_id + "'", error,
             message);
        return false;
    }
    const int64_t index = static_cast<int64_t>(
        std::distance(candidate.sequence.transitions.begin(), found));
    const DocumentTransition removed = *found;
    candidate.sequence.transitions.erase(found);
    if (!ValidateResult(candidate, error, message)) return false;
    inverse = AddTransitionOperation{removed, index};
    return true;
}

bool ApplyUpdateTransition(Document& candidate,
                           UpdateTransitionOperation& operation,
                           Operation& inverse, EditError& error,
                           std::string& message) {
    DocumentTransition* transition =
        candidate.FindTransition(operation.transition_id);
    if (!transition) {
        Fail(EditError::UnknownTransition,
             "unknown transition_id '" + operation.transition_id + "'", error,
             message);
        return false;
    }
    const DocumentTransition before = *transition;
    transition->type = operation.type;
    transition->duration = operation.duration;
    transition->alignment = operation.alignment;
    if (!ValidateResult(candidate, error, message)) return false;
    inverse = UpdateTransitionOperation{before.id, before.type, before.duration,
                                        before.alignment};
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

bool ApplySetClipEffects(Document& candidate,
                         SetClipEffectsOperation& operation, Operation& inverse,
                         EditError& error, std::string& message) {
    DocumentClip* clip = candidate.FindClip(operation.clip_id);
    if (!clip) {
        Fail(EditError::UnknownClip,
             "unknown clip_id '" + operation.clip_id + "'", error, message);
        return false;
    }
    const std::vector<ClipEffect> before = clip->effects;
    for (ClipEffect& effect : operation.effects) {
        if (effect.id.empty()) effect.id = GenerateUlid();
    }
    clip->effects = operation.effects;
    if (!ValidateResult(candidate, error, message)) return false;
    inverse = SetClipEffectsOperation{clip->id, before};
    return true;
}

bool ApplySetClipOpacity(Document& candidate,
                         SetClipOpacityOperation& operation, Operation& inverse,
                         EditError& error, std::string& message) {
    DocumentClip* clip = candidate.FindClip(operation.clip_id);
    const DocumentTrack* track =
        clip ? candidate.FindTrackForClip(operation.clip_id) : nullptr;
    if (!clip) {
        Fail(EditError::UnknownClip,
             "unknown clip_id '" + operation.clip_id + "'", error, message);
        return false;
    }
    if (!track || track->kind != "video") {
        Fail(EditError::InvalidOperation,
             "clip opacity is only supported on video tracks", error, message);
        return false;
    }
    const EffectParamValue before = clip->opacity;
    clip->opacity = operation.opacity;
    if (!ValidateResult(candidate, error, message)) return false;
    inverse = SetClipOpacityOperation{clip->id, before};
    return true;
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
    // Copy the original and override only what a cut actually changes --
    // identity and *where* the piece sits. Everything else a clip carries
    // describes the material, not its placement, so it belongs to both
    // halves. This used to build the right half from an explicit field
    // list, which silently dropped every field the list had not been taught
    // about: ClipEffect stacks and captions were added to DocumentClip
    // without touching this, so splitting a graded clip lost the grade on
    // every piece but the first, with no error anywhere.
    //
    // Captions are copied wholesale rather than re-sliced: duplicating the
    // run's text on both halves is wrong for a cut that lands mid-caption,
    // but re-slicing needs word timings this operation does not have, and
    // silently discarding the text is worse than duplicating it.
    if (operation.right_effect_ids.empty() && !original.effects.empty()) {
        for (size_t effect = 0; effect < original.effects.size(); ++effect)
            operation.right_effect_ids.push_back(GenerateUlid());
    }
    if (operation.right_effect_ids.size() != original.effects.size()) {
        Fail(EditError::InvalidOperation,
             "split requires one right effect ID per effect on the clip", error,
             message);
        return false;
    }
    for (const Ulid& effectId : operation.right_effect_ids) {
        if (!IsValidUlid(effectId) || candidate.FindClip(effectId) ||
            candidate.FindSource(effectId) || candidate.FindTrack(effectId)) {
            Fail(EditError::DuplicateId,
                 "split right effect ID is invalid or already exists: '" +
                     effectId + "'",
                 error, message);
            return false;
        }
    }

    DocumentClip right = original;
    right.id = operation.right_clip_id;
    right.source_in = original.source_in.add(leftDuration);
    right.duration = original.duration.sub(leftDuration);
    right.timeline_in = operation.timeline_position;
    for (size_t effect = 0; effect < right.effects.size(); ++effect)
        right.effects[effect].id = operation.right_effect_ids[effect];
    track->clips[index] = std::move(left);
    track->clips.insert(
        track->clips.begin() + static_cast<std::ptrdiff_t>(index + 1),
        std::move(right));
    if (!ValidateResult(candidate, error, message)) return false;
    inverse = JoinClipOperation{original.id, operation.right_clip_id,
                                TimesOf(original)};
    return true;
}

bool ApplySplitLinked(Document& candidate, SplitLinkedClipsOperation& operation,
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
        inverse = SplitLinkedClipsOperation{operation.link_group_id,
                                            operation.clip_ids,
                                            operation.timeline_position,
                                            operation.left_group_id,
                                            operation.right_group_id,
                                            operation.right_clip_ids,
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
    if (operation.left_group_id.empty())
        operation.left_group_id = GenerateUlid();
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
        if (!ApplySplit(candidate, split, ignored, error, message))
            return false;
    }
    for (size_t index = 0; index < operation.clip_ids.size(); ++index) {
        DocumentClip* left = candidate.FindClip(operation.clip_ids[index]);
        DocumentClip* right =
            candidate.FindClip(operation.right_clip_ids[index]);
        left->link_group_id = operation.left_group_id;
        left->sync_anchor_clip_id = leftAnchor;
        right->link_group_id = operation.right_group_id;
        right->sync_anchor_clip_id = rightAnchor;
    }
    if (!ValidateResult(candidate, error, message)) return false;
    operation.exact_track_result = snapshots(operation.clip_ids);
    inverse = SplitLinkedClipsOperation{operation.link_group_id,
                                        operation.clip_ids,
                                        operation.timeline_position,
                                        operation.left_group_id,
                                        operation.right_group_id,
                                        operation.right_clip_ids,
                                        before};
    return true;
}

// Every track the gesture can touch: the clip's own, plus each A/V-linked
// member's. Snapshotting all of them is what makes one undo restore the whole
// subdivision, however many pieces it produced.
std::vector<Ulid> SplitAffectedTrackIds(const Document& document,
                                        const DocumentClip& clip) {
    std::vector<Ulid> trackIds;
    const auto addTrackOf = [&](const Ulid& clipId) {
        const DocumentTrack* track = document.FindTrackForClip(clipId);
        if (track && std::find(trackIds.begin(), trackIds.end(), track->id) ==
                         trackIds.end())
            trackIds.push_back(track->id);
    };
    addTrackOf(clip.id);
    if (!clip.link_group_id.empty()) {
        for (const DocumentTrack& track : document.sequence.tracks)
            for (const DocumentClip& member : track.clips)
                if (member.link_group_id == clip.link_group_id)
                    addTrackOf(member.id);
    }
    return trackIds;
}

// The single-position cut for `clip`: linked when the clip belongs to a group
// whose members actually straddle `position`, plain otherwise. Deliberately a
// local twin of TimelineView.cc's TimelineCutOperationForClip rather than a
// call into it -- Operations.cc is the layer below the timeline view and does
// not depend on it. Both read the same link_group_id off the document, so
// they agree by construction; only the caller's linked-selection preference,
// which is a view concern, is missing here.
Operation TimelineCutOperation(const Document& document,
                               const DocumentClip& clip,
                               const RationalTime& position) {
    if (!clip.link_group_id.empty()) {
        std::vector<Ulid> members;
        for (const DocumentTrack& track : document.sequence.tracks) {
            for (const DocumentClip& member : track.clips) {
                if (member.link_group_id != clip.link_group_id) continue;
                if (position <= member.timeline_in ||
                    position >= member.timeline_in.add(member.duration))
                    continue;
                members.push_back(member.id);
            }
        }
        if (members.size() > 1)
            return SplitLinkedClipsOperation{
                clip.link_group_id, members, position, {}, {}, {}, {}};
    }
    return SplitClipOperation{clip.id, position, {}, {}};
}

bool ApplySplitAtPositions(Document& candidate,
                           SplitClipAtPositionsOperation& operation,
                           Operation& inverse, EditError& error,
                           std::string& message) {
    const auto snapshots = [&](const std::vector<Ulid>& trackIds) {
        std::vector<ExactTrackState> result;
        for (const Ulid& trackId : trackIds) {
            const DocumentTrack* track = candidate.FindTrack(trackId);
            if (track) result.push_back({track->id, track->clips});
        }
        return result;
    };
    if (!operation.exact_track_result.empty()) {
        std::vector<ExactTrackState> before;
        for (const ExactTrackState& state : operation.exact_track_result) {
            DocumentTrack* track = candidate.FindTrack(state.track_id);
            if (!track) {
                Fail(EditError::UnknownTrack,
                     "exact interval split references an unknown track", error,
                     message);
                return false;
            }
            before.push_back({track->id, track->clips});
        }
        for (const ExactTrackState& state : operation.exact_track_result)
            candidate.FindTrack(state.track_id)->clips = state.clips;
        if (!ValidateResult(candidate, error, message)) return false;
        inverse = SplitClipAtPositionsOperation{
            operation.clip_id, operation.positions, std::move(before)};
        return true;
    }

    const DocumentClip* original = candidate.FindClip(operation.clip_id);
    if (!original) {
        Fail(EditError::UnknownClip,
             "unknown clip_id '" + operation.clip_id + "'", error, message);
        return false;
    }
    if (operation.positions.empty()) {
        Fail(EditError::InvalidOperation,
             "split requires at least one position", error, message);
        return false;
    }
    const RationalTime clipStart = original->timeline_in;
    const RationalTime clipEnd = clipStart.add(original->duration);
    for (size_t index = 0; index < operation.positions.size(); ++index) {
        const RationalTime& position = operation.positions[index];
        if (position.rate <= 0) {
            Fail(EditError::ArithmeticError, "time rate must be positive",
                 error, message);
            return false;
        }
        if (position <= clipStart || position >= clipEnd) {
            Fail(EditError::InvalidTimelineIn,
                 "split position must be strictly inside the clip", error,
                 message);
            return false;
        }
        if (index > 0 && position <= operation.positions[index - 1]) {
            Fail(EditError::InvalidOperation,
                 "split positions must be strictly increasing", error, message);
            return false;
        }
    }

    const std::vector<Ulid> trackIds =
        SplitAffectedTrackIds(candidate, *original);
    const std::vector<ExactTrackState> before = snapshots(trackIds);

    // Each position is applied through the ordinary single-cut path, so a
    // subdivision is exactly N normal cuts -- same link-group bookkeeping,
    // same effect-stack cloning -- and never a second implementation of
    // "split" that could drift from it. Positions ascend, so each one lands
    // in the right-hand piece produced by the previous.
    for (const RationalTime& position : operation.positions) {
        const DocumentClip* piece = nullptr;
        for (const Ulid& trackId : trackIds) {
            const DocumentTrack* track = candidate.FindTrack(trackId);
            if (!track) continue;
            for (const DocumentClip& candidateClip : track->clips) {
                if (candidateClip.timeline_in < position &&
                    position <
                        candidateClip.timeline_in.add(candidateClip.duration) &&
                    !(candidateClip.timeline_in.add(candidateClip.duration) <=
                      clipStart) &&
                    !(clipEnd <= candidateClip.timeline_in)) {
                    piece = &candidateClip;
                    break;
                }
            }
            if (piece) break;
        }
        if (!piece) {
            Fail(EditError::InvalidTimelineIn,
                 "split position must be strictly inside the clip", error,
                 message);
            return false;
        }
        Operation cut = TimelineCutOperation(candidate, *piece, position);
        Operation ignored = RemoveClipOperation{};
        bool cutApplied = false;
        if (auto* linked = std::get_if<SplitLinkedClipsOperation>(&cut)) {
            cutApplied =
                ApplySplitLinked(candidate, *linked, ignored, error, message);
        } else {
            auto& single = std::get<SplitClipOperation>(cut);
            cutApplied = ApplySplit(candidate, single, ignored, error, message);
        }
        if (!cutApplied) return false;
    }

    if (!ValidateResult(candidate, error, message)) return false;
    operation.exact_track_result = snapshots(trackIds);
    inverse = SplitClipAtPositionsOperation{operation.clip_id,
                                            operation.positions, before};
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

void WriteColorManagement(std::ostringstream& output,
                          const ColorManagementSettings& settings) {
    output << "{\"enabled\":" << (settings.enabled ? "true" : "false")
           << ",\"input_gamut\":";
    WriteString(output, settings.input_gamut);
    output << ",\"input_transfer\":";
    WriteString(output, settings.input_transfer);
    output << ",\"input_ycbcr_matrix\":";
    WriteString(output, settings.input_ycbcr_matrix);
    output << ",\"input_range\":";
    WriteString(output, settings.input_range);
    output << ",\"working_gamut\":";
    WriteString(output, settings.working_gamut);
    output << ",\"output_gamut\":";
    WriteString(output, settings.output_gamut);
    output << ",\"output_transfer\":";
    WriteString(output, settings.output_transfer);
    output << '}';
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

void WriteEffectParamValue(std::ostringstream& output,
                           const EffectParamValue& value) {
    output << "{\"num\":" << value.num << ",\"den\":" << value.den << "}";
}

void WriteClipEffects(std::ostringstream& output,
                      const std::vector<ClipEffect>& effects) {
    output << '[';
    for (size_t index = 0; index < effects.size(); ++index) {
        if (index) output << ',';
        const ClipEffect& effect = effects[index];
        output << "{\"id\":\"" << effect.id << "\",\"type\":";
        WriteString(output, effect.type);
        output << ",\"params\":{";
        size_t paramIndex = 0;
        for (const auto& param : effect.params) {
            if (paramIndex++) output << ',';
            WriteString(output, param.first);
            output << ':';
            WriteEffectParamValue(output, param.second);
        }
        output << "}}";
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
            output << ",\"effects\":";
            WriteClipEffects(output, clip.effects);
            if (!clip.caption_group_id.empty()) {
                output << ",\"caption_group_id\":\"" << clip.caption_group_id
                       << '"';
            }
            if (!clip.caption_text.empty()) {
                output << ",\"caption_text\":";
                WriteString(output, clip.caption_text);
            }
            output << ",\"opacity\":";
            WriteEffectParamValue(output, clip.opacity);
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

ColorManagementSettings ReadColorManagement(Reader& reader) {
    ColorManagementSettings settings;
    reader.Expect("{\"enabled\":");
    settings.enabled = reader.Consume("true");
    if (!settings.enabled) reader.Expect("false");
    reader.Expect(",\"input_gamut\":");
    settings.input_gamut = reader.String();
    reader.Expect(",\"input_transfer\":");
    settings.input_transfer = reader.String();
    reader.Expect(",\"input_ycbcr_matrix\":");
    settings.input_ycbcr_matrix = reader.String();
    reader.Expect(",\"input_range\":");
    settings.input_range = reader.String();
    reader.Expect(",\"working_gamut\":");
    settings.working_gamut = reader.String();
    reader.Expect(",\"output_gamut\":");
    settings.output_gamut = reader.String();
    reader.Expect(",\"output_transfer\":");
    settings.output_transfer = reader.String();
    reader.Expect("}");
    return settings;
}

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

EffectParamValue ReadEffectParamValue(Reader& reader) {
    reader.Expect("{\"num\":");
    const int64_t num = reader.Integer();
    reader.Expect(",\"den\":");
    const int64_t den = reader.Integer();
    reader.Expect("}");
    if (num < std::numeric_limits<int32_t>::min() ||
        num > std::numeric_limits<int32_t>::max() ||
        den < std::numeric_limits<int32_t>::min() ||
        den > std::numeric_limits<int32_t>::max()) {
        throw std::runtime_error("EffectParamValue outside int32_t range");
    }
    return {static_cast<int32_t>(num), static_cast<int32_t>(den)};
}

std::vector<ClipEffect> ReadClipEffects(Reader& reader) {
    std::vector<ClipEffect> effects;
    reader.Expect("[");
    if (reader.Consume("]")) return effects;
    while (true) {
        ClipEffect effect;
        reader.Expect("{\"id\":");
        effect.id = reader.String();
        reader.Expect(",\"type\":");
        effect.type = reader.String();
        reader.Expect(",\"params\":{");
        if (!reader.Consume("}")) {
            while (true) {
                const std::string key = reader.String();
                reader.Expect(":");
                effect.params.emplace(key, ReadEffectParamValue(reader));
                if (reader.Consume("}")) break;
                reader.Expect(",");
            }
        }
        reader.Expect("}");
        effects.push_back(std::move(effect));
        if (reader.Consume("]")) return effects;
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
                reader.Expect(",\"effects\":");
                clip.effects = ReadClipEffects(reader);
                if (reader.Consume(",\"caption_group_id\":")) {
                    clip.caption_group_id = reader.String();
                }
                if (reader.Consume(",\"caption_text\":")) {
                    clip.caption_text = reader.String();
                }
                // ALPHA-2026-08 -- exact clip snapshots written before the
                // opacity field existed remain valid and mean fully opaque.
                if (reader.Consume(",\"opacity\":"))
                    clip.opacity = ReadEffectParamValue(reader);
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

// Removes the clip's transcript-derived ranges and ripple-closes each cut.
//
// Fragment placement rule (the one explicit rounding-adjacent decision this
// operation makes, documented once here rather than left implicit):
//   - The clip is cut into the kept fragments that remain outside every
//     removed range, in source order.
//   - The first kept fragment keeps the original clip's timeline_in exactly
//     -- if the very first range starts at the clip's own source_in (a head
//     removal), this is exactly an ordinary head ripple trim: nothing before
//     the clip moves.
//   - Every following kept fragment starts `gap_padding` after the previous
//     kept fragment's end. Padding therefore only ever appears *between two
//     fragments of this same clip* (an interior cut); it is never inserted
//     at the clip's own head or tail edge, where there is no fragment on the
//     other side to pad against -- a tail removal ripples the same way an
//     ordinary tail ripple trim does, with no trailing gap.
//   - Everything after the clip's original end -- on its own track, and on
//     every track in sync_track_ids -- shifts by exactly
//     (new total span) - (original total span), so downstream material
//     closes up around whatever combination of real cuts and kept padding
//     resulted.
bool ApplyRemoveWords(Document& candidate, RemoveWordsOperation& operation,
                      Operation& inverse, EditError& error,
                      std::string& message) {
    std::vector<Ulid> trackIds;
    const auto addTrack = [&](const Ulid& id) {
        if (!id.empty() &&
            std::find(trackIds.begin(), trackIds.end(), id) == trackIds.end())
            trackIds.push_back(id);
    };
    const auto snapshots = [&](const std::vector<Ulid>& ids) {
        std::vector<ExactTrackState> states;
        for (const Ulid& id : ids) {
            const DocumentTrack* track = candidate.FindTrack(id);
            if (track) states.push_back({id, track->clips});
        }
        return states;
    };

    if (!operation.exact_track_result.empty()) {
        for (const ExactTrackState& state : operation.exact_track_result)
            addTrack(state.track_id);
        const std::vector<ExactTrackState> before = snapshots(trackIds);
        if (before.size() != operation.exact_track_result.size()) {
            Fail(EditError::UnknownTrack,
                 "exact word removal state references an unknown track", error,
                 message);
            return false;
        }
        for (const ExactTrackState& state : operation.exact_track_result)
            candidate.FindTrack(state.track_id)->clips = state.clips;
        if (!ValidateResult(candidate, error, message)) return false;
        inverse = RemoveWordsOperation{
            operation.clip_id,        operation.ranges,
            operation.gap_padding,    operation.linked_clip_ids,
            operation.sync_track_ids, before};
        return true;
    }

    if (operation.ranges.empty()) {
        Fail(EditError::InvalidOperation,
             "RemoveWords requires at least one range", error, message);
        return false;
    }
    if (operation.gap_padding.rate <= 0 || operation.gap_padding.value < 0) {
        Fail(EditError::ArithmeticError,
             "RemoveWords gap_padding must be a non-negative exact duration",
             error, message);
        return false;
    }
    for (size_t index = 0; index < operation.ranges.size(); ++index) {
        const WordRemovalRange& range = operation.ranges[index];
        if (range.source_start.rate <= 0 || range.source_end.rate <= 0 ||
            range.source_start >= range.source_end) {
            Fail(EditError::InvalidOperation,
                 "RemoveWords range must have a positive duration", error,
                 message);
            return false;
        }
        if (index > 0 &&
            range.source_start < operation.ranges[index - 1].source_end) {
            Fail(EditError::InvalidOperation,
                 "RemoveWords ranges must be sorted and non-overlapping", error,
                 message);
            return false;
        }
    }

    // The anchor, plus every linked clip named alongside it. Each is cut by
    // the same source ranges and ripple-closed on its own track, inside one
    // operation. An A/V pair has to lose the same frames of picture and
    // sound or it drifts apart, and splitting the cleanup into two
    // operations would make one editorial gesture two undo steps.
    std::vector<Ulid> cutClipIds;
    cutClipIds.push_back(operation.clip_id);
    for (const Ulid& linkedId : operation.linked_clip_ids) {
        if (std::find(cutClipIds.begin(), cutClipIds.end(), linkedId) !=
            cutClipIds.end()) {
            Fail(EditError::DuplicateId,
                 "RemoveWords linked_clip_ids repeats '" + linkedId + "'",
                 error, message);
            return false;
        }
        cutClipIds.push_back(linkedId);
    }

    // Every clip is resolved and validated before any of them is touched, so
    // a partially applied cut is not a state this function can leave behind.
    struct CutTarget {
        DocumentTrack* track = nullptr;
        DocumentClip original;
    };
    std::vector<CutTarget> targets;
    for (const Ulid& cutId : cutClipIds) {
        DocumentTrack* cutTrack = candidate.FindTrackForClip(cutId);
        if (!cutTrack) {
            Fail(EditError::UnknownClip, "unknown clip_id '" + cutId + "'",
                 error, message);
            return false;
        }
        const auto found = std::find_if(
            cutTrack->clips.begin(), cutTrack->clips.end(),
            [&](const DocumentClip& clip) { return clip.id == cutId; });
        const DocumentClip original = *found;
        const RationalTime sourceEnd =
            original.source_in.add(original.duration);
        for (const WordRemovalRange& range : operation.ranges) {
            if (range.source_start < original.source_in ||
                range.source_end > sourceEnd) {
                Fail(EditError::SourceOutOfBounds,
                     "RemoveWords range falls outside the source range of "
                     "clip_id '" +
                         cutId + "'",
                     error, message);
                return false;
            }
        }
        targets.push_back({cutTrack, original});
        addTrack(cutTrack->id);
    }
    for (const Ulid& id : operation.sync_track_ids) {
        if (!candidate.FindTrack(id)) {
            Fail(EditError::UnknownTrack,
                 "unknown RemoveWords sync track_id '" + id + "'", error,
                 message);
            return false;
        }
        addTrack(id);
    }
    const std::vector<ExactTrackState> before = snapshots(trackIds);

    struct Fragment {
        RationalTime source_start;
        RationalTime source_end;
    };
    std::vector<DocumentClip> newClips;
    RationalTime anchorOriginalEnd{0, 1};
    RationalTime anchorShiftDelta{0, 1};
    for (size_t targetIndex = 0; targetIndex < targets.size(); ++targetIndex) {
        DocumentTrack* cutTrack = targets[targetIndex].track;
        const DocumentClip original = targets[targetIndex].original;
        const RationalTime clipSourceStart = original.source_in;
        const RationalTime clipSourceEnd =
            original.source_in.add(original.duration);

        std::vector<Fragment> fragments;
        RationalTime cursor = clipSourceStart;
        for (const WordRemovalRange& range : operation.ranges) {
            if (range.source_start > cursor)
                fragments.push_back({cursor, range.source_start});
            cursor = range.source_end;
        }
        if (cursor < clipSourceEnd)
            fragments.push_back({cursor, clipSourceEnd});

        std::vector<DocumentClip> targetClips;
        RationalTime timelineCursor = original.timeline_in;
        for (size_t fragmentIndex = 0; fragmentIndex < fragments.size();
             ++fragmentIndex) {
            if (fragmentIndex > 0)
                timelineCursor = timelineCursor.add(operation.gap_padding);
            DocumentClip fragment = original;
            fragment.id = fragmentIndex == 0 ? original.id : GenerateUlid();
            fragment.source_in = fragments[fragmentIndex].source_start;
            fragment.duration = fragments[fragmentIndex].source_end.sub(
                fragments[fragmentIndex].source_start);
            fragment.timeline_in = timelineCursor;
            timelineCursor = timelineCursor.add(fragment.duration);
            targetClips.push_back(std::move(fragment));
        }
        const RationalTime newEnd =
            fragments.empty() ? original.timeline_in : timelineCursor;
        const RationalTime originalEnd =
            original.timeline_in.add(original.duration);
        const RationalTime shiftDelta = newEnd.sub(originalEnd);
        if (targetIndex == 0) {
            anchorOriginalEnd = originalEnd;
            anchorShiftDelta = shiftDelta;
        }

        // Re-found rather than remembered: an earlier target may already have
        // spliced this same track, which moves every index after it.
        const auto position = std::find_if(
            cutTrack->clips.begin(), cutTrack->clips.end(),
            [&](const DocumentClip& clip) { return clip.id == original.id; });
        if (position == cutTrack->clips.end()) {
            Fail(EditError::UnknownClip,
                 "RemoveWords lost clip_id '" + original.id + "' while cutting",
                 error, message);
            return false;
        }
        const size_t positionIndex = static_cast<size_t>(
            std::distance(cutTrack->clips.begin(), position));
        for (size_t next = positionIndex + 1; next < cutTrack->clips.size();
             ++next)
            cutTrack->clips[next].timeline_in =
                cutTrack->clips[next].timeline_in.add(shiftDelta);
        cutTrack->clips.erase(cutTrack->clips.begin() +
                              static_cast<std::ptrdiff_t>(positionIndex));
        cutTrack->clips.insert(cutTrack->clips.begin() +
                                   static_cast<std::ptrdiff_t>(positionIndex),
                               targetClips.begin(), targetClips.end());
        newClips.insert(newClips.end(), targetClips.begin(), targetClips.end());
    }

    // Sync tracks shift by the anchor's delta. A track that was itself cut
    // above has already closed up around its own fragments, so shifting it
    // again here would move it twice.
    for (const Ulid& id : operation.sync_track_ids) {
        const bool alreadyCut = std::any_of(
            targets.begin(), targets.end(),
            [&](const CutTarget& target) { return target.track->id == id; });
        if (alreadyCut) continue;
        DocumentTrack* syncTrack = candidate.FindTrack(id);
        for (DocumentClip& clip : syncTrack->clips)
            if (clip.timeline_in >= anchorOriginalEnd)
                clip.timeline_in = clip.timeline_in.add(anchorShiftDelta);
    }

    for (const DocumentClip& fragment : newClips) {
        const DocumentSource* source = candidate.FindSource(fragment.source_id);
        if (!source) {
            Fail(EditError::UnknownSource,
                 "RemoveWords clip references an unknown source", error,
                 message);
            return false;
        }
        if (!ValidateSourceRange(*source, fragment.source_in, fragment.duration,
                                 error, message)) {
            return false;
        }
    }
    if (!ValidateResult(candidate, error, message)) return false;
    operation.exact_track_result = snapshots(trackIds);
    inverse = RemoveWordsOperation{
        operation.clip_id,        operation.ranges,
        operation.gap_padding,    operation.linked_clip_ids,
        operation.sync_track_ids, before};
    return true;
}

}  // namespace

bool ResolveIntervalSplits(const Document& document, const Ulid& clipId,
                           const RationalTime& interval,
                           SplitClipAtPositionsOperation& operation,
                           std::string& error) {
    operation = SplitClipAtPositionsOperation{};
    if (interval.rate <= 0 || interval.value <= 0) {
        error = "interval must be a positive duration";
        return false;
    }
    const DocumentClip* clip = document.FindClip(clipId);
    if (!clip) {
        error = "no clip matches id '" + clipId + "'";
        return false;
    }
    const RationalTime start = clip->timeline_in;
    const RationalTime end = start.add(clip->duration);
    // Accumulate by repeated exact addition rather than multiplying a
    // converted frame count: RationalTime::add is the only thing in this
    // codebase allowed to reconcile two timebases, and going through it once
    // per step keeps every position exact even when the interval's rate and
    // the clip's disagree.
    RationalTime position = start.add(interval);
    while (position < end) {
        operation.positions.push_back(position);
        position = position.add(interval);
    }
    if (operation.positions.empty()) {
        error = "interval is longer than the clip; nothing to cut";
        return false;
    }
    operation.clip_id = clipId;
    return true;
}

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
        case EditError::UnknownTransition:
            return "UnknownTransition";
        case EditError::UnknownCaptionStyle:
            return "UnknownCaptionStyle";
        case EditError::UnknownMulticamGroup:
            return "UnknownMulticamGroup";
        case EditError::UnknownMulticamAngle:
            return "UnknownMulticamAngle";
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
        case EditError::LockedTrack:
            return "LockedTrack";
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
        if (!std::holds_alternative<SetTrackLockOperation>(normalized) &&
            !std::holds_alternative<SetTrackSyncLockOperation>(normalized) &&
            !std::holds_alternative<SetTrackOutputOperation>(normalized)) {
            if (const DocumentTrack* locked =
                    LockedTrackTouchedBy(document, normalized)) {
                Fail(EditError::LockedTrack,
                     "track_id '" + locked->id + "' is locked", error, message);
                return false;
            }
        }
        bool applied = false;
        if (auto* insert = std::get_if<InsertClipOperation>(&normalized)) {
            applied = ApplyInsert(candidate, *insert, generatedInverse, error,
                                  message);
        } else if (auto* remove =
                       std::get_if<RemoveClipOperation>(&normalized)) {
            applied = ApplyRemove(candidate, *remove, generatedInverse, error,
                                  message);
        } else if (auto* clear = std::get_if<ClearClipOperation>(&normalized)) {
            applied =
                ApplyClear(candidate, *clear, generatedInverse, error, message);
        } else if (auto* clear =
                       std::get_if<ClearClipsOperation>(&normalized)) {
            applied = ApplyClearClips(candidate, *clear, generatedInverse,
                                      error, message);
        } else if (auto* paste =
                       std::get_if<PasteClipsOperation>(&normalized)) {
            applied = ApplyPasteClips(candidate, *paste, generatedInverse,
                                      error, message);
        } else if (auto* trim = std::get_if<TrimClipOperation>(&normalized)) {
            applied =
                ApplyTrim(candidate, *trim, generatedInverse, error, message);
        } else if (auto* move = std::get_if<MoveClipOperation>(&normalized)) {
            applied =
                ApplyMove(candidate, *move, generatedInverse, error, message);
        } else if (auto* moves = std::get_if<MoveClipsOperation>(&normalized)) {
            applied = ApplyMoveClips(candidate, *moves, generatedInverse, error,
                                     message);
        } else if (auto* linkedMove =
                       std::get_if<MoveLinkedClipsOperation>(&normalized)) {
            applied = ApplyMoveLinked(candidate, *linkedMove, generatedInverse,
                                      error, message);
        } else if (auto* linkedTrim =
                       std::get_if<TrimLinkedClipsOperation>(&normalized)) {
            applied = ApplyTrimLinked(candidate, *linkedTrim, generatedInverse,
                                      error, message);
        } else if (auto* rippleTrim =
                       std::get_if<RippleTrimOperation>(&normalized)) {
            applied = ApplyRippleTrim(candidate, *rippleTrim, generatedInverse,
                                      error, message);
        } else if (auto* rollEdit =
                       std::get_if<RollEditOperation>(&normalized)) {
            applied = ApplyRollEdit(candidate, *rollEdit, generatedInverse,
                                    error, message);
        } else if (auto* slipEdit =
                       std::get_if<SlipEditOperation>(&normalized)) {
            applied = ApplySlipEdit(candidate, *slipEdit, generatedInverse,
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
        } else if (auto* intervalSplit =
                       std::get_if<SplitClipAtPositionsOperation>(
                           &normalized)) {
            applied = ApplySplitAtPositions(candidate, *intervalSplit,
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
        } else if (auto* setTrackLock =
                       std::get_if<SetTrackLockOperation>(&normalized)) {
            applied = ApplySetTrackLock(candidate, *setTrackLock,
                                        generatedInverse, error, message);
        } else if (auto* setSyncLock =
                       std::get_if<SetTrackSyncLockOperation>(&normalized)) {
            applied = ApplySetTrackSyncLock(candidate, *setSyncLock,
                                            generatedInverse, error, message);
        } else if (auto* setOutput =
                       std::get_if<SetTrackOutputOperation>(&normalized)) {
            applied = ApplySetTrackOutput(candidate, *setOutput,
                                          generatedInverse, error, message);
        } else if (auto* updateSequence =
                       std::get_if<UpdateSequenceOperation>(&normalized)) {
            applied = ApplyUpdateSequence(candidate, *updateSequence,
                                          generatedInverse, error, message);
        } else if (auto* setColor =
                       std::get_if<SetColorManagementOperation>(&normalized)) {
            applied = ApplySetColorManagement(candidate, *setColor,
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
        } else if (auto* moveBin = std::get_if<MoveBinOperation>(&normalized)) {
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
        } else if (auto* addTransition =
                       std::get_if<AddTransitionOperation>(&normalized)) {
            applied = ApplyAddTransition(candidate, *addTransition,
                                         generatedInverse, error, message);
        } else if (auto* removeTransition =
                       std::get_if<RemoveTransitionOperation>(&normalized)) {
            applied = ApplyRemoveTransition(candidate, *removeTransition,
                                            generatedInverse, error, message);
        } else if (auto* updateTransition =
                       std::get_if<UpdateTransitionOperation>(&normalized)) {
            applied = ApplyUpdateTransition(candidate, *updateTransition,
                                            generatedInverse, error, message);
        } else if (auto* setClipLink =
                       std::get_if<SetClipLinkOperation>(&normalized)) {
            applied = ApplySetClipLink(candidate, *setClipLink,
                                       generatedInverse, error, message);
        } else if (auto* join = std::get_if<JoinClipOperation>(&normalized)) {
            applied =
                ApplyJoin(candidate, *join, generatedInverse, error, message);
        } else if (auto* setEffects =
                       std::get_if<SetClipEffectsOperation>(&normalized)) {
            applied = ApplySetClipEffects(candidate, *setEffects,
                                          generatedInverse, error, message);
        } else if (auto* setOpacity =
                       std::get_if<SetClipOpacityOperation>(&normalized)) {
            applied = ApplySetClipOpacity(candidate, *setOpacity,
                                          generatedInverse, error, message);
        } else if (auto* addCaptionStyle =
                       std::get_if<AddCaptionStyleOperation>(&normalized)) {
            applied = ApplyAddCaptionStyle(candidate, *addCaptionStyle,
                                           generatedInverse, error, message);
        } else if (auto* removeCaptionStyle =
                       std::get_if<RemoveCaptionStyleOperation>(&normalized)) {
            applied = ApplyRemoveCaptionStyle(candidate, *removeCaptionStyle,
                                              generatedInverse, error, message);
        } else if (auto* setCaption =
                       std::get_if<SetClipCaptionOperation>(&normalized)) {
            applied = ApplySetClipCaption(candidate, *setCaption,
                                          generatedInverse, error, message);
        } else if (auto* addMulticam =
                       std::get_if<AddMulticamGroupOperation>(&normalized)) {
            applied = ApplyAddMulticamGroup(candidate, *addMulticam,
                                            generatedInverse, error, message);
        } else if (auto* removeMulticam =
                       std::get_if<RemoveMulticamGroupOperation>(&normalized)) {
            applied = ApplyRemoveMulticamGroup(
                candidate, *removeMulticam, generatedInverse, error, message);
        } else if (auto* setActiveAngle =
                       std::get_if<SetMulticamActiveAngleOperation>(
                           &normalized)) {
            applied = ApplySetMulticamActiveAngle(
                candidate, *setActiveAngle, generatedInverse, error, message);
        } else if (auto* removeWords =
                       std::get_if<RemoveWordsOperation>(&normalized)) {
            applied = ApplyRemoveWords(candidate, *removeWords,
                                       generatedInverse, error, message);
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
    } else if (const auto* clear =
                   std::get_if<ClearClipsOperation>(&operation)) {
        output << "{\"type\":\"ClearClips\",\"clip_ids\":[";
        for (size_t index = 0; index < clear->clip_ids.size(); ++index) {
            if (index) output << ',';
            WriteString(output, clear->clip_ids[index]);
        }
        output << "],\"exact_tracks\":";
        WriteExactTracks(output, clear->exact_track_result);
        output << '}';
    } else if (const auto* paste =
                   std::get_if<PasteClipsOperation>(&operation)) {
        output << "{\"type\":\"PasteClips\",\"clips\":[";
        for (size_t index = 0; index < paste->clips.size(); ++index) {
            if (index) output << ',';
            const PastedClip& clip = paste->clips[index];
            output << "{\"copied_clip_id\":";
            WriteString(output, clip.copied_clip_id);
            output << ",\"track_id\":";
            WriteString(output, clip.track_id);
            output << ",\"source_id\":";
            WriteString(output, clip.source_id);
            output << ",\"source_in\":";
            WriteTime(output, clip.source_in);
            output << ",\"duration\":";
            WriteTime(output, clip.duration);
            output << ",\"timeline_in\":";
            WriteTime(output, clip.timeline_in);
            output << ",\"clip_id\":";
            WriteString(output, clip.clip_id);
            output << ",\"copied_link_group_id\":";
            WriteString(output, clip.copied_link_group_id);
            output << ",\"link_group_id\":";
            WriteString(output, clip.link_group_id);
            output << ",\"copied_sync_anchor_clip_id\":";
            WriteString(output, clip.copied_sync_anchor_clip_id);
            output << ",\"sync_anchor_clip_id\":";
            WriteString(output, clip.sync_anchor_clip_id);
            output << ",\"sync_reference_delta\":";
            WriteTime(output, clip.sync_reference_delta);
            output << '}';
        }
        output << "],\"exact_tracks\":";
        WriteExactTracks(output, paste->exact_track_result);
        output << ",\"overwrite\":" << (paste->overwrite ? "true" : "false");
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
    } else if (const auto* moves =
                   std::get_if<MoveClipsOperation>(&operation)) {
        output << "{\"type\":\"MoveClips\",\"moves\":[";
        for (size_t index = 0; index < moves->moves.size(); ++index) {
            if (index) output << ',';
            const LinkedClipMove& move = moves->moves[index];
            output << "{\"clip_id\":\"" << move.clip_id << "\",\"track_id\":\""
                   << move.track_id << "\",\"timeline_in\":";
            WriteTime(output, move.timeline_in);
            output << '}';
        }
        output << "],\"exact_tracks\":";
        WriteExactTracks(output, moves->exact_track_result);
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
    } else if (const auto* ripple =
                   std::get_if<RippleTrimOperation>(&operation)) {
        output << "{\"type\":\"RippleTrim\",\"clip_id\":\"" << ripple->clip_id
               << "\",\"edge\":\""
               << (ripple->edge == TrimEdge::Head ? "Head" : "Tail")
               << "\",\"delta\":";
        WriteTime(output, ripple->delta);
        output << ",\"linked_clip_ids\":[";
        for (size_t index = 0; index < ripple->linked_clip_ids.size();
             ++index) {
            if (index) output << ',';
            WriteString(output, ripple->linked_clip_ids[index]);
        }
        output << "],\"sync_track_ids\":[";
        for (size_t index = 0; index < ripple->sync_track_ids.size(); ++index) {
            if (index) output << ',';
            WriteString(output, ripple->sync_track_ids[index]);
        }
        output << "],\"exact_tracks\":";
        WriteExactTracks(output, ripple->exact_track_result);
        output << '}';
    } else if (const auto* roll = std::get_if<RollEditOperation>(&operation)) {
        output << "{\"type\":\"RollEdit\",\"pairs\":[";
        for (size_t index = 0; index < roll->pairs.size(); ++index) {
            if (index) output << ',';
            output << "{\"left_clip_id\":\"" << roll->pairs[index].left_clip_id
                   << "\",\"right_clip_id\":\""
                   << roll->pairs[index].right_clip_id << "\"}";
        }
        output << "],\"delta\":";
        WriteTime(output, roll->delta);
        output << ",\"exact_tracks\":";
        WriteExactTracks(output, roll->exact_track_result);
        output << '}';
    } else if (const auto* slip = std::get_if<SlipEditOperation>(&operation)) {
        output << "{\"type\":\"SlipEdit\",\"clip_ids\":[";
        for (size_t index = 0; index < slip->clip_ids.size(); ++index) {
            if (index) output << ',';
            WriteString(output, slip->clip_ids[index]);
        }
        output << "],\"delta\":";
        WriteTime(output, slip->delta);
        output << ",\"exact_tracks\":";
        WriteExactTracks(output, slip->exact_track_result);
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
        output << ",\"right_clip_id\":\"" << split->right_clip_id
               << "\",\"right_effect_ids\":[";
        for (size_t index = 0; index < split->right_effect_ids.size();
             ++index) {
            if (index) output << ',';
            WriteString(output, split->right_effect_ids[index]);
        }
        output << "]}";
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
            for (size_t index = 0; index < gap->linked_track_ids.size();
                 ++index) {
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
               << "\",\"index\":" << addTrack->index
               << ",\"locked\":" << (addTrack->locked ? "true" : "false")
               << ",\"sync_lock\":" << (addTrack->sync_lock ? "true" : "false")
               << ",\"visible\":" << (addTrack->visible ? "true" : "false")
               << ",\"muted\":" << (addTrack->muted ? "true" : "false")
               << ",\"solo\":" << (addTrack->solo ? "true" : "false");
        if (!addTrack->clips.empty()) {
            output << ",\"exact_tracks\":";
            WriteExactTracks(output, {{addTrack->track_id, addTrack->clips}});
        }
        output << '}';
    } else if (const auto* removeTrack =
                   std::get_if<RemoveTrackOperation>(&operation)) {
        output << "{\"type\":\"RemoveTrack\",\"track_id\":\""
               << removeTrack->track_id << "\"}";
    } else if (const auto* setTrackLock =
                   std::get_if<SetTrackLockOperation>(&operation)) {
        output << "{\"type\":\"SetTrackLock\",\"track_id\":\""
               << setTrackLock->track_id
               << "\",\"locked\":" << (setTrackLock->locked ? "true" : "false")
               << '}';
    } else if (const auto* setSyncLock =
                   std::get_if<SetTrackSyncLockOperation>(&operation)) {
        output << "{\"type\":\"SetTrackSyncLock\",\"track_id\":\""
               << setSyncLock->track_id << "\",\"sync_lock\":"
               << (setSyncLock->sync_lock ? "true" : "false") << '}';
    } else if (const auto* setOutput =
                   std::get_if<SetTrackOutputOperation>(&operation)) {
        output << "{\"type\":\"SetTrackOutput\",\"track_id\":\""
               << setOutput->track_id
               << "\",\"visible\":" << (setOutput->visible ? "true" : "false")
               << ",\"muted\":" << (setOutput->muted ? "true" : "false")
               << ",\"solo\":" << (setOutput->solo ? "true" : "false") << '}';
    } else if (const auto* updateSequence =
                   std::get_if<UpdateSequenceOperation>(&operation)) {
        output << "{\"type\":\"UpdateSequence\",\"sequence_id\":\""
               << updateSequence->sequence_id << "\",\"name\":";
        WriteString(output, updateSequence->name);
        output << ",\"width\":" << updateSequence->width
               << ",\"height\":" << updateSequence->height
               << ",\"frame_rate\":{\"num\":" << updateSequence->frame_rate.num
               << ",\"den\":" << updateSequence->frame_rate.den << "}}";
    } else if (const auto* setColor =
                   std::get_if<SetColorManagementOperation>(&operation)) {
        output << "{\"type\":\"SetColorManagement\",\"settings\":";
        WriteColorManagement(output, setColor->settings);
        output << '}';
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
        output << "},\"insertion_index\":" << addMarker->insertion_index << '}';
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
    } else if (const auto* addTransition =
                   std::get_if<AddTransitionOperation>(&operation)) {
        const DocumentTransition& value = addTransition->transition;
        const char* alignment =
            value.alignment == TransitionAlignment::Center ? "center"
            : value.alignment == TransitionAlignment::StartAtCut
                ? "start_at_cut"
                : "end_at_cut";
        output << "{\"type\":\"AddTransition\",\"transition\":{\"id\":";
        WriteString(output, value.id);
        output << ",\"track_id\":";
        WriteString(output, value.track_id);
        output << ",\"left_clip_id\":";
        WriteString(output, value.left_clip_id);
        output << ",\"right_clip_id\":";
        WriteString(output, value.right_clip_id);
        output << ",\"transition_type\":";
        WriteString(output, value.type);
        output << ",\"duration\":";
        WriteTime(output, value.duration);
        output << ",\"alignment\":";
        WriteString(output, alignment);
        output << "},\"insertion_index\":" << addTransition->insertion_index
               << '}';
    } else if (const auto* removeTransition =
                   std::get_if<RemoveTransitionOperation>(&operation)) {
        output << "{\"type\":\"RemoveTransition\",\"transition_id\":";
        WriteString(output, removeTransition->transition_id);
        output << '}';
    } else if (const auto* updateTransition =
                   std::get_if<UpdateTransitionOperation>(&operation)) {
        const char* alignment =
            updateTransition->alignment == TransitionAlignment::Center
                ? "center"
            : updateTransition->alignment == TransitionAlignment::StartAtCut
                ? "start_at_cut"
                : "end_at_cut";
        output << "{\"type\":\"UpdateTransition\",\"transition_id\":";
        WriteString(output, updateTransition->transition_id);
        output << ",\"transition_type\":";
        WriteString(output, updateTransition->type);
        output << ",\"duration\":";
        WriteTime(output, updateTransition->duration);
        output << ",\"alignment\":";
        WriteString(output, alignment);
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
    } else if (const auto* setEffects =
                   std::get_if<SetClipEffectsOperation>(&operation)) {
        output << "{\"type\":\"SetClipEffects\",\"clip_id\":\""
               << setEffects->clip_id << "\",\"effects\":";
        WriteClipEffects(output, setEffects->effects);
        output << '}';
    } else if (const auto* setOpacity =
                   std::get_if<SetClipOpacityOperation>(&operation)) {
        output << "{\"type\":\"SetClipOpacity\",\"clip_id\":\""
               << setOpacity->clip_id << "\",\"opacity\":";
        WriteEffectParamValue(output, setOpacity->opacity);
        output << '}';
    } else if (const auto* addCaptionStyle =
                   std::get_if<AddCaptionStyleOperation>(&operation)) {
        output << "{\"type\":\"AddCaptionStyle\",\"style\":{\"id\":\""
               << addCaptionStyle->style.id << "\",\"font_family\":";
        WriteString(output, addCaptionStyle->style.font_family);
        output << ",\"font_size\":" << addCaptionStyle->style.font_size
               << ",\"color\":";
        WriteString(output, addCaptionStyle->style.color);
        output << ",\"position\":";
        WriteString(output, addCaptionStyle->style.position);
        output << "},\"insertion_index\":" << addCaptionStyle->insertion_index
               << '}';
    } else if (const auto* removeCaptionStyle =
                   std::get_if<RemoveCaptionStyleOperation>(&operation)) {
        output << "{\"type\":\"RemoveCaptionStyle\",\"style_id\":\""
               << removeCaptionStyle->style_id << "\"}";
    } else if (const auto* setCaption =
                   std::get_if<SetClipCaptionOperation>(&operation)) {
        output << "{\"type\":\"SetClipCaption\",\"clip_id\":\""
               << setCaption->clip_id << "\",\"caption_group_id\":\""
               << setCaption->caption_group_id << "\",\"caption_text\":";
        WriteString(output, setCaption->caption_text);
        output << '}';
    } else if (const auto* addMulticam =
                   std::get_if<AddMulticamGroupOperation>(&operation)) {
        const MulticamGroup& value = addMulticam->group;
        output << "{\"type\":\"AddMulticamGroup\",\"group\":{\"id\":";
        WriteString(output, value.id);
        output << ",\"name\":";
        WriteString(output, value.name);
        output << ",\"angles\":[";
        for (size_t index = 0; index < value.angles.size(); ++index) {
            if (index) output << ',';
            const MulticamAngle& angle = value.angles[index];
            output << "{\"id\":";
            WriteString(output, angle.id);
            output << ",\"name\":";
            WriteString(output, angle.name);
            output << ",\"clip_id\":";
            WriteString(output, angle.clip_id);
            output << '}';
        }
        output << "],\"active_angle_id\":";
        WriteString(output, value.active_angle_id);
        output << "},\"insertion_index\":" << addMulticam->insertion_index
               << '}';
    } else if (const auto* removeMulticam =
                   std::get_if<RemoveMulticamGroupOperation>(&operation)) {
        output << "{\"type\":\"RemoveMulticamGroup\",\"group_id\":";
        WriteString(output, removeMulticam->group_id);
        output << '}';
    } else if (const auto* setActiveAngle =
                   std::get_if<SetMulticamActiveAngleOperation>(&operation)) {
        output << "{\"type\":\"SetMulticamActiveAngle\",\"group_id\":";
        WriteString(output, setActiveAngle->group_id);
        output << ",\"active_angle_id\":";
        WriteString(output, setActiveAngle->active_angle_id);
        output << '}';
    } else if (const auto* intervalSplit =
                   std::get_if<SplitClipAtPositionsOperation>(&operation)) {
        output << "{\"type\":\"SplitClipAtPositions\",\"clip_id\":";
        WriteString(output, intervalSplit->clip_id);
        output << ",\"positions\":[";
        for (size_t index = 0; index < intervalSplit->positions.size();
             ++index) {
            if (index) output << ',';
            WriteTime(output, intervalSplit->positions[index]);
        }
        output << "],\"exact_tracks\":";
        WriteExactTracks(output, intervalSplit->exact_track_result);
        output << '}';
    } else if (const auto* removeWords =
                   std::get_if<RemoveWordsOperation>(&operation)) {
        output << "{\"type\":\"RemoveWords\",\"clip_id\":\""
               << removeWords->clip_id << "\",\"ranges\":[";
        for (size_t index = 0; index < removeWords->ranges.size(); ++index) {
            if (index) output << ',';
            output << "{\"source_start\":";
            WriteTime(output, removeWords->ranges[index].source_start);
            output << ",\"source_end\":";
            WriteTime(output, removeWords->ranges[index].source_end);
            output << '}';
        }
        output << "],\"gap_padding\":";
        WriteTime(output, removeWords->gap_padding);
        output << ",\"linked_clip_ids\":[";
        for (size_t index = 0; index < removeWords->linked_clip_ids.size();
             ++index) {
            if (index) output << ',';
            WriteString(output, removeWords->linked_clip_ids[index]);
        }
        output << "],\"sync_track_ids\":[";
        for (size_t index = 0; index < removeWords->sync_track_ids.size();
             ++index) {
            if (index) output << ',';
            WriteString(output, removeWords->sync_track_ids[index]);
        }
        output << "],\"exact_tracks\":";
        WriteExactTracks(output, removeWords->exact_track_result);
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
        } else if (type == "ClearClips") {
            ClearClipsOperation value;
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
        } else if (type == "PasteClips") {
            PasteClipsOperation value;
            reader.Expect(",\"clips\":[");
            if (!reader.Consume("]")) {
                while (true) {
                    PastedClip clip;
                    reader.Expect("{\"copied_clip_id\":");
                    clip.copied_clip_id = reader.String();
                    reader.Expect(",\"track_id\":");
                    clip.track_id = reader.String();
                    reader.Expect(",\"source_id\":");
                    clip.source_id = reader.String();
                    reader.Expect(",\"source_in\":");
                    clip.source_in = ReadTime(reader);
                    reader.Expect(",\"duration\":");
                    clip.duration = ReadTime(reader);
                    reader.Expect(",\"timeline_in\":");
                    clip.timeline_in = ReadTime(reader);
                    reader.Expect(",\"clip_id\":");
                    clip.clip_id = reader.String();
                    reader.Expect(",\"copied_link_group_id\":");
                    clip.copied_link_group_id = reader.String();
                    reader.Expect(",\"link_group_id\":");
                    clip.link_group_id = reader.String();
                    reader.Expect(",\"copied_sync_anchor_clip_id\":");
                    clip.copied_sync_anchor_clip_id = reader.String();
                    reader.Expect(",\"sync_anchor_clip_id\":");
                    clip.sync_anchor_clip_id = reader.String();
                    reader.Expect(",\"sync_reference_delta\":");
                    clip.sync_reference_delta = ReadTime(reader);
                    reader.Expect("}");
                    value.clips.push_back(std::move(clip));
                    if (reader.Consume("]")) break;
                    reader.Expect(",");
                }
            }
            reader.Expect(",\"exact_tracks\":");
            value.exact_track_result = ReadExactTracks(reader);
            // PasteClips predates overwrite-at-playhead. Version-1 edit logs
            // without the appended field are insert-style pastes and must
            // remain readable after upgrading the application.
            if (!reader.Consume("}")) {
                reader.Expect(",\"overwrite\":");
                if (reader.Consume("true"))
                    value.overwrite = true;
                else {
                    reader.Expect("false");
                    value.overwrite = false;
                }
                reader.Expect("}");
            }
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
        } else if (type == "MoveClips") {
            MoveClipsOperation value;
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
        } else if (type == "RippleTrim") {
            RippleTrimOperation value;
            reader.Expect(",\"clip_id\":");
            value.clip_id = reader.String();
            reader.Expect(",\"edge\":");
            const std::string edge = reader.String();
            if (edge != "Head" && edge != "Tail")
                throw std::runtime_error("invalid ripple trim edge");
            value.edge = edge == "Head" ? TrimEdge::Head : TrimEdge::Tail;
            reader.Expect(",\"delta\":");
            value.delta = ReadTime(reader);
            reader.Expect(",\"linked_clip_ids\":[");
            if (!reader.Consume("]")) {
                while (true) {
                    value.linked_clip_ids.push_back(reader.String());
                    if (reader.Consume("]")) break;
                    reader.Expect(",");
                }
            }
            reader.Expect(",\"sync_track_ids\":[");
            if (!reader.Consume("]")) {
                while (true) {
                    value.sync_track_ids.push_back(reader.String());
                    if (reader.Consume("]")) break;
                    reader.Expect(",");
                }
            }
            reader.Expect(",\"exact_tracks\":");
            value.exact_track_result = ReadExactTracks(reader);
            reader.Expect("}");
            operation = std::move(value);
        } else if (type == "RollEdit") {
            RollEditOperation value;
            reader.Expect(",\"pairs\":[");
            if (!reader.Consume("]")) {
                while (true) {
                    RollEditPair pair;
                    reader.Expect("{\"left_clip_id\":");
                    pair.left_clip_id = reader.String();
                    reader.Expect(",\"right_clip_id\":");
                    pair.right_clip_id = reader.String();
                    reader.Expect("}");
                    value.pairs.push_back(std::move(pair));
                    if (reader.Consume("]")) break;
                    reader.Expect(",");
                }
            }
            reader.Expect(",\"delta\":");
            value.delta = ReadTime(reader);
            reader.Expect(",\"exact_tracks\":");
            value.exact_track_result = ReadExactTracks(reader);
            reader.Expect("}");
            operation = std::move(value);
        } else if (type == "SlipEdit") {
            SlipEditOperation value;
            reader.Expect(",\"clip_ids\":[");
            if (!reader.Consume("]")) {
                while (true) {
                    value.clip_ids.push_back(reader.String());
                    if (reader.Consume("]")) break;
                    reader.Expect(",");
                }
            }
            reader.Expect(",\"delta\":");
            value.delta = ReadTime(reader);
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
            // Optional: logs written before a split learned to carry the
            // clip's grade have no such field. Leaving it absent means the
            // same thing as an empty list -- no effect IDs assigned yet --
            // which ApplySplit fills in on the next application. Requiring
            // it here would make every project with a cut in its history
            // unreplayable, undo and redo included.
            if (reader.Consume(",\"right_effect_ids\":[")) {
                if (!reader.Consume("]")) {
                    while (true) {
                        value.right_effect_ids.push_back(reader.String());
                        if (reader.Consume("]")) break;
                        reader.Expect(",");
                    }
                }
            }
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
            if (reader.Consume(",\"locked\":")) {
                if (reader.Consume("true"))
                    value.locked = true;
                else {
                    reader.Expect("false");
                    value.locked = false;
                }
            }
            if (reader.Consume(",\"sync_lock\":")) {
                if (reader.Consume("true"))
                    value.sync_lock = true;
                else {
                    reader.Expect("false");
                    value.sync_lock = false;
                }
            }
            if (reader.Consume(",\"visible\":")) {
                value.visible = reader.Consume("true");
                if (!value.visible) reader.Expect("false");
            }
            if (reader.Consume(",\"muted\":")) {
                value.muted = reader.Consume("true");
                if (!value.muted) reader.Expect("false");
            }
            if (reader.Consume(",\"solo\":")) {
                value.solo = reader.Consume("true");
                if (!value.solo) reader.Expect("false");
            }
            if (!reader.Consume("}")) {
                reader.Expect(",\"exact_tracks\":");
                const auto tracks = ReadExactTracks(reader);
                if (tracks.size() != 1 ||
                    tracks.front().track_id != value.track_id)
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
        } else if (type == "SetTrackLock") {
            reader.Expect(",\"track_id\":");
            SetTrackLockOperation value;
            value.track_id = reader.String();
            reader.Expect(",\"locked\":");
            if (reader.Consume("true"))
                value.locked = true;
            else {
                reader.Expect("false");
                value.locked = false;
            }
            reader.Expect("}");
            operation = std::move(value);
        } else if (type == "SetTrackSyncLock") {
            reader.Expect(",\"track_id\":");
            SetTrackSyncLockOperation value;
            value.track_id = reader.String();
            reader.Expect(",\"sync_lock\":");
            if (reader.Consume("true"))
                value.sync_lock = true;
            else {
                reader.Expect("false");
                value.sync_lock = false;
            }
            reader.Expect("}");
            operation = std::move(value);
        } else if (type == "SetTrackOutput") {
            reader.Expect(",\"track_id\":");
            SetTrackOutputOperation value;
            value.track_id = reader.String();
            reader.Expect(",\"visible\":");
            value.visible = reader.Consume("true");
            if (!value.visible) reader.Expect("false");
            reader.Expect(",\"muted\":");
            value.muted = reader.Consume("true");
            if (!value.muted) reader.Expect("false");
            reader.Expect(",\"solo\":");
            value.solo = reader.Consume("true");
            if (!value.solo) reader.Expect("false");
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
        } else if (type == "SetColorManagement") {
            reader.Expect(",\"settings\":");
            SetColorManagementOperation value;
            value.settings = ReadColorManagement(reader);
            reader.Expect("}");
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
        } else if (type == "AddTransition") {
            AddTransitionOperation value;
            reader.Expect(",\"transition\":{\"id\":");
            value.transition.id = reader.String();
            reader.Expect(",\"track_id\":");
            value.transition.track_id = reader.String();
            reader.Expect(",\"left_clip_id\":");
            value.transition.left_clip_id = reader.String();
            reader.Expect(",\"right_clip_id\":");
            value.transition.right_clip_id = reader.String();
            reader.Expect(",\"transition_type\":");
            value.transition.type = reader.String();
            reader.Expect(",\"duration\":");
            value.transition.duration = ReadTime(reader);
            reader.Expect(",\"alignment\":");
            const std::string alignment = reader.String();
            if (alignment == "center")
                value.transition.alignment = TransitionAlignment::Center;
            else if (alignment == "start_at_cut")
                value.transition.alignment = TransitionAlignment::StartAtCut;
            else if (alignment == "end_at_cut")
                value.transition.alignment = TransitionAlignment::EndAtCut;
            else
                throw std::runtime_error("invalid transition alignment");
            reader.Expect("},\"insertion_index\":");
            value.insertion_index = reader.Integer();
            reader.Expect("}");
            operation = std::move(value);
        } else if (type == "RemoveTransition") {
            RemoveTransitionOperation value;
            reader.Expect(",\"transition_id\":");
            value.transition_id = reader.String();
            reader.Expect("}");
            operation = std::move(value);
        } else if (type == "UpdateTransition") {
            UpdateTransitionOperation value;
            reader.Expect(",\"transition_id\":");
            value.transition_id = reader.String();
            reader.Expect(",\"transition_type\":");
            value.type = reader.String();
            reader.Expect(",\"duration\":");
            value.duration = ReadTime(reader);
            reader.Expect(",\"alignment\":");
            const std::string alignment = reader.String();
            if (alignment == "center")
                value.alignment = TransitionAlignment::Center;
            else if (alignment == "start_at_cut")
                value.alignment = TransitionAlignment::StartAtCut;
            else if (alignment == "end_at_cut")
                value.alignment = TransitionAlignment::EndAtCut;
            else
                throw std::runtime_error("invalid transition alignment");
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
        } else if (type == "SetClipEffects") {
            SetClipEffectsOperation value;
            reader.Expect(",\"clip_id\":");
            value.clip_id = reader.String();
            reader.Expect(",\"effects\":");
            value.effects = ReadClipEffects(reader);
            reader.Expect("}");
            operation = std::move(value);
        } else if (type == "SetClipOpacity") {
            SetClipOpacityOperation value;
            reader.Expect(",\"clip_id\":");
            value.clip_id = reader.String();
            reader.Expect(",\"opacity\":");
            value.opacity = ReadEffectParamValue(reader);
            reader.Expect("}");
            operation = std::move(value);
        } else if (type == "AddCaptionStyle") {
            AddCaptionStyleOperation value;
            reader.Expect(",\"style\":{\"id\":");
            value.style.id = reader.String();
            reader.Expect(",\"font_family\":");
            value.style.font_family = reader.String();
            reader.Expect(",\"font_size\":");
            const int64_t fontSize = reader.Integer();
            if (fontSize < std::numeric_limits<int32_t>::min() ||
                fontSize > std::numeric_limits<int32_t>::max()) {
                throw std::runtime_error("font_size outside int32_t range");
            }
            value.style.font_size = static_cast<int32_t>(fontSize);
            reader.Expect(",\"color\":");
            value.style.color = reader.String();
            reader.Expect(",\"position\":");
            value.style.position = reader.String();
            reader.Expect("},\"insertion_index\":");
            value.insertion_index = reader.Integer();
            reader.Expect("}");
            operation = std::move(value);
        } else if (type == "RemoveCaptionStyle") {
            RemoveCaptionStyleOperation value;
            reader.Expect(",\"style_id\":");
            value.style_id = reader.String();
            reader.Expect("}");
            operation = std::move(value);
        } else if (type == "SetClipCaption") {
            SetClipCaptionOperation value;
            reader.Expect(",\"clip_id\":");
            value.clip_id = reader.String();
            reader.Expect(",\"caption_group_id\":");
            value.caption_group_id = reader.String();
            reader.Expect(",\"caption_text\":");
            value.caption_text = reader.String();
            reader.Expect("}");
            operation = std::move(value);
        } else if (type == "AddMulticamGroup") {
            AddMulticamGroupOperation value;
            reader.Expect(",\"group\":{\"id\":");
            value.group.id = reader.String();
            reader.Expect(",\"name\":");
            value.group.name = reader.String();
            reader.Expect(",\"angles\":[");
            if (!reader.Consume("]")) {
                while (true) {
                    MulticamAngle angle;
                    reader.Expect("{\"id\":");
                    angle.id = reader.String();
                    reader.Expect(",\"name\":");
                    angle.name = reader.String();
                    reader.Expect(",\"clip_id\":");
                    angle.clip_id = reader.String();
                    reader.Expect("}");
                    value.group.angles.push_back(std::move(angle));
                    if (reader.Consume("]")) break;
                    reader.Expect(",");
                }
            }
            reader.Expect(",\"active_angle_id\":");
            value.group.active_angle_id = reader.String();
            reader.Expect("},\"insertion_index\":");
            value.insertion_index = reader.Integer();
            reader.Expect("}");
            operation = std::move(value);
        } else if (type == "RemoveMulticamGroup") {
            RemoveMulticamGroupOperation value;
            reader.Expect(",\"group_id\":");
            value.group_id = reader.String();
            reader.Expect("}");
            operation = std::move(value);
        } else if (type == "SetMulticamActiveAngle") {
            SetMulticamActiveAngleOperation value;
            reader.Expect(",\"group_id\":");
            value.group_id = reader.String();
            reader.Expect(",\"active_angle_id\":");
            value.active_angle_id = reader.String();
            reader.Expect("}");
            operation = std::move(value);
        } else if (type == "SplitClipAtPositions") {
            SplitClipAtPositionsOperation value;
            reader.Expect(",\"clip_id\":");
            value.clip_id = reader.String();
            reader.Expect(",\"positions\":[");
            if (!reader.Consume("]")) {
                while (true) {
                    value.positions.push_back(ReadTime(reader));
                    if (reader.Consume("]")) break;
                    reader.Expect(",");
                }
            }
            reader.Expect(",\"exact_tracks\":");
            value.exact_track_result = ReadExactTracks(reader);
            reader.Expect("}");
            operation = std::move(value);
        } else if (type == "RemoveWords") {
            RemoveWordsOperation value;
            reader.Expect(",\"clip_id\":");
            value.clip_id = reader.String();
            reader.Expect(",\"ranges\":[");
            if (!reader.Consume("]")) {
                while (true) {
                    WordRemovalRange range;
                    reader.Expect("{\"source_start\":");
                    range.source_start = ReadTime(reader);
                    reader.Expect(",\"source_end\":");
                    range.source_end = ReadTime(reader);
                    reader.Expect("}");
                    value.ranges.push_back(range);
                    if (reader.Consume("]")) break;
                    reader.Expect(",");
                }
            }
            reader.Expect(",\"gap_padding\":");
            value.gap_padding = ReadTime(reader);
            // Optional on read, always written: an edit log recorded before
            // linked cuts existed still replays, and every log this build
            // writes is canonical. Same strict-writer / tolerant-reader split
            // Transcription.cc uses for its own cache.
            if (reader.Consume(",\"linked_clip_ids\":[")) {
                if (!reader.Consume("]")) {
                    while (true) {
                        value.linked_clip_ids.push_back(reader.String());
                        if (reader.Consume("]")) break;
                        reader.Expect(",");
                    }
                }
            }
            reader.Expect(",\"sync_track_ids\":[");
            if (!reader.Consume("]")) {
                while (true) {
                    value.sync_track_ids.push_back(reader.String());
                    if (reader.Consume("]")) break;
                    reader.Expect(",");
                }
            }
            reader.Expect(",\"exact_tracks\":");
            value.exact_track_result = ReadExactTracks(reader);
            reader.Expect("}");
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

namespace {

std::string HexEncode(const std::string& input) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string output;
    output.reserve(input.size() * 2);
    for (const unsigned char byte : input) {
        output.push_back(digits[byte >> 4]);
        output.push_back(digits[byte & 0x0f]);
    }
    return output;
}

std::string HexDecode(const std::string& input) {
    if (input.size() % 2 != 0)
        throw std::runtime_error("invalid exact project hex length");
    const auto digit = [](char value) -> int {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        throw std::runtime_error("invalid exact project hex digit");
    };
    std::string output;
    output.reserve(input.size() / 2);
    for (size_t index = 0; index < input.size(); index += 2)
        output.push_back(static_cast<char>((digit(input[index]) << 4) |
                                           digit(input[index + 1])));
    return output;
}

void WriteMediaRate(std::ostringstream& output, const MediaRate& rate) {
    output << "{\"num\":" << rate.num << ",\"den\":" << rate.den << '}';
}

MediaRate ReadMediaRate(Reader& reader) {
    reader.Expect("{\"num\":");
    const int64_t num = reader.Integer();
    reader.Expect(",\"den\":");
    const int64_t den = reader.Integer();
    reader.Expect("}");
    if (num < std::numeric_limits<int32_t>::min() ||
        num > std::numeric_limits<int32_t>::max() ||
        den < std::numeric_limits<int32_t>::min() ||
        den > std::numeric_limits<int32_t>::max())
        throw std::runtime_error("media rate outside int32_t range");
    return {static_cast<int32_t>(num), static_cast<int32_t>(den)};
}

void WriteLibraryMedia(std::ostringstream& output, const LibraryMedia& media) {
    output << "{\"id\":";
    WriteString(output, media.id);
    output << ",\"path\":";
    WriteString(output, media.path);
    output << ",\"filename\":";
    WriteString(output, media.filename);
    output << ",\"codec\":";
    WriteString(output, media.codec);
    output << ",\"width\":" << media.width << ",\"height\":" << media.height
           << ",\"pixel_format\":";
    WriteString(output, media.pixel_format);
    output << ",\"color_range\":";
    WriteString(output, media.color_range);
    output << ",\"color_space\":";
    WriteString(output, media.color_space);
    output << ",\"color_transfer\":";
    WriteString(output, media.color_transfer);
    output << ",\"color_primaries\":";
    WriteString(output, media.color_primaries);
    output << ",\"rotation_degrees\":" << media.rotation_degrees
           << ",\"rate\":";
    WriteMediaRate(output, media.rate);
    output << ",\"duration\":";
    WriteTime(output, media.duration);
    output << ",\"orientation\":";
    WriteString(output, media.orientation);
    output << ",\"has_audio\":" << (media.has_audio ? 1 : 0)
           << ",\"audio_rate\":" << media.audio_rate
           << ",\"audio_channels\":" << media.audio_channels << ",\"bin_id\":";
    WriteString(output, media.bin_id);
    output << ",\"proxy_path\":";
    WriteString(output, media.proxy_path);
    output << ",\"metadata_complete\":" << (media.metadata_complete ? 1 : 0)
           << '}';
}

LibraryMedia ReadLibraryMedia(Reader& reader) {
    LibraryMedia media;
    reader.Expect("{\"id\":");
    media.id = reader.String();
    reader.Expect(",\"path\":");
    media.path = reader.String();
    reader.Expect(",\"filename\":");
    media.filename = reader.String();
    reader.Expect(",\"codec\":");
    media.codec = reader.String();
    reader.Expect(",\"width\":");
    media.width = static_cast<int32_t>(reader.Integer());
    reader.Expect(",\"height\":");
    media.height = static_cast<int32_t>(reader.Integer());
    reader.Expect(",\"pixel_format\":");
    media.pixel_format = reader.String();
    reader.Expect(",\"color_range\":");
    media.color_range = reader.String();
    reader.Expect(",\"color_space\":");
    media.color_space = reader.String();
    reader.Expect(",\"color_transfer\":");
    media.color_transfer = reader.String();
    reader.Expect(",\"color_primaries\":");
    media.color_primaries = reader.String();
    reader.Expect(",\"rotation_degrees\":");
    media.rotation_degrees = static_cast<int32_t>(reader.Integer());
    reader.Expect(",\"rate\":");
    media.rate = ReadMediaRate(reader);
    reader.Expect(",\"duration\":");
    media.duration = ReadTime(reader);
    reader.Expect(",\"orientation\":");
    media.orientation = reader.String();
    reader.Expect(",\"has_audio\":");
    media.has_audio = reader.Integer() != 0;
    reader.Expect(",\"audio_rate\":");
    media.audio_rate = static_cast<int32_t>(reader.Integer());
    reader.Expect(",\"audio_channels\":");
    media.audio_channels = static_cast<int32_t>(reader.Integer());
    reader.Expect(",\"bin_id\":");
    media.bin_id = reader.String();
    reader.Expect(",\"proxy_path\":");
    media.proxy_path = reader.String();
    reader.Expect(",\"metadata_complete\":");
    media.metadata_complete = reader.Integer() != 0;
    reader.Expect("}");
    return media;
}

void WriteExactProject(std::ostringstream& output,
                       const std::optional<ExactProjectState>& exact) {
    if (!exact) {
        output << "null";
        return;
    }
    WriteString(output, HexEncode(exact->canonical_json));
}

std::optional<ExactProjectState> ReadExactProject(Reader& reader) {
    if (reader.Consume("null")) return std::nullopt;
    return ExactProjectState{HexDecode(reader.String())};
}

bool ValidateProjectRelinkCandidate(const Project& project,
                                    const ProjectRelinkItem& item,
                                    std::string& message) {
    const auto media = std::find_if(
        project.rushes.begin(), project.rushes.end(),
        [&](const LibraryMedia& value) { return value.id == item.media_id; });
    const auto source = std::find_if(
        project.sources.begin(), project.sources.end(),
        [&](const DocumentSource& value) { return value.id == item.media_id; });
    if (media == project.rushes.end() || source == project.sources.end()) {
        message = "unknown media or mounted source";
        return false;
    }
    if (item.stored_path.empty()) {
        message = "replacement path is empty";
        return false;
    }
    const LibraryMedia& replacement = item.replacement;
    if (!replacement.metadata_complete || replacement.width <= 0 ||
        replacement.height <= 0) {
        message = "replacement is not a valid video source";
        return false;
    }
    if (static_cast<int64_t>(replacement.rate.num) * source->rate.den !=
        static_cast<int64_t>(source->rate.num) * replacement.rate.den) {
        message = "replacement frame rate differs from the original";
        return false;
    }
    if (replacement.duration < source->duration) {
        message = "replacement is shorter than the original source";
        return false;
    }
    bool audioRequired = false;
    for (const DocumentSequence& timeline : project.timelines)
        for (const DocumentTrack& track : timeline.tracks)
            if (track.kind == "audio")
                for (const DocumentClip& clip : track.clips)
                    if (clip.source_id == item.media_id) audioRequired = true;
    if (audioRequired && !replacement.has_audio) {
        message = "replacement has no audio for existing audio clips";
        return false;
    }
    return true;
}

template <typename T>
void SetExactProject(T& operation, const std::string& json) {
    operation.exact_project_result = ExactProjectState{json};
}

}  // namespace

bool ApplyProjectOperation(Project& project, ProjectOperation& operation,
                           ProjectOperation& inverse, EditError& error,
                           std::string& message) {
    Project candidate = project;
    ProjectOperation normalized = operation;
    const std::string before = project.SaveToString();
    bool restoredExact = false;
    bool applied = std::visit(
        [&](auto& value) -> bool {
            if (value.exact_project_result) {
                Project restored;
                if (!Project::LoadFromString(
                        value.exact_project_result->canonical_json, restored,
                        message)) {
                    error = EditError::ParseError;
                    return false;
                }
                candidate = std::move(restored);
                restoredExact = true;
                return true;
            }
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, AddProjectTimelineOperation>) {
                if (value.name.empty() || value.width <= 0 ||
                    value.height <= 0 || value.frame_rate.num <= 0 ||
                    value.frame_rate.den <= 0) {
                    Fail(EditError::InvalidOperation,
                         "project timeline settings are invalid", error,
                         message);
                    return false;
                }
                if (value.timeline_id.empty())
                    value.timeline_id = GenerateUlid();
                if (value.video_track_id.empty())
                    value.video_track_id = GenerateUlid();
                if (value.audio_track_id.empty())
                    value.audio_track_id = GenerateUlid();
                if (!IsValidUlid(value.timeline_id) ||
                    !IsValidUlid(value.video_track_id) ||
                    !IsValidUlid(value.audio_track_id) ||
                    candidate.FindTimeline(value.timeline_id)) {
                    Fail(EditError::DuplicateId,
                         "project timeline operation contains duplicate IDs",
                         error, message);
                    return false;
                }
                DocumentSequence timeline;
                timeline.id = value.timeline_id;
                timeline.name = value.name;
                timeline.width = value.width;
                timeline.height = value.height;
                timeline.frame_rate = value.frame_rate;
                timeline.tracks = {{value.video_track_id, "video", 0, {}},
                                   {value.audio_track_id, "audio", 1, {}}};
                candidate.timelines.push_back(std::move(timeline));
                return true;
            } else if constexpr (
                std::is_same_v<T, CreateProjectTimelineFromSegmentsOperation>) {
                if (value.name.empty() || value.width <= 0 ||
                    value.height <= 0 || value.frame_rate.num <= 0 ||
                    value.frame_rate.den <= 0 || value.segments.empty()) {
                    Fail(EditError::InvalidOperation,
                         "project short timeline settings are invalid", error,
                         message);
                    return false;
                }
                if (value.timeline_id.empty())
                    value.timeline_id = GenerateUlid();
                if (value.video_track_id.empty())
                    value.video_track_id = GenerateUlid();
                if (value.audio_track_id.empty())
                    value.audio_track_id = GenerateUlid();
                if (!IsValidUlid(value.timeline_id) ||
                    !IsValidUlid(value.video_track_id) ||
                    !IsValidUlid(value.audio_track_id) ||
                    candidate.FindTimeline(value.timeline_id)) {
                    Fail(EditError::DuplicateId,
                         "project short operation contains duplicate IDs",
                         error, message);
                    return false;
                }
                DocumentSequence timeline;
                timeline.id = value.timeline_id;
                timeline.name = value.name;
                timeline.width = value.width;
                timeline.height = value.height;
                timeline.frame_rate = value.frame_rate;
                DocumentTrack video{value.video_track_id, "video", 0, {}};
                DocumentTrack audio{value.audio_track_id, "audio", 1, {}};
                RationalTime cursor{0, value.frame_rate.num};
                std::vector<Ulid> generatedIds{value.timeline_id,
                                               value.video_track_id,
                                               value.audio_track_id};
                for (ProjectTimelineSourceSegment& segment : value.segments) {
                    const auto source = std::find_if(
                        candidate.sources.begin(), candidate.sources.end(),
                        [&](const DocumentSource& item) {
                            return item.id == segment.source_id;
                        });
                    if (source == candidate.sources.end() ||
                        segment.source_in < RationalTime{0, 1} ||
                        !(RationalTime{0, 1} < segment.duration) ||
                        source->duration <
                            segment.source_in.add(segment.duration)) {
                        Fail(EditError::InvalidOperation,
                             "project short contains an invalid source range",
                             error, message);
                        return false;
                    }
                    if (segment.video_clip_id.empty())
                        segment.video_clip_id = GenerateUlid();
                    if (segment.audio_clip_id.empty())
                        segment.audio_clip_id = GenerateUlid();
                    if (segment.link_group_id.empty())
                        segment.link_group_id = GenerateUlid();
                    const std::vector<Ulid> segmentIds{segment.video_clip_id,
                                                       segment.audio_clip_id,
                                                       segment.link_group_id};
                    for (const Ulid& id : segmentIds) {
                        if (!IsValidUlid(id) ||
                            std::find(generatedIds.begin(), generatedIds.end(),
                                      id) != generatedIds.end()) {
                            Fail(EditError::DuplicateId,
                                 "project short contains duplicate clip IDs",
                                 error, message);
                            return false;
                        }
                        generatedIds.push_back(id);
                    }
                    DocumentClip videoClip;
                    videoClip.id = segment.video_clip_id;
                    videoClip.source_id = segment.source_id;
                    videoClip.source_in = segment.source_in;
                    videoClip.duration = segment.duration;
                    videoClip.timeline_in = cursor;
                    videoClip.include_audio = false;
                    videoClip.link_group_id = segment.link_group_id;
                    DocumentClip audioClip = videoClip;
                    audioClip.id = segment.audio_clip_id;
                    video.clips.push_back(std::move(videoClip));
                    audio.clips.push_back(std::move(audioClip));
                    cursor = cursor.add(segment.duration);
                }
                timeline.tracks = {std::move(video), std::move(audio)};
                candidate.timelines.push_back(std::move(timeline));
                if (value.make_active)
                    candidate.active_timeline_id = value.timeline_id;
                return true;
            } else if constexpr (std::is_same_v<
                                     T, RemoveProjectTimelineOperation>) {
                const auto found = std::find_if(
                    candidate.timelines.begin(), candidate.timelines.end(),
                    [&](const DocumentSequence& timeline) {
                        return timeline.id == value.timeline_id;
                    });
                if (found == candidate.timelines.end()) {
                    Fail(EditError::UnknownSequence,
                         "unknown project timeline_id '" + value.timeline_id +
                             "'",
                         error, message);
                    return false;
                }
                if (candidate.timelines.size() == 1) {
                    Fail(EditError::InvalidOperation,
                         "a project must contain at least one timeline", error,
                         message);
                    return false;
                }
                candidate.timelines.erase(found);
                candidate.timeline_bin_ids.erase(value.timeline_id);
                candidate.bin_metadata.erase(value.timeline_id);
                if (candidate.active_timeline_id == value.timeline_id)
                    candidate.active_timeline_id =
                        candidate.timelines.front().id;
                return true;
            } else if constexpr (std::is_same_v<
                                     T, SetProjectBinMetadataOperation>) {
                const bool exists =
                    candidate.FindTimeline(value.item_id) ||
                    std::any_of(candidate.rushes.begin(),
                                candidate.rushes.end(),
                                [&](const LibraryMedia& media) {
                                    return media.id == value.item_id;
                                });
                if (!exists) {
                    Fail(EditError::UnknownMedia,
                         "unknown project bin item '" + value.item_id + "'",
                         error, message);
                    return false;
                }
                if (value.metadata.rating > 5) {
                    Fail(EditError::InvalidOperation,
                         "project bin rating must be between 0 and 5", error,
                         message);
                    return false;
                }
                candidate.bin_metadata[value.item_id] = value.metadata;
                return true;
            } else if constexpr (std::is_same_v<
                                     T, SetProjectTimelineBinOperation>) {
                if (!candidate.FindTimeline(value.timeline_id)) {
                    Fail(EditError::UnknownSequence,
                         "unknown project timeline_id '" + value.timeline_id +
                             "'",
                         error, message);
                    return false;
                }
                if (!value.bin_id.empty() &&
                    std::none_of(candidate.bins.begin(), candidate.bins.end(),
                                 [&](const DocumentBin& bin) {
                                     return bin.id == value.bin_id;
                                 })) {
                    Fail(EditError::UnknownBin,
                         "unknown bin_id '" + value.bin_id + "'", error,
                         message);
                    return false;
                }
                if (value.bin_id.empty())
                    candidate.timeline_bin_ids.erase(value.timeline_id);
                else
                    candidate.timeline_bin_ids[value.timeline_id] =
                        value.bin_id;
                return true;
            } else if constexpr (std::is_same_v<T,
                                                RenameProjectItemOperation>) {
                if (value.name.empty() || value.name.size() > 128) {
                    Fail(EditError::InvalidOperation,
                         "project item name must contain between 1 and 128 "
                         "bytes",
                         error, message);
                    return false;
                }
                if (DocumentSequence* timeline =
                        candidate.FindTimeline(value.item_id)) {
                    timeline->name = value.name;
                    return true;
                }
                const auto rush = std::find_if(
                    candidate.rushes.begin(), candidate.rushes.end(),
                    [&](const LibraryMedia& media) {
                        return media.id == value.item_id;
                    });
                if (rush == candidate.rushes.end()) {
                    Fail(EditError::UnknownMedia,
                         "unknown project item_id '" + value.item_id + "'",
                         error, message);
                    return false;
                }
                candidate.bin_metadata[value.item_id].display_name = value.name;
                return true;
            } else if constexpr (std::is_same_v<
                                     T, SetActiveProjectTimelineOperation>) {
                if (!candidate.FindTimeline(value.timeline_id)) {
                    Fail(EditError::UnknownSequence,
                         "unknown timeline_id '" + value.timeline_id + "'",
                         error, message);
                    return false;
                }
                candidate.active_timeline_id = value.timeline_id;
                return true;
            } else if constexpr (std::is_same_v<T,
                                                RelinkProjectMediaOperation>) {
                if (value.replacements.empty()) {
                    Fail(EditError::InvalidOperation, "relink batch is empty",
                         error, message);
                    return false;
                }
                std::vector<Ulid> seen;
                for (const ProjectRelinkItem& item : value.replacements) {
                    if (std::find(seen.begin(), seen.end(), item.media_id) !=
                        seen.end()) {
                        Fail(EditError::InvalidOperation,
                             "relink batch contains duplicate media IDs", error,
                             message);
                        return false;
                    }
                    if (!ValidateProjectRelinkCandidate(candidate, item,
                                                        message)) {
                        error = EditError::InvalidOperation;
                        return false;
                    }
                    seen.push_back(item.media_id);
                    auto media = std::find_if(
                        candidate.rushes.begin(), candidate.rushes.end(),
                        [&](const LibraryMedia& value) {
                            return value.id == item.media_id;
                        });
                    auto source = std::find_if(
                        candidate.sources.begin(), candidate.sources.end(),
                        [&](const DocumentSource& value) {
                            return value.id == item.media_id;
                        });
                    const Ulid binId = media->bin_id;
                    *media = item.replacement;
                    media->id = item.media_id;
                    media->path = item.stored_path;
                    media->bin_id = binId;
                    media->proxy_path.clear();
                    source->id = item.media_id;
                    source->path = item.stored_path;
                    source->rate = item.replacement.rate;
                    source->duration = item.replacement.duration;
                }
                return true;
            }
            return false;
        },
        normalized);
    if (!applied) return false;
    if (!candidate.Validate(message)) {
        error = EditError::ValidationFailed;
        return false;
    }
    const std::string after = candidate.SaveToString();
    inverse = normalized;
    std::visit([&](auto& value) { SetExactProject(value, before); }, inverse);
    if (!restoredExact)
        std::visit([&](auto& value) { SetExactProject(value, after); },
                   normalized);
    project = std::move(candidate);
    operation = std::move(normalized);
    error = EditError::None;
    message.clear();
    return true;
}

std::string SerializeProjectOperation(const ProjectOperation& operation) {
    std::ostringstream output;
    std::visit(
        [&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, AddProjectTimelineOperation>) {
                output << "{\"type\":\"AddProjectTimeline\",\"name\":";
                WriteString(output, value.name);
                output << ",\"width\":" << value.width
                       << ",\"height\":" << value.height << ",\"frame_rate\":";
                WriteMediaRate(output, value.frame_rate);
                output << ",\"timeline_id\":";
                WriteString(output, value.timeline_id);
                output << ",\"video_track_id\":";
                WriteString(output, value.video_track_id);
                output << ",\"audio_track_id\":";
                WriteString(output, value.audio_track_id);
            } else if constexpr (
                std::is_same_v<T, CreateProjectTimelineFromSegmentsOperation>) {
                output << "{\"type\":\"CreateProjectTimelineFromSegments\","
                          "\"name\":";
                WriteString(output, value.name);
                output << ",\"width\":" << value.width
                       << ",\"height\":" << value.height << ",\"frame_rate\":";
                WriteMediaRate(output, value.frame_rate);
                output << ",\"segments\":[";
                for (size_t index = 0; index < value.segments.size(); ++index) {
                    if (index) output << ',';
                    const ProjectTimelineSourceSegment& segment =
                        value.segments[index];
                    output << "{\"source_id\":";
                    WriteString(output, segment.source_id);
                    output << ",\"source_in\":";
                    WriteTime(output, segment.source_in);
                    output << ",\"duration\":";
                    WriteTime(output, segment.duration);
                    output << ",\"video_clip_id\":";
                    WriteString(output, segment.video_clip_id);
                    output << ",\"audio_clip_id\":";
                    WriteString(output, segment.audio_clip_id);
                    output << ",\"link_group_id\":";
                    WriteString(output, segment.link_group_id);
                    output << '}';
                }
                output << "],\"timeline_id\":";
                WriteString(output, value.timeline_id);
                output << ",\"video_track_id\":";
                WriteString(output, value.video_track_id);
                output << ",\"audio_track_id\":";
                WriteString(output, value.audio_track_id);
                output << ",\"make_active\":"
                       << (value.make_active ? "true" : "false");
            } else if constexpr (std::is_same_v<
                                     T, RemoveProjectTimelineOperation>) {
                output
                    << "{\"type\":\"RemoveProjectTimeline\",\"timeline_id\":";
                WriteString(output, value.timeline_id);
            } else if constexpr (std::is_same_v<
                                     T, SetProjectBinMetadataOperation>) {
                output << "{\"type\":\"SetProjectBinMetadata\",\"item_id\":";
                WriteString(output, value.item_id);
                output << ",\"metadata\":{\"description\":";
                WriteString(output, value.metadata.description);
                output << ",\"rating\":" << value.metadata.rating
                       << ",\"tags\":[";
                for (size_t index = 0; index < value.metadata.tags.size();
                     ++index) {
                    if (index) output << ',';
                    WriteString(output, value.metadata.tags[index]);
                }
                output << "],\"insert_order\":" << value.metadata.insert_order
                       << ",\"display_name\":";
                WriteString(output, value.metadata.display_name);
                output << '}';
            } else if constexpr (std::is_same_v<
                                     T, SetProjectTimelineBinOperation>) {
                output
                    << "{\"type\":\"SetProjectTimelineBin\",\"timeline_id\":";
                WriteString(output, value.timeline_id);
                output << ",\"bin_id\":";
                WriteString(output, value.bin_id);
            } else if constexpr (std::is_same_v<T,
                                                RenameProjectItemOperation>) {
                output << "{\"type\":\"RenameProjectItem\",\"item_id\":";
                WriteString(output, value.item_id);
                output << ",\"name\":";
                WriteString(output, value.name);
            } else if constexpr (std::is_same_v<
                                     T, SetActiveProjectTimelineOperation>) {
                output << "{\"type\":\"SetActiveProjectTimeline\","
                          "\"timeline_id\":";
                WriteString(output, value.timeline_id);
            } else if constexpr (std::is_same_v<T,
                                                RelinkProjectMediaOperation>) {
                output << "{\"type\":\"RelinkProjectMedia\",\"replacements\":[";
                for (size_t index = 0; index < value.replacements.size();
                     ++index) {
                    if (index) output << ',';
                    output << "{\"media_id\":";
                    WriteString(output, value.replacements[index].media_id);
                    output << ",\"replacement\":";
                    WriteLibraryMedia(output,
                                      value.replacements[index].replacement);
                    output << ",\"stored_path\":";
                    WriteString(output, value.replacements[index].stored_path);
                    output << '}';
                }
                output << ']';
            }
            output << ",\"exact_project_hex\":";
            WriteExactProject(output, value.exact_project_result);
            output << '}';
        },
        operation);
    return output.str();
}

bool DeserializeProjectOperation(const std::string& json,
                                 ProjectOperation& operation, EditError& error,
                                 std::string& message) {
    try {
        Reader reader(json);
        reader.Expect("{\"type\":");
        const std::string type = reader.String();
        if (type == "AddProjectTimeline") {
            AddProjectTimelineOperation value;
            reader.Expect(",\"name\":");
            value.name = reader.String();
            reader.Expect(",\"width\":");
            value.width = static_cast<int32_t>(reader.Integer());
            reader.Expect(",\"height\":");
            value.height = static_cast<int32_t>(reader.Integer());
            reader.Expect(",\"frame_rate\":");
            value.frame_rate = ReadMediaRate(reader);
            reader.Expect(",\"timeline_id\":");
            value.timeline_id = reader.String();
            reader.Expect(",\"video_track_id\":");
            value.video_track_id = reader.String();
            reader.Expect(",\"audio_track_id\":");
            value.audio_track_id = reader.String();
            reader.Expect(",\"exact_project_hex\":");
            value.exact_project_result = ReadExactProject(reader);
            reader.Expect("}");
            operation = std::move(value);
        } else if (type == "CreateProjectTimelineFromSegments") {
            CreateProjectTimelineFromSegmentsOperation value;
            reader.Expect(",\"name\":");
            value.name = reader.String();
            reader.Expect(",\"width\":");
            value.width = static_cast<int32_t>(reader.Integer());
            reader.Expect(",\"height\":");
            value.height = static_cast<int32_t>(reader.Integer());
            reader.Expect(",\"frame_rate\":");
            value.frame_rate = ReadMediaRate(reader);
            reader.Expect(",\"segments\":[");
            if (!reader.Consume("]")) {
                while (true) {
                    ProjectTimelineSourceSegment segment;
                    reader.Expect("{\"source_id\":");
                    segment.source_id = reader.String();
                    reader.Expect(",\"source_in\":");
                    segment.source_in = ReadTime(reader);
                    reader.Expect(",\"duration\":");
                    segment.duration = ReadTime(reader);
                    reader.Expect(",\"video_clip_id\":");
                    segment.video_clip_id = reader.String();
                    reader.Expect(",\"audio_clip_id\":");
                    segment.audio_clip_id = reader.String();
                    reader.Expect(",\"link_group_id\":");
                    segment.link_group_id = reader.String();
                    reader.Expect("}");
                    value.segments.push_back(std::move(segment));
                    if (reader.Consume("]")) break;
                    reader.Expect(",");
                }
            }
            reader.Expect(",\"timeline_id\":");
            value.timeline_id = reader.String();
            reader.Expect(",\"video_track_id\":");
            value.video_track_id = reader.String();
            reader.Expect(",\"audio_track_id\":");
            value.audio_track_id = reader.String();
            reader.Expect(",\"make_active\":");
            value.make_active = reader.Consume("true");
            if (!value.make_active) reader.Expect("false");
            reader.Expect(",\"exact_project_hex\":");
            value.exact_project_result = ReadExactProject(reader);
            reader.Expect("}");
            operation = std::move(value);
        } else if (type == "RemoveProjectTimeline") {
            RemoveProjectTimelineOperation value;
            reader.Expect(",\"timeline_id\":");
            value.timeline_id = reader.String();
            reader.Expect(",\"exact_project_hex\":");
            value.exact_project_result = ReadExactProject(reader);
            reader.Expect("}");
            operation = std::move(value);
        } else if (type == "SetProjectBinMetadata") {
            SetProjectBinMetadataOperation value;
            reader.Expect(",\"item_id\":");
            value.item_id = reader.String();
            reader.Expect(",\"metadata\":{\"description\":");
            value.metadata.description = reader.String();
            reader.Expect(",\"rating\":");
            value.metadata.rating = static_cast<uint32_t>(reader.Integer());
            reader.Expect(",\"tags\":[");
            if (!reader.Consume("]")) {
                while (true) {
                    value.metadata.tags.push_back(reader.String());
                    if (reader.Consume("]")) break;
                    reader.Expect(",");
                }
            }
            reader.Expect(",\"insert_order\":");
            value.metadata.insert_order =
                static_cast<uint64_t>(reader.Integer());
            if (reader.Consume(",\"display_name\":"))
                value.metadata.display_name = reader.String();
            reader.Expect("}");
            reader.Expect(",\"exact_project_hex\":");
            value.exact_project_result = ReadExactProject(reader);
            reader.Expect("}");
            operation = std::move(value);
        } else if (type == "SetProjectTimelineBin") {
            SetProjectTimelineBinOperation value;
            reader.Expect(",\"timeline_id\":");
            value.timeline_id = reader.String();
            reader.Expect(",\"bin_id\":");
            value.bin_id = reader.String();
            reader.Expect(",\"exact_project_hex\":");
            value.exact_project_result = ReadExactProject(reader);
            reader.Expect("}");
            operation = std::move(value);
        } else if (type == "SetActiveProjectTimeline") {
            SetActiveProjectTimelineOperation value;
            reader.Expect(",\"timeline_id\":");
            value.timeline_id = reader.String();
            reader.Expect(",\"exact_project_hex\":");
            value.exact_project_result = ReadExactProject(reader);
            reader.Expect("}");
            operation = std::move(value);
        } else if (type == "RenameProjectItem") {
            RenameProjectItemOperation value;
            reader.Expect(",\"item_id\":");
            value.item_id = reader.String();
            reader.Expect(",\"name\":");
            value.name = reader.String();
            reader.Expect(",\"exact_project_hex\":");
            value.exact_project_result = ReadExactProject(reader);
            reader.Expect("}");
            operation = std::move(value);
        } else if (type == "RelinkProjectMedia") {
            RelinkProjectMediaOperation value;
            reader.Expect(",\"replacements\":[");
            if (!reader.Consume("]")) {
                while (true) {
                    ProjectRelinkItem item;
                    reader.Expect("{\"media_id\":");
                    item.media_id = reader.String();
                    reader.Expect(",\"replacement\":");
                    item.replacement = ReadLibraryMedia(reader);
                    reader.Expect(",\"stored_path\":");
                    item.stored_path = reader.String();
                    reader.Expect("}");
                    value.replacements.push_back(std::move(item));
                    if (reader.Consume("]")) break;
                    reader.Expect(",");
                }
            }
            reader.Expect(",\"exact_project_hex\":");
            value.exact_project_result = ReadExactProject(reader);
            reader.Expect("}");
            operation = std::move(value);
        } else {
            throw std::runtime_error("unknown project operation type '" + type +
                                     "'");
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
