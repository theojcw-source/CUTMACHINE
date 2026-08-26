#include "InterviewShort.h"

#include "Json.h"
#include "Subtitles.h"
#include "Transcription.h"

#include <algorithm>
#include <map>
#include <vector>

namespace {

mcp_json::Value TimeValue(const RationalTime& time) {
    mcp_json::Value value = mcp_json::Value::MakeObject();
    value.Set("value", mcp_json::Value::MakeInt(time.value));
    value.Set("rate", mcp_json::Value::MakeInt(time.rate));
    return value;
}

}  // namespace

bool DescribeTimelineTranscriptForAgent(
    const Document& document, const std::filesystem::path& transcriptDirectory,
    std::string& json, std::string& error) {
    const bool hasSolo = std::any_of(
        document.sequence.tracks.begin(), document.sequence.tracks.end(),
        [](const DocumentTrack& track) {
            return track.kind == "audio" && track.solo;
        });
    std::map<Ulid, Transcript> transcripts;
    mcp_json::Value spans = mcp_json::Value::MakeArray();
    size_t spanIndex = 0;
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
                mcp_json::Value span = mcp_json::Value::MakeObject();
                span.Set("span_id", mcp_json::Value::MakeString(
                                        "S" + std::to_string(++spanIndex)));
                span.Set("source_id",
                         mcp_json::Value::MakeString(clip.source_id));
                const RationalTime offset =
                    cue.timeline_in.sub(clip.timeline_in);
                span.Set("source_in", TimeValue(clip.source_in.add(offset)));
                span.Set("duration", TimeValue(cue.duration));
                span.Set("timeline_in", TimeValue(cue.timeline_in));
                span.Set("approx_seconds",
                         mcp_json::Value::MakeDouble(
                             static_cast<double>(cue.duration.value) /
                             cue.duration.rate));
                span.Set("text", mcp_json::Value::MakeString(cue.text));
                spans.Push(std::move(span));
            }
        }
    }
    if (spanIndex == 0) {
        error = "no cached transcript spans for the active audio tracks";
        return false;
    }
    mcp_json::Value root = mcp_json::Value::MakeObject();
    root.Set("timeline_id", mcp_json::Value::MakeString(document.sequence.id));
    root.Set("timeline_name",
             mcp_json::Value::MakeString(document.sequence.name));
    root.Set("spans", std::move(spans));
    json = root.Dump();
    error.clear();
    return true;
}
