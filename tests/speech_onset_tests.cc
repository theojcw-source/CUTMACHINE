#include "Document.h"
#include "SpeechOnset.h"
#include "Ulid.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

// Niveau attendu pour une sinusoïde d'amplitude `peak` : RMS = peak/sqrt(2).
int64_t ExpectedSineLevel(int16_t peak) {
    return static_cast<int64_t>(static_cast<double>(peak) / std::sqrt(2.0) /
                                32768.0 *
                                static_cast<double>(kSpeechLevelScale));
}

std::vector<int16_t> Sine(size_t count, int16_t peak) {
    std::vector<int16_t> samples(count);
    for (size_t index = 0; index < count; ++index)
        samples[index] = static_cast<int16_t>(
            peak * std::sin(2.0 * 3.14159265358979 * index / 32.0));
    return samples;
}

std::string Quote(const std::filesystem::path& path) {
    std::string result = "'";
    for (char character : path.string())
        result += character == '\'' ? "'\\''" : std::string(1, character);
    return result + "'";
}

DocumentClip Clip(RationalTime sourceIn, RationalTime duration) {
    DocumentClip clip;
    clip.id = "01K30000000000000000000020";
    clip.source_id = "01K30000000000000000000001";
    clip.source_in = sourceIn;
    clip.duration = duration;
    clip.timeline_in = RationalTime{0, 25};
    return clip;
}

// Enveloppe synthétique : `silence` fenêtres de souffle, puis de la parole.
SpeechOnsetReport Envelope(int64_t silence, int64_t speech, int64_t room,
                           int64_t voice) {
    SpeechOnsetReport report;
    report.media_id = "01K30000000000000000000001";
    report.windows_per_second = 50;
    report.decode_sample_rate = 16000;
    for (int64_t index = 0; index < silence; ++index)
        report.levels.push_back(room);
    for (int64_t index = 0; index < speech; ++index)
        report.levels.push_back(voice);
    report.speech_level = SpeechLevelPercentile(report.levels, 90);
    report.noise_floor = SpeechLevelPercentile(report.levels, 5);
    return report;
}

}  // namespace

