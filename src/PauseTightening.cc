#include "PauseTightening.h"

#include "Json.h"
#include "Transcription.h"

#include <algorithm>
#include <vector>

namespace {

using mcp_json::Value;

// One maximal run of windows under the speech threshold, as window indices
// into the envelope: [first, last).
struct Hollow {
    int64_t first = 0;
    int64_t last = 0;
};

void WriteTime(Value& parent, const std::string& key,
               const RationalTime& time) {
    Value entry = Value::MakeObject();
    entry.Set("value", Value::MakeInt(time.value));
    entry.Set("rate", Value::MakeInt(time.rate));
    parent.Set(key, std::move(entry));
}

}  // namespace

bool ResolvePauseTightening(const DocumentClip& clip,
                            const SpeechOnsetReport& envelope,
                            const PauseTighteningSettings& settings,
                            const MediaRate& sourceRate,
                            const std::vector<Ulid>& syncTrackIds,
                            RemoveWordsOperation& operation,
                            PauseTighteningReport& report, std::string& error) {
    error.clear();
    operation = RemoveWordsOperation{};
    report = PauseTighteningReport{};
    if (clip.source_id != envelope.media_id) {
        error = "speech envelope belongs to another source";
        return false;
    }
    if (envelope.windows_per_second == 0 || envelope.levels.empty()) {
        error = "speech envelope has no analysis grid";
        return false;
    }
    if (sourceRate.num <= 0 || sourceRate.den <= 0) {
        error = "source frame rate is invalid";
        return false;
    }
    if (settings.minimum_gap_milliseconds <= 0) {
        error = "minimum gap must be a positive duration";
        return false;
    }
    if (settings.keep_frames < 0) {
        error = "frames to keep cannot be negative";
        return false;
    }
    if (settings.speech_ratio_percent <= 0 ||
        settings.speech_ratio_percent >= 100) {
        error = "speech ratio must be a percentage strictly between 0 and 100";
        return false;
    }

    const int32_t grid = static_cast<int32_t>(envelope.windows_per_second);
    const RationalTime clipStart = clip.source_in;
    const RationalTime clipEnd = clip.source_in.add(clip.duration);
    // Floor at the head and ceiling at the tail, so the analysed span never
    // reaches past the material the clip actually plays.
    int64_t firstWindow = clipStart.to_frames(grid);
    int64_t lastWindow = clipEnd.to_frames(grid);
    if (clipEnd.compare(RationalTime{lastWindow, grid}) != 0) ++lastWindow;
    if (firstWindow < 0) firstWindow = 0;
    if (lastWindow > static_cast<int64_t>(envelope.levels.size()))
        lastWindow = static_cast<int64_t>(envelope.levels.size());
    if (lastWindow <= firstWindow) {
        error = "clip holds no analysis window";
        return false;
    }

    // The clip's own speech level, never the source's: a rush holding one
    // loud take and one quiet one would otherwise measure the quiet clip
    // against the loud one and find it silent from end to end. Same choice,
    // and same reason, as SummarizeClipSpeechOnset.
    std::vector<int64_t> inside(
        envelope.levels.begin() + static_cast<size_t>(firstWindow),
        envelope.levels.begin() + static_cast<size_t>(lastWindow));
    const int64_t speechLevel = SpeechLevelPercentile(inside, 90);
    const int64_t threshold = speechLevel * settings.speech_ratio_percent / 100;

    std::vector<Hollow> hollows;
    int64_t runStart = -1;
    for (int64_t window = firstWindow; window < lastWindow; ++window) {
        const bool silent =
            envelope.levels[static_cast<size_t>(window)] < threshold;
        if (silent && runStart < 0) runStart = window;
        if (!silent && runStart >= 0) {
            hollows.push_back({runStart, window});
            runStart = -1;
        }
    }
    if (runStart >= 0) hollows.push_back({runStart, lastWindow});

    // Head and tail air belong to SpeechOnset.h's question, not this one.
    // They are measured and reported so a caller sees what was left standing.
    if (!hollows.empty() && hollows.front().first == firstWindow) {
        report.head_air =
            RationalTime{hollows.front().last - firstWindow, grid};
        hollows.erase(hollows.begin());
    }
    if (!hollows.empty() && hollows.back().last == lastWindow) {
        report.tail_air = RationalTime{lastWindow - hollows.back().first, grid};
        hollows.pop_back();
    }

    const RationalTime minimumGap{settings.minimum_gap_milliseconds, 1000};
    const RationalTime keep{settings.keep_frames * sourceRate.den,
                            sourceRate.num};
    // Split between the two ends rather than left at one: the splice then
    // sits in the deepest part of the hollow, as far from the decay of the
    // previous word and from the attack of the next as the budget allows.
    // Odd frames go to the head, where a trailing sibilant lives.
    const int64_t headFrames = (settings.keep_frames + 1) / 2;
    const int64_t tailFrames = settings.keep_frames / 2;
    const RationalTime headKeep{headFrames * sourceRate.den, sourceRate.num};
    const RationalTime tailKeep{tailFrames * sourceRate.den, sourceRate.num};

    RationalTime removedTotal{0, sourceRate.num};
    for (const Hollow& hollow : hollows) {
        const RationalTime rawStart{hollow.first, grid};
        const RationalTime rawEnd{hollow.last, grid};
        if (rawEnd.sub(rawStart).compare(minimumGap) < 0) {
            ++report.skipped_short;
            continue;
        }
        // Inward on both sides: a boundary the envelope places between two
        // frames resolves inside the silence, never over the speech next to
        // it. RoundToSourceFrame is the codebase's one explicit rounding rule
        // for exactly this kind of continuous-to-frame snap.
        const RationalTime start = RoundToSourceFrame(rawStart, sourceRate,
                                                      /*roundUp=*/true);
        const RationalTime end = RoundToSourceFrame(rawEnd, sourceRate,
                                                    /*roundUp=*/false);
        if (start.compare(end) >= 0) {
            ++report.skipped_short;
            continue;
        }
        const RationalTime cutStart = start.add(headKeep);
        const RationalTime cutEnd = end.sub(tailKeep);
        if (cutStart.compare(cutEnd) >= 0) {
            // The hollow is already at or under the length asked for. Not a
            // failure: the clip is already as tight as the request wants.
            ++report.skipped_already_tight;
            continue;
        }
        if (cutStart.compare(clipStart) < 0 || cutEnd.compare(clipEnd) > 0) {
            // Rounding pushed a boundary outside the clip. ApplyOperation
            // would refuse the whole operation for it, so drop this one
            // pause rather than lose the others with it.
            ++report.skipped_short;
            continue;
        }
        operation.ranges.push_back({cutStart, cutEnd});
        const RationalTime removed = cutEnd.sub(cutStart);
        report.pauses.push_back({rawStart, rawEnd, removed});
        removedTotal = removedTotal.add(removed);
        // Reported in the same domain as the pauses that make it up, so a
        // caller adding the entries up gets the published total back.
        report.removed_total = removedTotal;
    }

    operation.clip_id = clip.id;
    operation.gap_padding = RationalTime{0, sourceRate.num};
    operation.sync_track_ids = syncTrackIds;
    return true;
}

std::string SerializePauseTighteningReport(
    const PauseTighteningReport& report) {
    Value root = Value::MakeObject();
    root.Set("closed_count",
             Value::MakeInt(static_cast<int64_t>(report.pauses.size())));
    WriteTime(root, "removed_total", report.removed_total);
    root.Set("skipped_short", Value::MakeInt(report.skipped_short));
    root.Set("skipped_already_tight",
             Value::MakeInt(report.skipped_already_tight));
    WriteTime(root, "head_air", report.head_air);
    WriteTime(root, "tail_air", report.tail_air);
    Value closed = Value::MakeArray();
    for (const TightenedPause& pause : report.pauses) {
        Value entry = Value::MakeObject();
        WriteTime(entry, "gap_start", pause.gap_start);
        WriteTime(entry, "gap_end", pause.gap_end);
        WriteTime(entry, "removed", pause.removed);
        closed.Push(std::move(entry));
    }
    root.Set("closed", std::move(closed));
    return root.Dump();
}
