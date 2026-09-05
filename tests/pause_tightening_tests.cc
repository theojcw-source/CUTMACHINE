#include "PauseTightening.h"

#include "EditLog.h"
#include "Json.h"
#include "Operations.h"

#include <iostream>
#include <string>
#include <vector>

// QC-2026-09 A2. Le cœur est pur : une enveloppe synthétique, aucun FFmpeg,
// aucun modèle. Ce qui est épinglé ici, c'est ce que le ticket promet — les
// creux internes se referment, les bords ne bougent pas, l'arrondi ne mord
// jamais sur la parole — et la réversibilité, puisque la fonction produit une
// opération et rien d'autre.

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

constexpr int64_t kRoom = 20000;
constexpr int64_t kVoice = 200000;
const char kSourceId[] = "01K30000000000000000000001";

// Enveloppe à 50 fenêtres/s décrite par une suite (niveau, nombre de
// fenêtres), pour que chaque cas se lise comme la forme d'onde qu'il
// représente.
SpeechOnsetReport Envelope(
    const std::vector<std::pair<int64_t, int64_t>>& blocks) {
    SpeechOnsetReport report;
    report.media_id = kSourceId;
    report.windows_per_second = 50;
    report.decode_sample_rate = 16000;
    for (const auto& block : blocks)
        for (int64_t index = 0; index < block.second; ++index)
            report.levels.push_back(block.first);
    report.speech_level = SpeechLevelPercentile(report.levels, 90);
    report.noise_floor = SpeechLevelPercentile(report.levels, 5);
    return report;
}

DocumentClip Clip(RationalTime sourceIn, RationalTime duration) {
    DocumentClip clip;
    clip.id = "01K30000000000000000000020";
    clip.source_id = kSourceId;
    clip.source_in = sourceIn;
    clip.duration = duration;
    clip.timeline_in = RationalTime{0, 25};
    return clip;
}

}  // namespace