int main() {
    const MediaRate kPal{25, 1};

    // --- RMS ---------------------------------------------------------------
    Check(WindowRmsLevel(nullptr, 0) == 0, "aucun échantillon donne zéro");
    std::vector<int16_t> silence(320, 0);
    Check(WindowRmsLevel(silence.data(), silence.size()) == 0,
          "du silence numérique donne zéro");
    std::vector<int16_t> loud = Sine(320, 30000);
    const int64_t measured = WindowRmsLevel(loud.data(), loud.size());
    const int64_t expected = ExpectedSineLevel(30000);
    Check(std::llabs(measured - expected) < expected / 50,
          "la RMS d'une sinusoïde vaut crête/racine(2), à 2 % près");
    std::vector<int16_t> quiet = Sine(320, 3000);
    Check(WindowRmsLevel(quiet.data(), quiet.size()) < measured / 5,
          "une sinusoïde dix fois plus faible mesure bien plus bas");
    // Déterminisme : c'est la raison d'être de l'arithmétique entière.
    Check(WindowRmsLevel(loud.data(), loud.size()) == measured,
          "deux mesures des mêmes échantillons donnent le même entier");

    // --- Percentile --------------------------------------------------------
    Check(SpeechLevelPercentile({}, 50) == 0, "une liste vide donne zéro");
    Check(SpeechLevelPercentile({5, 1, 3}, 50) == 3,
          "la médiane est le rang le plus proche, pas une interpolation");
    Check(SpeechLevelPercentile({1, 2, 3, 4, 5, 6, 7, 8, 9, 10}, 90) == 9,
          "le 90e centile de dix valeurs est la neuvième");
    Check(SpeechLevelPercentile({4, 4, 4}, 5) == 4,
          "un centile bas ne descend pas sous la plus petite valeur");

    // --- Attaque soutenue --------------------------------------------------
    const std::vector<int64_t> spike = {0, 0, 900, 0, 0, 900, 900, 900, 900};
    Check(FirstSustainedWindow(spike, 0, 500, 4) == 5,
          "un pic isolé n'est pas une attaque : seul le passage tenu compte");
    Check(FirstSustainedWindow(spike, 0, 500, 1) == 2,
          "sans exigence de tenue, le premier dépassement suffit");
    Check(FirstSustainedWindow(spike, 6, 500, 2) == 6,
          "la recherche démarre bien au point demandé");
    Check(FirstSustainedWindow({0, 0, 0}, 0, 500, 2) == -1,
          "une enveloppe sans parole se refuse au lieu de rendre zéro");

    // --- Résumé par clip ---------------------------------------------------
    // 34 fenêtres de souffle (0,68 s) puis de la parole : le cas mesuré sur
    // C7429, où whisper annonçait le premier mot à l'image 0.
    const SpeechOnsetReport report = Envelope(34, 200, 3000, 200000);
    ClipSpeechOnset summary;
    std::string error;
    Check(SummarizeClipSpeechOnset(Clip({0, 25}, {200, 25}), report, {}, kPal,
                                   summary, error),
          "un clip s'analyse : " + error);
    Check(summary.measured, "l'attaque est mesurée");
    Check(summary.onset == RationalTime{34, 50},
          "l'attaque tombe sur la première fenêtre voisée");
    Check(summary.lead_in == RationalTime{34, 50},
          "le blanc en entrée est la distance entre l'entrée et l'attaque");
    // 34 fenêtres = 0,68 s ; moins 80 ms d'amorce = 0,60 s = 15 images.
    Check(summary.suggested_trim == RationalTime{15, 25},
          "la coupe proposée retire le blanc moins l'amorce, en images "
          "entières de séquence");
    Check(!summary.tight, "un blanc de 0,68 s n'est pas considéré serré");

    // Un clip qui entre déjà sur le mot ne doit rien proposer.
    ClipSpeechOnset already;
    Check(SummarizeClipSpeechOnset(Clip({34, 50}, {200, 50}), report, {}, kPal,
                                   already, error) &&
              already.measured && already.tight &&
              already.suggested_trim == RationalTime{0, 25},
          "une entrée déjà sur le mot ne propose aucune coupe : " + error);

    // --- Refus plutôt qu'invention -----------------------------------------
    ClipSpeechOnset rejected;
    DocumentClip foreign = Clip({0, 25}, {200, 25});
    foreign.source_id = "01K30000000000000000000009";
    Check(
        !SummarizeClipSpeechOnset(foreign, report, {}, kPal, rejected, error) &&
            error.find("another source") != std::string::npos,
        "un rapport d'une autre source est refusé : " + error);

    // Du souffle d'un bout à l'autre : le 90e centile n'est que le haut du
    // bruit, et l'appeler « parole » inventerait une attaque.
    const SpeechOnsetReport flat = Envelope(100, 100, 3000, 3100);
    ClipSpeechOnset unmeasured;
    Check(SummarizeClipSpeechOnset(Clip({0, 25}, {200, 25}), flat, {}, kPal,
                                   unmeasured, error) &&
              !unmeasured.measured &&
              unmeasured.detail.find("dynamic range") != std::string::npos,
          "une source sans dynamique est déclarée non mesurée : " +
              unmeasured.detail);

    ClipSpeechOnset outside;
    Check(SummarizeClipSpeechOnset(Clip({5000, 25}, {200, 25}), report, {},
                                   kPal, outside, error) &&
              !outside.measured &&
              outside.detail.find("no analysis window") != std::string::npos,
          "un clip hors de l'enveloppe est déclaré non mesuré, pas serré");

    // --- Aller-retour du cache ---------------------------------------------
    const std::string json = SerializeSpeechOnset(report);
    SpeechOnsetReport parsed;
    Check(DeserializeSpeechOnset(json, parsed, error) &&
              parsed.media_id == report.media_id &&
              parsed.windows_per_second == report.windows_per_second &&
              parsed.decode_sample_rate == report.decode_sample_rate &&
              parsed.speech_level == report.speech_level &&
              parsed.noise_floor == report.noise_floor &&
              parsed.levels == report.levels,
          "le cache fait l'aller-retour sans perte : " + error);
    Check(SerializeSpeechOnset(parsed) == json,
          "deux sérialisations donnent les mêmes octets");
    Check(json.find('.') == std::string::npos,
          "aucun flottant n'atteint le fichier de cache");
    SpeechOnsetReport old;
    Check(!DeserializeSpeechOnset("{\"version\":0,\"media_id\":\"x\"}", old,
                                  error),
          "une version de cache inconnue est refusée, pas migrée");

    // --- Passe d'analyse, avec un vrai FFmpeg -------------------------------
    // Le cœur pur ci-dessus ne prouve pas que le décodage produit la bonne
    // enveloppe. Fixture : 1,2 s de silence, puis 2 s de ton. L'attaque doit
    // tomber sur la frontière, pas ailleurs.
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / (GenerateUlid() + "-onset");
    std::filesystem::create_directories(root);
    const std::filesystem::path fixture = root / "voice.wav";
    const std::filesystem::path cache = root / "voice.json";
    const std::string makeFixture =
        Quote(FFMPEG_EXECUTABLE) +
        " -hide_banner -loglevel error -f lavfi -i "
        "'sine=frequency=220:duration=2' -af "
        "'adelay=1200:all=1,apad=pad_dur=0' -ar 16000 -ac 1 -y " +
        Quote(fixture);
    if (std::system(makeFixture.c_str()) != 0) {
        std::cerr << "FAIL: impossible de générer la fixture audio\n";
        std::filesystem::remove_all(root);
        return 1;
    }
    SpeechOnsetSettings settings;
    settings.ffmpeg_path = FFMPEG_EXECUTABLE;
    MediaTaskManager manager;
    const std::string mediaId = "01KQ00000000000000000000VO";
    manager.Enqueue(MediaTaskKind::SpeechOnset, "voix",
                    [&](MediaTaskContext& context, std::string& taskError) {
                        return GenerateSpeechOnset(
                            fixture.string(), cache.string(), mediaId, settings,
                            context, taskError);
                    });
    if (!manager.WaitForIdle(120000)) {
        std::cerr << "FAIL: l'analyse de parole a expiré\n";
        std::filesystem::remove_all(root);
        return 1;
    }
    SpeechOnsetReport decoded;
    std::string decodeError;
    Check(LoadSpeechOnset(cache.string(), decoded, decodeError),
          "la passe d'analyse écrit un cache lisible : " + decodeError);
    if (!decoded.levels.empty()) {
        Check(decoded.windows_per_second == 50 &&
                  decoded.decode_sample_rate == 16000,
              "la grille d'analyse est celle demandée");
        Check(decoded.speech_level > decoded.noise_floor * 4,
              "le ton se détache nettement du silence");
        DocumentClip whole = Clip({0, 25}, {80, 25});
        whole.source_id = mediaId;
        ClipSpeechOnset decodedSummary;
        std::string summaryError;
        Check(SummarizeClipSpeechOnset(whole, decoded, {}, kPal, decodedSummary,
                                       summaryError) &&
                  decodedSummary.measured,
              "l'enveloppe décodée se résume : " + summaryError);
        // 1,2 s = fenêtre 60. Une fenêtre de tolérance de chaque côté suffit :
        // le décodeur ne place pas la frontière au demi-échantillon près.
        const int64_t onsetWindow = decodedSummary.onset.value;
        Check(onsetWindow >= 58 && onsetWindow <= 63,
              "l'attaque tombe sur la frontière silence/ton (fenêtre " +
                  std::to_string(onsetWindow) + ", attendue vers 60)");
    }
    std::filesystem::remove_all(root);

    if (failures != 0) {
        std::cerr << failures << " assertion(s) en échec\n";
        return 1;
    }
    std::cout << "All speech onset tests passed\n";
    return 0;
}
