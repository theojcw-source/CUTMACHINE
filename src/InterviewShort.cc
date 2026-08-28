#include "InterviewShort.h"

#include "Json.h"
#include "Subtitles.h"
#include "Transcription.h"

#include <algorithm>
#include <limits>
#include <map>
#include <vector>

namespace {

mcp_json::Value TimeValue(const RationalTime& time) {
    mcp_json::Value value = mcp_json::Value::MakeObject();
    value.Set("value", mcp_json::Value::MakeInt(time.value));
    value.Set("rate", mcp_json::Value::MakeInt(time.rate));
    return value;
}

bool ReadTimeValue(const mcp_json::Value* field, RationalTime& time) {
    if (field == nullptr || !field->IsObject()) return false;
    const mcp_json::Value* value = field->Find("value");
    const mcp_json::Value* rate = field->Find("rate");
    int64_t valueOut = 0;
    int64_t rateOut = 0;
    if (value == nullptr || rate == nullptr || !value->AsInt64(valueOut) ||
        !rate->AsInt64(rateOut) || rateOut <= 0 ||
        rateOut > std::numeric_limits<int32_t>::max()) {
        return false;
    }
    time = RationalTime{valueOut, static_cast<int32_t>(rateOut)};
    return true;
}

}  // namespace

bool BuildTimelineTranscriptSpans(
    const Document& document, const std::filesystem::path& transcriptDirectory,
    std::vector<TimelineTranscriptSpan>& spans, std::string& error) {
    spans.clear();
    const bool hasSolo = std::any_of(
        document.sequence.tracks.begin(), document.sequence.tracks.end(),
        [](const DocumentTrack& track) {
            return track.kind == "audio" && track.solo;
        });
    std::map<Ulid, Transcript> transcripts;
    for (const DocumentTrack& track : document.sequence.tracks) {
        if (track.kind != "audio" || track.muted || (hasSolo && !track.solo))
            continue;
        for (const DocumentClip& clip : track.clips) {
            auto found = transcripts.find(clip.source_id);
            if (found == transcripts.end()) {
                Transcript transcript;
                const std::filesystem::path path =
                    transcriptDirectory / (clip.source_id + ".json");
                std::string loadError;
                if (!LoadAudioTranscript(path.string(), transcript, loadError))
                    continue;
                found =
                    transcripts.emplace(clip.source_id, std::move(transcript))
                        .first;
            }
            std::vector<SubtitleCue> cues;
            std::string cueError;
            if (!SubtitleCuesForClip(found->second, clip, cues, cueError))
                continue;
            for (const SubtitleCue& cue : cues) {
                TimelineTranscriptSpan span;
                span.span_id = "S" + std::to_string(spans.size() + 1);
                span.source_id = clip.source_id;
                const RationalTime offset =
                    cue.timeline_in.sub(clip.timeline_in);
                span.source_in = clip.source_in.add(offset);
                span.duration = cue.duration;
                span.timeline_in = cue.timeline_in;
                span.text = cue.text;
                spans.push_back(std::move(span));
            }
        }
    }
    if (spans.empty()) {
        error = "no cached transcript spans for the active audio tracks";
        return false;
    }
    error.clear();
    return true;
}

std::string SerializeTimelineTranscriptSpans(
    const Document& document,
    const std::vector<TimelineTranscriptSpan>& spans) {
    mcp_json::Value list = mcp_json::Value::MakeArray();
    for (const TimelineTranscriptSpan& span : spans) {
        mcp_json::Value value = mcp_json::Value::MakeObject();
        value.Set("span_id", mcp_json::Value::MakeString(span.span_id));
        value.Set("source_id", mcp_json::Value::MakeString(span.source_id));
        value.Set("source_in", TimeValue(span.source_in));
        value.Set("duration", TimeValue(span.duration));
        value.Set("timeline_in", TimeValue(span.timeline_in));
        value.Set(
            "approx_seconds",
            mcp_json::Value::MakeDouble(
                static_cast<double>(span.duration.value) / span.duration.rate));
        value.Set("text", mcp_json::Value::MakeString(span.text));
        list.Push(std::move(value));
    }
    mcp_json::Value root = mcp_json::Value::MakeObject();
    root.Set("timeline_id", mcp_json::Value::MakeString(document.sequence.id));
    root.Set("timeline_name",
             mcp_json::Value::MakeString(document.sequence.name));
    root.Set("spans", std::move(list));
    return root.Dump();
}

