#include "BoundaryAir.h"

#include "Json.h"
#include "Transcription.h"

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

namespace {

using mcp_json::Value;

struct MeasuredBoundaries {
    RationalTime head_air{0, 1};
    RationalTime tail_air{0, 1};
    RationalTime speech_start{0, 1};
    RationalTime speech_end{0, 1};
};

void WriteTime(Value& parent, const std::string& key,
               const RationalTime& time) {
    Value value = Value::MakeObject();
    value.Set("value", Value::MakeInt(time.value));
    value.Set("rate", Value::MakeInt(time.rate));
    parent.Set(key, std::move(value));
}

bool ValidateSettings(const BoundaryAirSettings& settings,
                      const MediaRate& sourceRate, std::string& error) {
    if (sourceRate.num <= 0 || sourceRate.den <= 0) {
        error = "source frame rate is invalid";
        return false;
    }
    if (settings.keep_frames < 0) {
        error = "frames to keep cannot be negative";
        return false;
    }
    const __int128 keepValue =
        static_cast<__int128>(settings.keep_frames) * sourceRate.den;
    if (keepValue > std::numeric_limits<int64_t>::max()) {
        error = "frames to keep exceed the exact time range";
        return false;
    }
    if (settings.minimum_air_milliseconds <= 0) {
        error = "minimum boundary air must be a positive duration";
        return false;
    }
    if (settings.speech_thresholds.dominant_sustain_windows <= 0) {
        error = "sustained speech must cover at least one analysis window";
        return false;
    }
    return true;
}

bool MeasureBoundaries(const DocumentClip& clip,
                       const SpeechOnsetReport& envelope,
                       const BoundaryAirSettings& settings,
                       MeasuredBoundaries& measured, std::string& error) {
    measured = MeasuredBoundaries{};
    if (clip.source_id != envelope.media_id) {
        error = "speech envelope belongs to another source";
        return false;
    }
    if (envelope.windows_per_second == 0 || envelope.levels.empty()) {
        error = "speech envelope has no analysis grid";
        return false;
    }
    const RationalTime clipStart = clip.source_in;
    const RationalTime clipEnd = clip.source_in.add(clip.duration);
    const int32_t grid = static_cast<int32_t>(envelope.windows_per_second);

    std::vector<SpeechGroup> rebuilt;
    const std::vector<SpeechGroup>* groups = &envelope.groups;
    if (groups->empty() ||
        envelope.group_gap_milliseconds !=
            settings.speech_thresholds.group_gap_milliseconds ||
        envelope.group_floor_db != settings.speech_thresholds.group_floor_db) {
        if (!BuildSpeechGroups(envelope.levels, envelope.windows_per_second,
                               envelope.speech_level, envelope.noise_floor,
                               settings.speech_thresholds, rebuilt, error)) {
            return false;
        }
        groups = &rebuilt;
    }

    bool found = false;
    const RationalTime minimumGroup{
        settings.speech_thresholds.dominant_sustain_windows, grid};
    for (const SpeechGroup& group : *groups) {
        const RationalTime start =
            group.start.compare(clipStart) < 0 ? clipStart : group.start;
        const RationalTime end =
            group.end.compare(clipEnd) > 0 ? clipEnd : group.end;
        if (end.compare(start) <= 0 ||
            end.sub(start).compare(minimumGroup) < 0) {
            continue;
        }
        if (!found) measured.speech_start = start;
        measured.speech_end = end;
        found = true;
    }
    if (!found) {
        error = "clip contains no sustained speech group";
        return false;
    }
    measured.head_air = measured.speech_start.sub(clipStart);
    measured.tail_air = clipEnd.sub(measured.speech_end);
    return true;
}

RationalTime KeepDuration(const BoundaryAirSettings& settings,
                          const MediaRate& sourceRate) {
    return RationalTime{settings.keep_frames * sourceRate.den, sourceRate.num};
}

bool HeadTrim(const DocumentClip& clip, const MeasuredBoundaries& measured,
              const BoundaryAirSettings& settings, const MediaRate& sourceRate,
              BoundaryAirTrim& trim, RationalTime& removed) {
    const RationalTime speechFrame =
        RoundToSourceFrame(measured.speech_start, sourceRate,
                           /*roundUp=*/false);
    const RationalTime cutEnd =
        speechFrame.sub(KeepDuration(settings, sourceRate));
    if (cutEnd.compare(clip.source_in) <= 0) return false;
    removed = cutEnd.sub(clip.source_in);
    trim = {clip.id, TrimEdge::Head, removed, {}};
    return true;
}

bool TailTrim(const DocumentClip& clip, const MeasuredBoundaries& measured,
              const BoundaryAirSettings& settings, const MediaRate& sourceRate,
              BoundaryAirTrim& trim, RationalTime& removed) {
    const RationalTime speechFrame =
        RoundToSourceFrame(measured.speech_end, sourceRate,
                           /*roundUp=*/true);
    const RationalTime cutStart =
        speechFrame.add(KeepDuration(settings, sourceRate));
    const RationalTime clipEnd = clip.source_in.add(clip.duration);
    if (cutStart.compare(clipEnd) >= 0) return false;
    removed = clipEnd.sub(cutStart);
    trim = {clip.id,
            TrimEdge::Tail,
            RationalTime{-removed.value, removed.rate},
            {}};
    return true;
}

bool AtLeastMinimum(const RationalTime& air,
                    const BoundaryAirSettings& settings) {
    return air.compare(RationalTime{settings.minimum_air_milliseconds, 1000}) >=
           0;
}

}  // namespace

