#include "BoundaryAir.h"

#include "EditLog.h"
#include "Json.h"
#include "Operations.h"

#include <iostream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

// B4 -- ROADMAP.md. Synthetic B1 envelopes reproduce the measured failure:
// each half of a junction is below the per-clip threshold, while the silence
// heard across the cut is above it. The old one-side-at-a-time loop needed
// four passes and moved the cut the wrong way; the compound operation below
// measures both original sides, applies once, and is already converged when
// resolved again.

namespace {

int failures = 0;

constexpr int64_t kRoom = 20000;
constexpr int64_t kVoice = 200000;
const char kSourceA[] = "01K40000000000000000000001";
const char kSourceB[] = "01K40000000000000000000002";

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

SpeechOnsetReport Envelope(
    const std::string& sourceId,
    const std::vector<std::pair<int64_t, int64_t>>& blocks) {
    SpeechOnsetReport report;
    report.media_id = sourceId;
    report.windows_per_second = 50;
    report.decode_sample_rate = 16000;
    for (const auto& block : blocks) {
        for (int64_t index = 0; index < block.second; ++index)
            report.levels.push_back(block.first);
    }
    report.speech_level = SpeechLevelPercentile(report.levels, 90);
    report.noise_floor = SpeechLevelPercentile(report.levels, 5);
    std::string error;
    Check(BuildSpeechGroups(report.levels, report.windows_per_second,
                            report.speech_level, report.noise_floor, {},
                            report.groups, error),
          "les groupes synthétiques se construisent : " + error);
    return report;
}

DocumentClip Clip(const std::string& id, const std::string& sourceId,
                  RationalTime sourceIn, RationalTime duration,
                  RationalTime timelineIn, const std::string& link = {}) {
    DocumentClip clip;
    clip.id = id;
    clip.source_id = sourceId;
    clip.source_in = sourceIn;
    clip.duration = duration;
    clip.timeline_in = timelineIn;
    clip.link_group_id = link;
    return clip;
}

}  // namespace

