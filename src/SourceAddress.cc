#include "SourceAddress.h"

#include "Json.h"

#include <algorithm>
#include <limits>

namespace {

using mcp_json::Value;

void WriteTime(Value& parent, const std::string& key,
               const RationalTime& time) {
    Value entry = Value::MakeObject();
    entry.Set("value", Value::MakeInt(time.value));
    entry.Set("rate", Value::MakeInt(time.rate));
    parent.Set(key, std::move(entry));
}

// A frame index in a source's own rate, as an exact time in that same
// domain. The source's rate is a MediaRate (num/den), so frame k starts at
// k*den/num seconds; expressing it as {k*den, num} keeps it exact for a
// 24000/1001 rush as much as for a 25 i/s one.
bool SourceFrameTime(const MediaRate& rate, int64_t frame, RationalTime& time,
                     std::string& error) {
    if (rate.num <= 0 || rate.den <= 0) {
        error = "source has an invalid frame rate";
        return false;
    }
    if (frame < 0) {
        error = "source_frame cannot be negative";
        return false;
    }
    if (frame > std::numeric_limits<int64_t>::max() / rate.den) {
        error = "source_frame is too large for the source frame rate";
        return false;
    }
    time = RationalTime{frame * static_cast<int64_t>(rate.den), rate.num};
    return true;
}

bool LastFrameBefore(const RationalTime& time, const MediaRate& rate,
                     int64_t& frame, std::string& error) {
    frame = time.to_frames(rate.num, rate.den);
    RationalTime frameTime;
    if (!SourceFrameTime(rate, frame, frameTime, error)) return false;
    if (frameTime == time) {
        if (frame == std::numeric_limits<int64_t>::min()) {
            error = "source frame bound underflows";
            return false;
        }
        --frame;
    }
    return true;
}

bool FirstFrameAfter(const RationalTime& time, const MediaRate& rate,
                     int64_t& frame, std::string& error) {
    frame = time.to_frames(rate.num, rate.den);
    RationalTime frameTime;
    if (!SourceFrameTime(rate, frame, frameTime, error)) return false;
    if (frameTime <= time) {
        if (frame == std::numeric_limits<int64_t>::max()) {
            error = "source frame bound overflows";
            return false;
        }
        ++frame;
    }
    return true;
}

bool ValidateSourceFrameBounds(const std::string& operation,
                               int64_t sourceFrame, int64_t first,
                               int64_t last, std::string& error) {
    if (sourceFrame >= first && sourceFrame <= last) return true;
    error = operation + " source_frame must be within [" +
            std::to_string(first) + ", " + std::to_string(last) +
            "]; got " + std::to_string(sourceFrame);
    return false;
}

}  // namespace

bool ResolveSourceFrame(const Document& document, const Ulid& mediaId,
                        int64_t sourceFrame,
                        std::vector<SourceFrameMatch>& matches,
                        std::string& error) {
    error.clear();
    matches.clear();
    const DocumentSource* source = document.FindSource(mediaId);
    if (source == nullptr) {
        error = "no mounted source with id '" + mediaId + "'";
        return false;
    }
    RationalTime sourceTime;
    if (!SourceFrameTime(source->rate, sourceFrame, sourceTime, error))
        return false;

    for (const DocumentTrack& track : document.sequence.tracks) {
        for (const DocumentClip& clip : track.clips) {
            if (clip.source_id != mediaId) continue;
            const RationalTime clipEnd = clip.source_in.add(clip.duration);
            // Half-open, like every other range in this codebase: a clip
            // plays [source_in, source_in+duration). The frame at the exact
            // end belongs to whatever comes after it, not here.
            if (sourceTime.compare(clip.source_in) < 0 ||
                sourceTime.compare(clipEnd) >= 0)
                continue;
            SourceFrameMatch match;
            match.clip_id = clip.id;
            match.track_id = track.id;
            match.track_kind = track.kind;
            match.track_index = track.index;
            match.link_group_id = clip.link_group_id;
            match.offset_in_clip = sourceTime.sub(clip.source_in);
            match.timeline_position =
                clip.timeline_in.add(match.offset_in_clip);
            match.clip_source_in = clip.source_in;
            match.clip_duration = clip.duration;
            match.clip_timeline_in = clip.timeline_in;
            matches.push_back(std::move(match));
        }
    }
    std::sort(
        matches.begin(), matches.end(),
        [](const SourceFrameMatch& left, const SourceFrameMatch& right) {
            if (left.track_index != right.track_index)
                return left.track_index < right.track_index;
            return left.timeline_position.compare(right.timeline_position) < 0;
        });
    return true;
}

