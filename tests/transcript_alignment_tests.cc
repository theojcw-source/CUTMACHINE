#include "TranscriptAlignment.h"

#include <iostream>
#include <string>

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

const char* kMedia = "01K30000000000000000000001";

// Enveloppe à 50 fenêtres/s : `pattern` décrit une fenêtre par caractère,
// '.' pour du silence et '#' pour de la parole.
SpeechOnsetReport Envelope(const std::string& pattern) {
    SpeechOnsetReport env;
    env.media_id = kMedia;
    env.windows_per_second = 50;
    env.decode_sample_rate = 16000;
    env.speech_level = 100000;
    env.noise_floor = 1000;
    for (char c : pattern) env.levels.push_back(c == '#' ? 100000 : 500);
    return env;
}

Transcript Words(std::vector<std::pair<std::string, int64_t>> words) {
    Transcript t;
    t.media_id = kMedia;
    t.verbatim = true;
    t.source_rate = {50, 1};
    for (auto& w : words) {
        TranscriptWord word;
        word.text = w.first;
        word.start = RationalTime{w.second, 50};
        word.end = RationalTime{w.second + 5, 50};
        t.words.push_back(word);
    }
    return t;
}

}  // namespace

int main() {
    const TranscriptAlignmentSettings k{};

    // --- Segmentation ------------------------------------------------------
    {
        const SpeechOnsetReport env = Envelope("..####...###");
        const std::vector<int64_t> s = SpeechRegionStarts(env, 50000);
        Check(s.size() == 2 && s[0] == 2 && s[1] == 9,
              "les débuts de région de parole sont repérés");
        Check(SpeechRegionStarts(Envelope("...."), 50000).empty(),
              "une enveloppe muette n'a aucune région");
    }

    // --- Un mot déjà dans la parole ne bouge pas ---------------------------
    {
        // 20 fenêtres de silence (0,40 s) puis de la parole.
        const SpeechOnsetReport env =
            Envelope(std::string(20, '.') + std::string(60, '#'));
        Transcript out;
        TranscriptAlignmentReport rep;
        std::string err;
        Check(AlignTranscriptToSpeech(Words({{"bonjour", 30}}), env, k, out,
                                      rep, err),
              "alignement d'un mot correct : " + err);
        Check(rep.moved.empty() && rep.kept_in_speech == 1,
              "un mot posé sur de la parole est laissé tel quel");
        Check(out.words[0].start == RationalTime{30, 50},
              "et sa borne est inchangée");
    }

    // --- Le cas qui motive tout : un mot posé sur du silence ---------------
    {
        // Parole à partir de la fenêtre 20 (0,40 s) ; le mot prétend commencer
        // à la fenêtre 5 (0,10 s), soit 300 ms trop tôt. C'est la signature
        // mesurée sur C7429 et C6748.
        const SpeechOnsetReport env =
            Envelope(std::string(20, '.') + std::string(60, '#'));
        Transcript out;
        TranscriptAlignmentReport rep;
        std::string err;
        AlignTranscriptToSpeech(Words({{"Alors", 5}}), env, k, out, rep, err);
        Check(rep.moved.size() == 1 && rep.moved[0].moved_milliseconds == 300,
              "un mot posé sur le silence est recalé de 300 ms");
        Check(out.words[0].start == RationalTime{20, 50},
              "sur le début exact de la région de parole");
        Check(out.words[0].end.compare(out.words[0].start) >= 0,
              "et la borne de fin ne passe jamais avant la borne de début");
    }

    // --- Refus plutôt que réparation ---------------------------------------
    {
        const SpeechOnsetReport env =
            Envelope(std::string(20, '.') + std::string(60, '#'));
        Transcript out;
        TranscriptAlignmentReport rep;
        std::string err;
        // 80 ms d'écart : sous le seuil de mouvement, c'est de la
        // quantification et non une erreur.
        AlignTranscriptToSpeech(Words({{"mot", 16}}), env, k, out, rep, err);
        Check(rep.moved.empty() && rep.refused_too_small == 1,
              "un écart sous 100 ms ne déclenche aucun déplacement");
        Check(out.words[0].start == RationalTime{16, 50},
              "et la borne reste où elle était");
    }
    {
        // Mot très loin de toute parole : au-delà du plafond, on refuse.
        const SpeechOnsetReport env =
            Envelope(std::string(60, '.') + std::string(20, '#'));
        Transcript out;
        TranscriptAlignmentReport rep;
        std::string err;
        AlignTranscriptToSpeech(Words({{"fantome", 2}}), env, k, out, rep, err);
        Check(rep.moved.empty() && rep.refused_no_edge == 1,
              "un mot sans bord de parole à portée est refusé, pas traîné");
    }
    {
        // Bords de parole en 0 et en 15 ; le mot prétend commencer en 7,
        // soit 7 fenêtres de l'un et 8 de l'autre. Rien ne dit lequel.
        const SpeechOnsetReport env = Envelope(
            std::string(5, '#') + std::string(10, '.') + std::string(5, '#'));
        Transcript out;
        TranscriptAlignmentReport rep;
        std::string err;
        AlignTranscriptToSpeech(Words({{"entre", 7}}), env, k, out, rep, err);
        Check(rep.moved.empty() && rep.refused_ambiguous == 1,
              "deux bords équidistants font refuser le recalage");
    }

    // --- L'ordre des mots est une invariante -------------------------------
    {
        // Parole en 0-9 puis 20-79. Le mot 2 prétend commencer en 12, dans le
        // silence ; le bord le plus proche est 20, mais le mot 3 commence
        // déjà en 20 : le recaler créerait un doublon. Mesuré : 3 des 15
        // corrections du corpus de référence faisaient exactement ça.
        const SpeechOnsetReport env = Envelope(
            std::string(10, '#') + std::string(10, '.') + std::string(60, '#'));
        Transcript out;
        TranscriptAlignmentReport rep;
        std::string err;
        AlignTranscriptToSpeech(Words({{"un", 2}, {"deux", 12}, {"trois", 20}}),
                                env, k, out, rep, err);
        Check(rep.refused_out_of_order == 1,
              "un recalage qui doublerait le mot suivant est refusé");
        Check(out.words[1].start == RationalTime{12, 50},
              "et la borne fautive reste où elle était");
        for (size_t i = 1; i < out.words.size(); ++i)
            Check(out.words[i].start.compare(out.words[i - 1].start) > 0,
                  "les mots restent strictement ordonnés");
    }

    // --- Refus d'entrée ----------------------------------------------------
    {
        SpeechOnsetReport env = Envelope("####");
        env.media_id = "01K30000000000000000000009";
        Transcript out;
        TranscriptAlignmentReport rep;
        std::string err;
        const bool ok =
            AlignTranscriptToSpeech(Words({{"x", 0}}), env, k, out, rep, err);
        Check(!ok && err.find("different sources") != std::string::npos,
              "une enveloppe d'une autre source est refusée : " + err);
    }

    // --- Rapport -----------------------------------------------------------
    {
        const SpeechOnsetReport env =
            Envelope(std::string(20, '.') + std::string(60, '#'));
        Transcript out;
        TranscriptAlignmentReport rep;
        std::string err;
        AlignTranscriptToSpeech(Words({{"Alors", 5}, {"bien", 30}}), env, k,
                                out, rep, err);
        const std::string json = SerializeTranscriptAlignmentReport(rep);
        Check(json.find("\"moved_count\":1") != std::string::npos &&
                  json.find("\"kept_in_speech\":1") != std::string::npos &&
                  json.find("Alors") != std::string::npos,
              "le rapport dit ce qui bouge et ce qui ne bouge pas : " + json);
    }

    if (failures != 0) {
        std::cerr << failures << " assertion(s) en échec\n";
        return 1;
    }
    std::cout << "All transcript alignment tests passed\n";
    return 0;
}