bool ResolveBoundaryAir(const DocumentClip& clip,
                        const SpeechOnsetReport& envelope,
                        const BoundaryAirSettings& settings,
                        const MediaRate& sourceRate,
                        const std::vector<Ulid>& syncTrackIds,
                        TrimBoundaryAirOperation& operation,
                        BoundaryAirReport& report, std::string& error) {
    error.clear();
    operation = TrimBoundaryAirOperation{};
    report = BoundaryAirReport{};
    if (!ValidateSettings(settings, sourceRate, error)) return false;
    MeasuredBoundaries measured;
    if (!MeasureBoundaries(clip, envelope, settings, measured, error))
        return false;
    report.head_air = measured.head_air;
    report.tail_air = measured.tail_air;

    BoundaryAirTrim trim;
    if (AtLeastMinimum(measured.head_air, settings) &&
        HeadTrim(clip, measured, settings, sourceRate, trim,
                 report.head_removed)) {
        operation.trims.push_back(std::move(trim));
    }
    if (AtLeastMinimum(measured.tail_air, settings) &&
        TailTrim(clip, measured, settings, sourceRate, trim,
                 report.tail_removed)) {
        operation.trims.push_back(std::move(trim));
    }
    operation.sync_track_ids = syncTrackIds;
    return true;
}

bool ResolveJunctionAir(
    const DocumentClip& left, const SpeechOnsetReport& leftEnvelope,
    const MediaRate& leftSourceRate, const DocumentClip& right,
    const SpeechOnsetReport& rightEnvelope, const MediaRate& rightSourceRate,
    const BoundaryAirSettings& settings, const std::vector<Ulid>& syncTrackIds,
    TrimBoundaryAirOperation& operation, JunctionAirReport& report,
    std::string& error) {
    error.clear();
    operation = TrimBoundaryAirOperation{};
    report = JunctionAirReport{};
    if (!ValidateSettings(settings, leftSourceRate, error) ||
        !ValidateSettings(settings, rightSourceRate, error)) {
        return false;
    }
    if (left.timeline_in.add(left.duration).compare(right.timeline_in) != 0) {
        error = "clips do not form an exact timeline junction";
        return false;
    }
    MeasuredBoundaries leftMeasured;
    MeasuredBoundaries rightMeasured;
    if (!MeasureBoundaries(left, leftEnvelope, settings, leftMeasured, error) ||
        !MeasureBoundaries(right, rightEnvelope, settings, rightMeasured,
                           error)) {
        return false;
    }
    report.left_tail_air = leftMeasured.tail_air;
    report.right_head_air = rightMeasured.head_air;
    report.combined_air = report.left_tail_air.add(report.right_head_air);
    if (!AtLeastMinimum(report.combined_air, settings)) {
        operation.sync_track_ids = syncTrackIds;
        return true;
    }

    BoundaryAirTrim trim;
    if (TailTrim(left, leftMeasured, settings, leftSourceRate, trim,
                 report.left_removed)) {
        operation.trims.push_back(std::move(trim));
    }
    if (HeadTrim(right, rightMeasured, settings, rightSourceRate, trim,
                 report.right_removed)) {
        operation.trims.push_back(std::move(trim));
    }
    operation.sync_track_ids = syncTrackIds;
    return true;
}

std::vector<Ulid> LinkedBoundaryClipIds(const Document& document,
                                        const DocumentClip& clip) {
    std::vector<Ulid> linked;
    if (clip.link_group_id.empty()) return linked;
    for (const DocumentTrack& track : document.sequence.tracks) {
        for (const DocumentClip& candidate : track.clips) {
            if (candidate.id != clip.id &&
                candidate.link_group_id == clip.link_group_id) {
                linked.push_back(candidate.id);
            }
        }
    }
    return linked;
}

std::vector<Ulid> BoundarySyncTrackIds(
    const Document& document, const std::vector<BoundaryAirTrim>& trims) {
    std::vector<Ulid> directlyAffected;
    const auto addClipTrack = [&](const Ulid& clipId) {
        const DocumentTrack* track = document.FindTrackForClip(clipId);
        if (track && std::find(directlyAffected.begin(), directlyAffected.end(),
                               track->id) == directlyAffected.end()) {
            directlyAffected.push_back(track->id);
        }
    };
    for (const BoundaryAirTrim& trim : trims) {
        addClipTrack(trim.clip_id);
        for (const Ulid& linkedId : trim.linked_clip_ids)
            addClipTrack(linkedId);
    }

    std::vector<Ulid> sync;
    for (const DocumentTrack& track : document.sequence.tracks) {
        if (track.sync_lock &&
            std::find(directlyAffected.begin(), directlyAffected.end(),
                      track.id) == directlyAffected.end()) {
            sync.push_back(track.id);
        }
    }
    return sync;
}

std::string SerializeBoundaryAirReport(const BoundaryAirReport& report) {
    Value root = Value::MakeObject();
    WriteTime(root, "head_air", report.head_air);
    WriteTime(root, "tail_air", report.tail_air);
    WriteTime(root, "head_removed", report.head_removed);
    WriteTime(root, "tail_removed", report.tail_removed);
    return root.Dump();
}

std::string SerializeJunctionAirReport(const JunctionAirReport& report) {
    Value root = Value::MakeObject();
    WriteTime(root, "left_tail_air", report.left_tail_air);
    WriteTime(root, "right_head_air", report.right_head_air);
    WriteTime(root, "combined_air", report.combined_air);
    WriteTime(root, "left_removed", report.left_removed);
    WriteTime(root, "right_removed", report.right_removed);
    return root.Dump();
}