bool ResolveClipSourceFramePosition(const Document& document,
                                    const Ulid& clipId, int64_t sourceFrame,
                                    RationalTime& timelinePosition,
                                    std::string& error) {
    error.clear();
    const DocumentClip* clip = document.FindClip(clipId);
    if (clip == nullptr) {
        error = "no clip matches id '" + clipId + "'";
        return false;
    }
    const DocumentSource* source = document.FindSource(clip->source_id);
    if (source == nullptr) {
        error = "clip '" + clipId + "' reads from an unmounted source";
        return false;
    }
    int64_t first = 0;
    int64_t last = 0;
    if (!FirstFrameAfter(clip->source_in, source->rate, first, error) ||
        !LastFrameBefore(clip->source_in.add(clip->duration), source->rate,
                         last, error))
        return false;
    if (!ValidateSourceFrameBounds("split_clip", sourceFrame, first, last,
                                   error)) {
        return false;
    }
    RationalTime sourceTime;
    if (!SourceFrameTime(source->rate, sourceFrame, sourceTime, error))
        return false;
    timelinePosition = clip->timeline_in.add(sourceTime.sub(clip->source_in));
    return true;
}

bool ResolveClipSourceFrameTrim(const Document& document, const Ulid& clipId,
                                int64_t sourceFrame, TrimEdge edge,
                                RationalTime& delta, std::string& error) {
    error.clear();
    const DocumentClip* clip = document.FindClip(clipId);
    if (clip == nullptr) {
        error = "no clip matches id '" + clipId + "'";
        return false;
    }
    const DocumentSource* source = document.FindSource(clip->source_id);
    if (source == nullptr) {
        error = "clip '" + clipId + "' reads from an unmounted source";
        return false;
    }
    const int64_t clipFirst =
        clip->source_in.to_frames(source->rate.num, source->rate.den);
    int64_t clipLast = 0;
    if (!LastFrameBefore(clip->source_in.add(clip->duration), source->rate,
                         clipLast, error))
        return false;
    int64_t sourceLast = 0;
    if (!LastFrameBefore(source->duration, source->rate, sourceLast, error))
        return false;
    const int64_t first = edge == TrimEdge::Head ? 0 : clipFirst;
    const int64_t last = edge == TrimEdge::Head ? clipLast : sourceLast;
    if (!ValidateSourceFrameBounds("trim_clip", sourceFrame, first, last,
                                   error)) {
        return false;
    }
    RationalTime sourceTime;
    if (!SourceFrameTime(source->rate, sourceFrame, sourceTime, error))
        return false;
    if (edge == TrimEdge::Head) {
        // ApplyTrimClip adds the delta to source_in, so the delta is simply
        // the distance from where the clip starts now to where it should.
        delta = sourceTime.sub(clip->source_in);
        if (clip->duration.sub(delta).value <= 0) {
            error = "a head on source frame " + std::to_string(sourceFrame) +
                    " would leave clip '" + clipId + "' empty";
            return false;
        }
        return true;
    }
    // Inclusive: the named frame is the last one kept, so the clip has to
    // run to the start of the frame after it. This +1 is exactly the
    // arithmetic a caller must not be asked to do.
    RationalTime lastFrameEnd;
    if (sourceFrame == std::numeric_limits<int64_t>::max()) {
        error = "trim_clip source_frame is too large";
        return false;
    }
    if (!SourceFrameTime(source->rate, sourceFrame + 1, lastFrameEnd, error))
        return false;
    delta = lastFrameEnd.sub(clip->source_in.add(clip->duration));
    if (clip->duration.add(delta).value <= 0) {
        error = "a tail on source frame " + std::to_string(sourceFrame) +
                " would leave clip '" + clipId + "' empty";
        return false;
    }
    return true;
}

std::string DescribeSourceFrameMatches(
    const Ulid& mediaId, int64_t sourceFrame,
    const std::vector<SourceFrameMatch>& matches) {
    Value root = Value::MakeObject();
    root.Set("media_id", Value::MakeString(mediaId));
    root.Set("source_frame", Value::MakeInt(sourceFrame));
    root.Set("match_count",
             Value::MakeInt(static_cast<int64_t>(matches.size())));
    Value list = Value::MakeArray();
    for (const SourceFrameMatch& match : matches) {
        Value entry = Value::MakeObject();
        entry.Set("clip_id", Value::MakeString(match.clip_id));
        entry.Set("track_id", Value::MakeString(match.track_id));
        entry.Set("track_kind", Value::MakeString(match.track_kind));
        entry.Set("track_index", Value::MakeInt(match.track_index));
        if (!match.link_group_id.empty())
            entry.Set("link_group_id", Value::MakeString(match.link_group_id));
        WriteTime(entry, "timeline_position", match.timeline_position);
        WriteTime(entry, "offset_in_clip", match.offset_in_clip);
        WriteTime(entry, "clip_source_in", match.clip_source_in);
        WriteTime(entry, "clip_duration", match.clip_duration);
        WriteTime(entry, "clip_timeline_in", match.clip_timeline_in);
        list.Push(std::move(entry));
    }
    root.Set("matches", std::move(list));
    return root.Dump();
}
