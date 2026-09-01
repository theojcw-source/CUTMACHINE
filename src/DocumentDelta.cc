#include "DocumentDelta.h"

#include "Json.h"
#include "Timeline.h"

#include <algorithm>
#include <map>
#include <sstream>

namespace {

using mcp_json::Value;

struct Placed {
    const DocumentClip* clip = nullptr;
    const DocumentTrack* track = nullptr;
};

std::map<Ulid, Placed> IndexClips(const Document& document) {
    std::map<Ulid, Placed> index;
    for (const DocumentTrack& track : document.sequence.tracks)
        for (const DocumentClip& clip : track.clips)
            index[clip.id] = Placed{&clip, &track};
    return index;
}

Value TimeValue(const RationalTime& time) {
    Value value = Value::MakeObject();
    value.Set("value", Value::MakeInt(time.value));
    value.Set("rate", Value::MakeInt(time.rate));
    return value;
}

// A move and nothing else: same track, same source range, same everything
// but where it sits. Only these can be folded into a DeltaShift.
bool OnlyMoved(const Placed& before, const Placed& after) {
    if (before.track->id != after.track->id) return false;
    DocumentClip candidate = *after.clip;
    candidate.timeline_in = before.clip->timeline_in;
    return ClipsEqual(*before.clip, candidate);
}

DeltaClip Describe(const Placed& placed, bool created) {
    DeltaClip entry;
    entry.clip_id = placed.clip->id;
    entry.track_id = placed.track->id;
    entry.source_id = placed.clip->source_id;
    entry.source_in = placed.clip->source_in;
    entry.duration = placed.clip->duration;
    entry.timeline_in = placed.clip->timeline_in;
    entry.created = created;
    return entry;
}

}  // namespace

bool ClipsEqual(const DocumentClip& left, const DocumentClip& right) {
    if (left.id != right.id || left.source_id != right.source_id ||
        left.include_audio != right.include_audio ||
        left.link_group_id != right.link_group_id ||
        left.sync_anchor_clip_id != right.sync_anchor_clip_id ||
        left.caption_group_id != right.caption_group_id ||
        left.caption_text != right.caption_text)
        return false;
    if (left.source_in.compare(right.source_in) != 0 ||
        left.duration.compare(right.duration) != 0 ||
        left.timeline_in.compare(right.timeline_in) != 0 ||
        left.sync_reference_delta.compare(right.sync_reference_delta) != 0)
        return false;
    if (left.opacity.num != right.opacity.num ||
        left.opacity.den != right.opacity.den)
        return false;
    if (left.effects.size() != right.effects.size()) return false;
    for (size_t index = 0; index < left.effects.size(); ++index) {
        const ClipEffect& a = left.effects[index];
        const ClipEffect& b = right.effects[index];
        if (a.id != b.id || a.type != b.type) return false;
        if (a.params.size() != b.params.size()) return false;
        for (const auto& entry : a.params) {
            const auto found = b.params.find(entry.first);
            if (found == b.params.end() ||
                found->second.num != entry.second.num ||
                found->second.den != entry.second.den)
                return false;
        }
    }
    return true;
}

