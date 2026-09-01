#include "TranscriptAlignment.h"

#include "Json.h"

#include <algorithm>
#include <cstdlib>
#include <sstream>

namespace {

using mcp_json::Value;

int64_t WindowsFor(int64_t milliseconds, uint32_t windowsPerSecond) {
    if (windowsPerSecond == 0) return 0;
    return milliseconds * static_cast<int64_t>(windowsPerSecond) / 1000;
}

int64_t Milliseconds(const RationalTime& time) {
    if (time.rate <= 0) return 0;
    return time.value * 1000 / time.rate;
}

}  // namespace

std::vector<int64_t> SpeechRegionStarts(const SpeechOnsetReport& envelope,
                                        int64_t threshold) {
    std::vector<int64_t> starts;
    bool inside = false;
    for (size_t index = 0; index < envelope.levels.size(); ++index) {
        const bool speech = envelope.levels[index] >= threshold;
        if (speech && !inside) starts.push_back(static_cast<int64_t>(index));
        inside = speech;
    }
    return starts;
}

bool AlignTranscriptToSpeech(const Transcript& transcript,
                             const SpeechOnsetReport& envelope,
                             const TranscriptAlignmentSettings& settings,
                             Transcript& aligned,
                             TranscriptAlignmentReport& report,
                             std::string& error) {
    error.clear();
    report = TranscriptAlignmentReport{};
    aligned = transcript;
    if (transcript.media_id != envelope.media_id) {
        error = "transcript and speech envelope describe different sources";
        return false;
    }
    if (envelope.windows_per_second == 0 || envelope.levels.empty()) {
        error = "speech envelope has no analysis grid";
        return false;
    }
    const uint32_t wps = envelope.windows_per_second;
    const int64_t threshold =
        envelope.speech_level * settings.speech_ratio_percent / 100;
    const std::vector<int64_t> starts = SpeechRegionStarts(envelope, threshold);
    const int64_t maxWindows =
        WindowsFor(settings.maximum_move_milliseconds, wps);

    for (size_t index = 0; index < aligned.words.size(); ++index) {
        TranscriptWord& word = aligned.words[index];
        if (word.start.rate <= 0) continue;
        // Window the word claims to start in, floored: a boundary inside
        // window k is judged by window k's level, which is the only reading
        // the envelope has for that instant.
        const int64_t claimed =
            word.start.value * static_cast<int64_t>(wps) / word.start.rate;
        const int64_t windowCount =
            static_cast<int64_t>(envelope.levels.size());
        if (claimed >= 0 && claimed < windowCount &&
            envelope.levels[static_cast<size_t>(claimed)] >= threshold) {
            ++report.kept_in_speech;
            continue;
        }
        // Candidate speech-region starts within the cap.
        std::vector<int64_t> near;
        for (int64_t start : starts)
            if (std::llabs(start - claimed) <= maxWindows)
                near.push_back(start);
        if (near.empty()) {
            ++report.refused_no_edge;
            continue;
        }
        std::sort(near.begin(), near.end(), [&](int64_t left, int64_t right) {
            return std::llabs(left - claimed) < std::llabs(right - claimed);
        });
        if (near.size() > 1) {
            const int64_t first = std::llabs(near[0] - claimed);
            const int64_t second = std::llabs(near[1] - claimed);
            const int64_t gapMs = (second - first) * 1000 / wps;
            if (gapMs < settings.ambiguity_milliseconds) {
                ++report.refused_ambiguous;
                continue;
            }
        }
        const int64_t moveWindows = near[0] - claimed;
        const int64_t moveMs = moveWindows * 1000 / static_cast<int64_t>(wps);
        if (std::llabs(moveMs) < settings.minimum_move_milliseconds) {
            ++report.refused_too_small;
            continue;
        }
        if (std::llabs(moveMs) > settings.maximum_move_milliseconds) {
            ++report.refused_too_far;
            continue;
        }
        // Expressed in the transcript's own timebase, never the envelope's:
        // the words stay in the domain every other tool reads them in.
        const RationalTime corrected{
            near[0] * word.start.rate / static_cast<int64_t>(wps),
            word.start.rate};
        // Order is an invariant of a transcript, not a preference. The
        // nearest speech edge is often the start of the region the previous
        // word already sits in, and snapping there would put two words in
        // the same place or invert them.
        if (index > 0 &&
            corrected.compare(aligned.words[index - 1].start) <= 0) {
            ++report.refused_out_of_order;
            continue;
        }
        if (index + 1 < transcript.words.size() &&
            corrected.compare(transcript.words[index + 1].start) >= 0) {
            ++report.refused_out_of_order;
            continue;
        }
        AlignedWord entry;
        entry.index = index;
        entry.text = word.text;
        entry.before = word.start;
        entry.after = corrected;
        entry.moved_milliseconds =
            Milliseconds(corrected) - Milliseconds(word.start);
        report.moved.push_back(std::move(entry));
        word.start = corrected;
        // A start pushed past its own end would invert the word. Carry the
        // end with it rather than producing a negative duration.
        if (word.end.compare(word.start) < 0) word.end = word.start;
    }
    return true;
}

std::string SerializeTranscriptAlignmentReport(
    const TranscriptAlignmentReport& report) {
    Value root = Value::MakeObject();
    root.Set("moved_count",
             Value::MakeInt(static_cast<int64_t>(report.moved.size())));
    root.Set("kept_in_speech", Value::MakeInt(report.kept_in_speech));
    root.Set("refused_ambiguous", Value::MakeInt(report.refused_ambiguous));
    root.Set("refused_no_edge", Value::MakeInt(report.refused_no_edge));
    root.Set("refused_too_small", Value::MakeInt(report.refused_too_small));
    root.Set("refused_too_far", Value::MakeInt(report.refused_too_far));
    root.Set("refused_out_of_order",
             Value::MakeInt(report.refused_out_of_order));
    Value moved = Value::MakeArray();
    for (const AlignedWord& word : report.moved) {
        Value entry = Value::MakeObject();
        entry.Set("index", Value::MakeInt(static_cast<int64_t>(word.index)));
        entry.Set("text", Value::MakeString(word.text));
        entry.Set("moved_ms", Value::MakeInt(word.moved_milliseconds));
        Value before = Value::MakeObject();
        before.Set("value", Value::MakeInt(word.before.value));
        before.Set("rate", Value::MakeInt(word.before.rate));
        entry.Set("before", std::move(before));
        Value after = Value::MakeObject();
        after.Set("value", Value::MakeInt(word.after.value));
        after.Set("rate", Value::MakeInt(word.after.rate));
        entry.Set("after", std::move(after));
        moved.Push(std::move(entry));
    }
    root.Set("moved", std::move(moved));
    return root.Dump();
}