int main() {
    const MediaRate kPal{25, 1};
    PauseTighteningSettings defaults;
    std::string error;

    // --- Un creux interne se referme ---------------------------------------
    // 1 s de parole, 1 s de silence, 1 s de parole. À 25 i/s le creux fait 25
    // images ; on en garde 6, réparties 3/3, donc 19 images partent.
    {
        const SpeechOnsetReport envelope =
            Envelope({{kVoice, 50}, {kRoom, 50}, {kVoice, 50}});
        RemoveWordsOperation operation;
        PauseTighteningReport report;
        Check(
            ResolvePauseTightening(Clip({0, 25}, {75, 25}), envelope, defaults,
                                   kPal, {}, operation, report, error),
            "un plan à un creux se résout : " + error);
        Check(operation.ranges.size() == 1,
              "un seul creux donne une seule plage");
        Check(operation.clip_id == "01K30000000000000000000020",
              "l'opération vise bien le plan demandé");
        if (operation.ranges.size() == 1) {
            Check(operation.ranges[0].source_start == RationalTime{28, 25},
                  "la coupe démarre 3 images après le début du creux");
            Check(operation.ranges[0].source_end == RationalTime{47, 25},
                  "et s'arrête 3 images avant sa fin");
        }
        Check(report.pauses.size() == 1 &&
                  report.removed_total == RationalTime{19, 25},
              "le rapport annonce les 19 images retirées");
        Check(report.head_air == RationalTime{0, 50} &&
                  report.tail_air == RationalTime{0, 50},
              "un plan qui entre et sort sur la parole n'a pas d'air aux "
              "bords");
    }

    // --- Les bords ne sont pas la question de cet outil ---------------------
    {
        const SpeechOnsetReport envelope = Envelope({{kRoom, 40},
                                                     {kVoice, 50},
                                                     {kRoom, 50},
                                                     {kVoice, 50},
                                                     {kRoom, 30}});
        RemoveWordsOperation operation;
        PauseTighteningReport report;
        Check(
            ResolvePauseTightening(Clip({0, 25}, {110, 25}), envelope, defaults,
                                   kPal, {}, operation, report, error),
            "un plan bordé de silence se résout : " + error);
        Check(operation.ranges.size() == 1,
              "seul le creux interne est coupé : l'air de tête et de queue "
              "relève d'un rognage, pas d'un ripple interne");
        Check(report.head_air == RationalTime{40, 50} &&
                  report.tail_air == RationalTime{30, 50},
              "l'air laissé aux bords est mesuré et publié");
        if (operation.ranges.size() == 1) {
            Check(operation.ranges[0].source_start == RationalTime{48, 25},
                  "la coupe interne démarre après les 3 images gardées");
        }
    }

    // --- Un souffle n'est pas une hésitation --------------------------------
    {
        // 200 ms de creux : sous les 400 ms par défaut.
        const SpeechOnsetReport envelope =
            Envelope({{kVoice, 50}, {kRoom, 10}, {kVoice, 50}});
        RemoveWordsOperation operation;
        PauseTighteningReport report;
        Check(
            ResolvePauseTightening(Clip({0, 25}, {55, 25}), envelope, defaults,
                                   kPal, {}, operation, report, error),
            "un plan sans hésitation se résout : " + error);
        Check(operation.ranges.empty() && report.skipped_short == 1,
              "un creux plus court que le seuil est compté, pas coupé");

        PauseTighteningSettings aggressive = defaults;
        aggressive.minimum_gap_milliseconds = 150;
        RemoveWordsOperation cut;
        PauseTighteningReport second;
        Check(
            ResolvePauseTightening(Clip({0, 25}, {55, 25}), envelope,
                                   aggressive, kPal, {}, cut, second, error) &&
                cut.ranges.empty() && second.skipped_already_tight == 1,
            "abaisser le seuil ne coupe pas un creux déjà plus court que ce "
            "qu'on garde : " +
                error);
    }

    // --- L'arrondi ne mord jamais sur la parole -----------------------------
    {
        // Le creux commence à la fenêtre 51 et finit à la 99 : ni l'une ni
        // l'autre ne tombe sur une image de 25 i/s (2 fenêtres = 1 image).
        const SpeechOnsetReport envelope =
            Envelope({{kVoice, 51}, {kRoom, 48}, {kVoice, 50}});
        PauseTighteningSettings noKeep = defaults;
        noKeep.keep_frames = 0;
        RemoveWordsOperation operation;
        PauseTighteningReport report;
        Check(ResolvePauseTightening(Clip({0, 25}, {75, 25}), envelope, noKeep,
                                     kPal, {}, operation, report, error) &&
                  operation.ranges.size() == 1,
              "un creux mal aligné se résout quand même : " + error);
        if (operation.ranges.size() == 1) {
            // 51/50 s arrondi vers le haut donne l'image 26 ; 99/50 s arrondi
            // vers le bas donne l'image 49. Les deux tombent à l'intérieur du
            // silence, jamais sur la parole qui l'encadre.
            Check(operation.ranges[0].source_start == RationalTime{26, 25},
                  "le début de coupe s'arrondit vers l'intérieur du silence");
            Check(operation.ranges[0].source_end == RationalTime{49, 25},
                  "la fin de coupe aussi");
        }
    }

    // --- Refus plutôt qu'invention -----------------------------------------
    {
        const SpeechOnsetReport envelope =
            Envelope({{kVoice, 50}, {kRoom, 50}, {kVoice, 50}});
        RemoveWordsOperation operation;
        PauseTighteningReport report;
        DocumentClip foreign = Clip({0, 25}, {75, 25});
        foreign.source_id = "01K30000000000000000000009";
        Check(!ResolvePauseTightening(foreign, envelope, defaults, kPal, {},
                                      operation, report, error) &&
                  error.find("another source") != std::string::npos,
              "une enveloppe d'une autre source est refusée : " + error);

        SpeechOnsetReport empty;
        empty.media_id = kSourceId;
        Check(!ResolvePauseTightening(Clip({0, 25}, {75, 25}), empty, defaults,
                                      kPal, {}, operation, report, error),
              "une enveloppe sans grille d'analyse est refusée");

        PauseTighteningSettings negative = defaults;
        negative.keep_frames = -1;
        Check(
            !ResolvePauseTightening(Clip({0, 25}, {75, 25}), envelope, negative,
                                    kPal, {}, operation, report, error),
            "garder un nombre négatif d'images est refusé");

        Check(!ResolvePauseTightening(Clip({5000, 25}, {75, 25}), envelope,
                                      defaults, kPal, {}, operation, report,
                                      error),
              "un plan hors de l'enveloppe est refusé, pas déclaré serré");
    }

    // --- Réversibilité ------------------------------------------------------
    // La promesse du ticket : « le tout en une opération réversible ». On la
    // vérifie comme AGENTS.md l'exige, octet à octet après annulation, et sur
    // la paire A/V liée, qui est ce que Q4b a rendu possible.
    {
        Document document;
        document.sources = {{kSourceId, "rush.MP4", {25, 1}, {1000, 25}}};
        DocumentClip picture = Clip({0, 25}, {75, 25});
        picture.link_group_id = "01K30000000000000000000005";
        DocumentClip sound = picture;
        sound.id = "01K30000000000000000000021";
        document.sequence.tracks = {
            {"01K30000000000000000000002", "video", 0, {picture}},
            {"01K30000000000000000000003", "audio", 1, {sound}},
        };
        const std::string before = document.SaveToString();

        const SpeechOnsetReport envelope =
            Envelope({{kVoice, 50}, {kRoom, 50}, {kVoice, 50}});
        RemoveWordsOperation operation;
        PauseTighteningReport report;
        Check(ResolvePauseTightening(picture, envelope, defaults, kPal, {},
                                     operation, report, error),
              "le plan image se résout : " + error);
        operation.linked_clip_ids =
            LinkedClipIdsCoveringRanges(document, picture, operation.ranges);
        Check(operation.linked_clip_ids.size() == 1 &&
                  operation.linked_clip_ids[0] == "01K30000000000000000000021",
              "le son lié est emporté par la même coupe");

        EditLog log;
        EditError editError = EditError::None;
        std::string message;
        Check(log.Apply(document, operation, editError, message),
              "l'opération s'applique : " + message);
        Check(document.SaveToString() != before, "le montage a bien changé");
        const DocumentTrack* video =
            document.FindTrack("01K30000000000000000000002");
        const DocumentTrack* audio =
            document.FindTrack("01K30000000000000000000003");
        Check(video != nullptr && audio != nullptr &&
                  video->clips.size() == audio->clips.size(),
              "image et son sont découpés en autant de fragments");
        Check(log.Undo(document, editError, message),
              "l'opération s'annule : " + message);
        Check(document.SaveToString() == before,
              "l'annulation rend un document octet pour octet identique");
        Check(log.Redo(document, editError, message),
              "et se rétablit : " + message);
    }

    // --- Sérialisation du rapport ------------------------------------------
    {
        const SpeechOnsetReport envelope =
            Envelope({{kVoice, 50}, {kRoom, 50}, {kVoice, 50}});
        RemoveWordsOperation operation;
        PauseTighteningReport report;
        ResolvePauseTightening(Clip({0, 25}, {75, 25}), envelope, defaults,
                               kPal, {}, operation, report, error);
        mcp_json::Value parsed;
        std::string parseError;
        Check(mcp_json::Value::Parse(SerializePauseTighteningReport(report),
                                     parsed, parseError) &&
                  parsed.IsObject(),
              "le rapport est du JSON valide : " + parseError);
        const mcp_json::Value* closed = parsed.Find("closed");
        Check(closed != nullptr && closed->IsArray() &&
                  closed->AsArray().size() == 1,
              "il publie le creux refermé");
    }

    if (failures != 0) {
        std::cerr << failures << " assertion(s) en échec\n";
        return 1;
    }
    std::cout << "Tous les tests de resserrement des silences passent\n";
    return 0;
}