bool ComputeDocumentDelta(const Document& before, const Document& after,
                          DocumentDelta& delta) {
    delta = DocumentDelta{};
    try {
        delta.duration = Timeline(after).Duration();
    } catch (const std::exception&) {
        delta.duration = RationalTime{0, 1};
    }
    const DocumentSequence& oldSequence = before.sequence;
    const DocumentSequence& newSequence = after.sequence;
    delta.sequence_changed =
        oldSequence.name != newSequence.name ||
        oldSequence.width != newSequence.width ||
        oldSequence.height != newSequence.height ||
        oldSequence.frame_rate.num != newSequence.frame_rate.num ||
        oldSequence.frame_rate.den != newSequence.frame_rate.den;

    std::map<Ulid, const DocumentTrack*> oldTracks;
    for (const DocumentTrack& track : before.sequence.tracks)
        oldTracks[track.id] = &track;
    for (const DocumentTrack& track : after.sequence.tracks) {
        if (oldTracks.count(track.id)) continue;
        DeltaTrack entry;
        entry.track_id = track.id;
        entry.kind = track.kind;
        entry.index = track.index;
        delta.created_tracks.push_back(std::move(entry));
    }
    std::map<Ulid, const DocumentTrack*> newTracks;
    for (const DocumentTrack& track : after.sequence.tracks)
        newTracks[track.id] = &track;
    for (const auto& entry : oldTracks)
        if (!newTracks.count(entry.first))
            delta.removed_track_ids.push_back(entry.first);

    const std::map<Ulid, Placed> oldClips = IndexClips(before);
    const std::map<Ulid, Placed> newClips = IndexClips(after);
    for (const auto& entry : oldClips)
        if (!newClips.count(entry.first))
            delta.removed_clip_ids.push_back(entry.first);

    // Pure moves are held back: they may fold into a shift rule, and only
    // the ones that do not are published individually.
    std::vector<std::pair<Placed, Placed>> moved;  // before, after
    std::vector<DeltaClip> individual;
    for (const auto& entry : newClips) {
        const auto found = oldClips.find(entry.first);
        if (found == oldClips.end()) {
            individual.push_back(Describe(entry.second, true));
            continue;
        }
        if (found->second.track->id == entry.second.track->id &&
            ClipsEqual(*found->second.clip, *entry.second.clip))
            continue;  // untouched
        if (OnlyMoved(found->second, entry.second)) {
            moved.push_back({found->second, entry.second});
            continue;
        }
        individual.push_back(Describe(entry.second, false));
    }

    // A rule stands only if it explains every clip on its track from `from`
    // onward. Anything less would let a caller apply it to a clip that did
    // not actually move.
    std::map<Ulid, std::vector<std::pair<Placed, Placed>>> byTrack;
    for (const auto& pair : moved)
        byTrack[pair.first.track->id].push_back(pair);
    for (auto& entry : byTrack) {
        std::vector<std::pair<Placed, Placed>>& group = entry.second;
        const RationalTime by = group.front().second.clip->timeline_in.sub(
            group.front().first.clip->timeline_in);
        bool uniform = true;
        RationalTime from = group.front().first.clip->timeline_in;
        for (const auto& pair : group) {
            const RationalTime step =
                pair.second.clip->timeline_in.sub(pair.first.clip->timeline_in);
            if (step.compare(by) != 0) uniform = false;
            if (pair.first.clip->timeline_in.compare(from) < 0)
                from = pair.first.clip->timeline_in;
        }
        bool covers = uniform;
        if (covers) {
            const DocumentTrack* track = group.front().first.track;
            for (const DocumentClip& clip : track->clips) {
                if (clip.timeline_in.compare(from) < 0) continue;
                const bool listed =
                    std::any_of(group.begin(), group.end(),
                                [&](const std::pair<Placed, Placed>& pair) {
                                    return pair.first.clip->id == clip.id;
                                });
                if (!listed) {
                    covers = false;
                    break;
                }
            }
        }
        if (covers) {
            DeltaShift shift;
            shift.track_id = entry.first;
            shift.from = from;
            shift.by = by;
            shift.count = static_cast<int32_t>(group.size());
            delta.shifted.push_back(std::move(shift));
            continue;
        }
        for (const auto& pair : group)
            individual.push_back(Describe(pair.second, false));
    }

    std::stable_sort(individual.begin(), individual.end(),
                     [](const DeltaClip& left, const DeltaClip& right) {
                         return left.timeline_in.compare(right.timeline_in) < 0;
                     });
    delta.clips = std::move(individual);
    return !delta.clips.empty() || !delta.removed_clip_ids.empty() ||
           !delta.shifted.empty() || !delta.created_tracks.empty() ||
           !delta.removed_track_ids.empty() || delta.sequence_changed;
}

std::string SerializeDocumentDelta(const DocumentDelta& delta) {
    Value root = Value::MakeObject();
    root.Set("duration", TimeValue(delta.duration));
    if (!delta.clips.empty()) {
        Value clips = Value::MakeArray();
        for (const DeltaClip& clip : delta.clips) {
            Value entry = Value::MakeObject();
            entry.Set("clip_id", Value::MakeString(clip.clip_id));
            entry.Set("track_id", Value::MakeString(clip.track_id));
            entry.Set("source_id", Value::MakeString(clip.source_id));
            entry.Set("source_in", TimeValue(clip.source_in));
            entry.Set("duration", TimeValue(clip.duration));
            entry.Set("timeline_in", TimeValue(clip.timeline_in));
            if (clip.created) entry.Set("created", Value::MakeBool(true));
            clips.Push(std::move(entry));
        }
        root.Set("clips", std::move(clips));
    }
    if (!delta.shifted.empty()) {
        Value shifted = Value::MakeArray();
        for (const DeltaShift& shift : delta.shifted) {
            Value entry = Value::MakeObject();
            entry.Set("track_id", Value::MakeString(shift.track_id));
            entry.Set("from", TimeValue(shift.from));
            entry.Set("by", TimeValue(shift.by));
            entry.Set("count", Value::MakeInt(shift.count));
            shifted.Push(std::move(entry));
        }
        root.Set("shifted", std::move(shifted));
    }
    if (!delta.removed_clip_ids.empty()) {
        Value removed = Value::MakeArray();
        for (const Ulid& id : delta.removed_clip_ids)
            removed.Push(Value::MakeString(id));
        root.Set("removed_clip_ids", std::move(removed));
    }
    if (!delta.created_tracks.empty()) {
        Value tracks = Value::MakeArray();
        for (const DeltaTrack& track : delta.created_tracks) {
            Value entry = Value::MakeObject();
            entry.Set("track_id", Value::MakeString(track.track_id));
            entry.Set("kind", Value::MakeString(track.kind));
            entry.Set("index", Value::MakeInt(track.index));
            tracks.Push(std::move(entry));
        }
        root.Set("created_tracks", std::move(tracks));
    }
    if (!delta.removed_track_ids.empty()) {
        Value removed = Value::MakeArray();
        for (const Ulid& id : delta.removed_track_ids)
            removed.Push(Value::MakeString(id));
        root.Set("removed_track_ids", std::move(removed));
    }
    if (delta.sequence_changed)
        root.Set("sequence_changed", Value::MakeBool(true));
    return root.Dump();
}