int main() {
    const MediaRate kPal{25, 1};
    std::string error;

    // One clip: both measured edges are removed together, its A/V pair gets
    // identical source bounds, and a third sync-locked track moves by the
    // aggregate delta. Undo must restore canonical bytes, not just geometry.
    {
        Document document;
        document.sources = {
            {kSourceA, "A.wav", kPal, {1000, 25}},
        };
        const std::string videoId = "01K40000000000000000000010";
        const std::string audioId = "01K40000000000000000000011";
        const std::string nextVideoId = "01K40000000000000000000012";
        const std::string nextAudioId = "01K40000000000000000000013";
        const std::string overlayId = "01K40000000000000000000014";
        const std::string link = "01K40000000000000000000020";
        const DocumentClip video =
            Clip(videoId, kSourceA, {0, 25}, {50, 25}, {0, 25}, link);
        const DocumentClip audio =
            Clip(audioId, kSourceA, {0, 25}, {50, 25}, {0, 25}, link);
        document.sequence.tracks = {
            {"01K40000000000000000000030",
             "video",
             0,
             {video,
              Clip(nextVideoId, kSourceA, {100, 25}, {10, 25}, {50, 25})}},
            {"01K40000000000000000000031",
             "audio",
             1,
             {audio,
              Clip(nextAudioId, kSourceA, {100, 25}, {10, 25}, {50, 25})}},
            {"01K40000000000000000000032",
             "video",
             2,
             {Clip(overlayId, kSourceA, {200, 25}, {10, 25}, {50, 25})}},
        };
        const std::string before = document.SaveToString();
        const SpeechOnsetReport envelope =
            Envelope(kSourceA, {{kRoom, 20}, {kVoice, 50}, {kRoom, 30}});
        BoundaryAirSettings settings;
        TrimBoundaryAirOperation operation;
        BoundaryAirReport report;
        Check(ResolveBoundaryAir(video, envelope, settings, kPal, {}, operation,
                                 report, error),
              "les deux bords se résolvent : " + error);
        Check(operation.trims.size() == 2,
              "la tête et la queue partent dans une seule opération");
        if (operation.trims.size() == 2) {
            Check(operation.trims[0].edge == TrimEdge::Head &&
                      operation.trims[0].delta == RationalTime{7, 25},
                  "trois images sont gardées avant l'attaque");
            Check(operation.trims[1].edge == TrimEdge::Tail &&
                      operation.trims[1].delta == RationalTime{-12, 25},
                  "trois images sont gardées après la décroissance");
        }
        for (BoundaryAirTrim& trim : operation.trims)
            trim.linked_clip_ids = LinkedBoundaryClipIds(document, video);
        operation.sync_track_ids =
            BoundarySyncTrackIds(document, operation.trims);
        Check(operation.sync_track_ids.size() == 1 &&
                  operation.sync_track_ids[0] == "01K40000000000000000000032",
              "la piste sync-lock hors paire suit automatiquement");

        const std::string canonical = SerializeOperation(operation);
        Operation parsed = RemoveClipOperation{};
        EditError editError = EditError::None;
        std::string message;
        Check(DeserializeOperation(canonical, parsed, editError, message) &&
                  SerializeOperation(parsed) == canonical,
              "TrimBoundaryAir se sérialise canoniquement : " + message);

        EditLog log;
        Check(log.Apply(document, operation, editError, message),
              "l'opération de bordure s'applique : " + message);
        const DocumentClip* trimmedVideo = document.FindClip(videoId);
        const DocumentClip* trimmedAudio = document.FindClip(audioId);
        Check(trimmedVideo && trimmedAudio &&
                  trimmedVideo->source_in == RationalTime{7, 25} &&
                  trimmedVideo->duration == RationalTime{31, 25} &&
                  trimmedAudio->source_in == trimmedVideo->source_in &&
                  trimmedAudio->duration == trimmedVideo->duration,
              "la paire A/V perd exactement les mêmes bordures");
        Check(document.FindClip(nextVideoId)->timeline_in ==
                      RationalTime{31, 25} &&
                  document.FindClip(nextAudioId)->timeline_in ==
                      RationalTime{31, 25} &&
                  document.FindClip(overlayId)->timeline_in ==
                      RationalTime{31, 25},
              "toutes les pistes synchronisées avancent de 19 images");
        Check(log.AppliedCount() == 1,
              "les deux bords ne créent qu'une entrée de journal");
        Check(log.Undo(document, editError, message),
              "le geste complet s'annule : " + message);
        Check(document.SaveToString() == before,
              "undo restaure le document octet pour octet");
        Check(log.Redo(document, editError, message),
              "le geste complet se rejoue : " + message);
    }

    // Four-pass divergence fixture: 280 ms on either side is below the
    // 300 ms per-boundary minimum, but the listener hears a 560 ms hole.
    // Individual cleanup does nothing; the junction resolver trims both
    // original sides in one operation and a second resolution is a no-op.
    {
        Document document;
        document.sources = {
            {kSourceA, "left.wav", kPal, {1000, 25}},
            {kSourceB, "right.wav", kPal, {1000, 25}},
        };
        const std::string leftId = "01K40000000000000000000040";
        const std::string rightId = "01K40000000000000000000041";
        const DocumentClip left =
            Clip(leftId, kSourceA, {0, 25}, {32, 25}, {0, 25});
        const DocumentClip right =
            Clip(rightId, kSourceB, {0, 25}, {32, 25}, {32, 25});
        document.sequence.tracks = {
            {"01K40000000000000000000042", "audio", 0, {left, right}},
        };
        const SpeechOnsetReport leftEnvelope =
            Envelope(kSourceA, {{kVoice, 50}, {kRoom, 14}});
        const SpeechOnsetReport rightEnvelope =
            Envelope(kSourceB, {{kRoom, 14}, {kVoice, 50}});
        BoundaryAirSettings settings;
        settings.keep_frames = 2;

        TrimBoundaryAirOperation isolatedLeft;
        TrimBoundaryAirOperation isolatedRight;
        BoundaryAirReport leftReport;
        BoundaryAirReport rightReport;
        Check(ResolveBoundaryAir(left, leftEnvelope, settings, kPal, {},
                                 isolatedLeft, leftReport, error) &&
                  ResolveBoundaryAir(right, rightEnvelope, settings, kPal, {},
                                     isolatedRight, rightReport, error) &&
                  isolatedLeft.trims.empty() && isolatedRight.trims.empty(),
              "chaque demi-creux isolé reste sous le seuil mesuré");

        TrimBoundaryAirOperation operation;
        JunctionAirReport report;
        Check(ResolveJunctionAir(left, leftEnvelope, kPal, right, rightEnvelope,
                                 kPal, settings, {}, operation, report, error),
              "la jonction complète se résout : " + error);
        Check(report.combined_air == RationalTime{28, 50} &&
                  operation.trims.size() == 2,
              "les 560 ms sont mesurées ensemble et les deux côtés partent");

        EditLog log;
        EditError editError = EditError::None;
        std::string message;
        Check(log.Apply(document, operation, editError, message),
              "la fermeture atomique s'applique : " + message);
        const DocumentClip* newLeft = document.FindClip(leftId);
        const DocumentClip* newRight = document.FindClip(rightId);
        Check(newLeft && newRight &&
                  newLeft->duration == RationalTime{27, 25} &&
                  newRight->source_in == RationalTime{5, 25} &&
                  newRight->duration == RationalTime{27, 25} &&
                  newLeft->timeline_in.add(newLeft->duration) ==
                      newRight->timeline_in,
              "queue et tête ferment la jonction sans créer de trou");

        TrimBoundaryAirOperation secondPass;
        JunctionAirReport secondReport;
        Check(ResolveJunctionAir(*newLeft, leftEnvelope, kPal, *newRight,
                                 rightEnvelope, kPal, settings, {}, secondPass,
                                 secondReport, error) &&
                  secondPass.trims.empty() &&
                  secondReport.combined_air == RationalTime{4, 25},
              "une seconde passe est déjà convergée à quatre images gardées");
    }

    // If either constituent trim is invalid, ApplyOperation's candidate copy
    // prevents the first from leaking into the document or the event log.
    {
        Document document;
        document.sources = {
            {kSourceA, "A.wav", kPal, {1000, 25}},
        };
        document.sequence.tracks = {
            {"01K40000000000000000000050",
             "audio",
             0,
             {Clip("01K40000000000000000000051", kSourceA, {0, 25}, {20, 25},
                   {0, 25})}},
        };
        const std::string before = document.SaveToString();
        TrimBoundaryAirOperation invalid{
            {{"01K40000000000000000000051", TrimEdge::Head, {2, 25}, {}},
             {"01K40000000000000000000999", TrimEdge::Tail, {-2, 25}, {}}},
            {},
            {}};
        EditLog log;
        EditError editError = EditError::None;
        std::string message;
        Check(!log.Apply(document, invalid, editError, message),
              "une seconde bordure invalide refuse tout le geste");
        Check(document.SaveToString() == before && log.AppliedCount() == 0,
              "le refus atomique ne laisse ni mutation ni journal");
    }

    // CLI/MCP integers are untrusted. Converting keep_frames to the source
    // timebase refuses before multiplication can wrap an exact duration.
    {
        const SpeechOnsetReport envelope =
            Envelope(kSourceA, {{kRoom, 20}, {kVoice, 50}, {kRoom, 20}});
        BoundaryAirSettings settings;
        settings.keep_frames = std::numeric_limits<int64_t>::max();
        const MediaRate fractionalRate{24000, 1001};
        TrimBoundaryAirOperation operation;
        BoundaryAirReport report;
        Check(
            !ResolveBoundaryAir(Clip("01K40000000000000000000060", kSourceA,
                                     {0, 25}, {45, 25}, {0, 25}),
                                envelope, settings, fractionalRate, {},
                                operation, report, error) &&
                error.find("exact time range") != std::string::npos,
            "un keep_frames débordant est refusé avant conversion : " + error);
    }

    // Reports are stable JSON values for both headless surfaces.
    {
        BoundaryAirReport report{{10, 25}, {12, 25}, {7, 25}, {9, 25}};
        JunctionAirReport junction{
            {7, 25}, {7, 25}, {14, 25}, {5, 25}, {5, 25}};
        mcp_json::Value parsed;
        std::string parseError;
        Check(mcp_json::Value::Parse(SerializeBoundaryAirReport(report), parsed,
                                     parseError) &&
                  parsed.IsObject(),
              "le rapport de plan est du JSON valide : " + parseError);
        Check(mcp_json::Value::Parse(SerializeJunctionAirReport(junction),
                                     parsed, parseError) &&
                  parsed.IsObject(),
              "le rapport de jonction est du JSON valide : " + parseError);
    }

    if (failures != 0) {
        std::cerr << failures << " assertion(s) en échec\n";
        return 1;
    }
    std::cout << "Tous les tests d'air de bordure passent\n";
    return 0;
}