bool ParseTimelineTranscriptSpans(const std::string& json,
                                  std::vector<TimelineTranscriptSpan>& spans,
                                  std::string& error) {
    spans.clear();
    mcp_json::Value root;
    std::string parseError;
    if (!mcp_json::Value::Parse(json, root, parseError) || !root.IsObject()) {
        error = "malformed transcript span view: " + parseError;
        return false;
    }
    const mcp_json::Value* list = root.Find("spans");
    if (list == nullptr || !list->IsArray()) {
        error = "transcript span view has no spans";
        return false;
    }
    for (const mcp_json::Value& value : list->AsArray()) {
        TimelineTranscriptSpan span;
        const mcp_json::Value* spanId = value.Find("span_id");
        const mcp_json::Value* sourceId = value.Find("source_id");
        const mcp_json::Value* text = value.Find("text");
        if (spanId == nullptr || !spanId->IsString() || sourceId == nullptr ||
            !sourceId->IsString() ||
            !ReadTimeValue(value.Find("source_in"), span.source_in) ||
            !ReadTimeValue(value.Find("duration"), span.duration) ||
            !ReadTimeValue(value.Find("timeline_in"), span.timeline_in)) {
            error = "transcript span view contains an invalid span";
            return false;
        }
        span.span_id = spanId->AsString();
        span.source_id = sourceId->AsString();
        if (text != nullptr && text->IsString()) span.text = text->AsString();
        spans.push_back(std::move(span));
    }
    error.clear();
    return true;
}

bool DescribeTimelineTranscriptForAgent(
    const Document& document, const std::filesystem::path& transcriptDirectory,
    std::string& json, std::string& error) {
    std::vector<TimelineTranscriptSpan> spans;
    if (!BuildTimelineTranscriptSpans(document, transcriptDirectory, spans,
                                      error)) {
        return false;
    }
    json = SerializeTimelineTranscriptSpans(document, spans);
    error.clear();
    return true;
}

bool ResolveTranscriptSpanRange(
    const std::vector<TimelineTranscriptSpan>& spans,
    const std::string& startSpanId, const std::string& endSpanId,
    Ulid& sourceId, RationalTime& sourceIn, RationalTime& duration,
    std::string& error) {
    const auto find = [&](const std::string& id) {
        return std::find_if(spans.begin(), spans.end(),
                            [&](const TimelineTranscriptSpan& span) {
                                return span.span_id == id;
                            });
    };
    const auto first = find(startSpanId);
    if (first == spans.end()) {
        error = "unknown span_id '" + startSpanId + "'";
        return false;
    }
    auto last = first;
    if (!endSpanId.empty()) {
        last = find(endSpanId);
        if (last == spans.end()) {
            error = "unknown end_span_id '" + endSpanId + "'";
            return false;
        }
        if (last < first) {
            error = "end_span_id '" + endSpanId +
                    "' comes before start span_id '" + startSpanId + "'";
            return false;
        }
    }
    // Every span in the run must belong to one source and butt against its
    // predecessor. Anything else is a gap the caller did not name, and
    // stitching over it would invent an edit.
    for (auto span = first; span != last; ++span) {
        const auto next = span + 1;
        if (next->source_id != span->source_id) {
            error = "spans '" + span->span_id + "' and '" + next->span_id +
                    "' come from different sources and cannot be merged";
            return false;
        }
        // Two cues are usually flush, but a cue also breaks on a pause
        // between words, and that pause then belongs to neither of them. A
        // run that steps over one of those is still one continuous stretch
        // of the source -- merging it takes the breath along, which is what
        // a cut through a sentence has to do. Refusing it, as an earlier
        // version of this function did, made ordinary sentences
        // unselectable: measured on a real interview, three of eight
        // editorial beats were rejected over gaps of one and two frames.
        //
        // What must stay refused is a run that steps over a real editorial
        // gap, because merging that would silently splice out whatever sits
        // between. The line between the two is the cue segmenter's own
        // maximum gap, taken from the same constant so the two cannot drift.
        const RationalTime gap =
            next->source_in.sub(span->source_in.add(span->duration));
        if (gap.compare(RationalTime{0, 1}) < 0 ||
            gap.compare(kSubtitleCueMaximumGap) >= 0) {
            error = "spans '" + span->span_id + "' and '" + next->span_id +
                    "' are separated by more than a breath; select them as "
                    "separate segments if the silence between them is wanted";
            return false;
        }
    }
    sourceId = first->source_id;
    sourceIn = first->source_in;
    duration = last->source_in.add(last->duration).sub(first->source_in);
    error.clear();
    return true;
}
