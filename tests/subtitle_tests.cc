#include "EditLog.h"
#include "Subtitles.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <variant>

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

Document Fixture() {
    Document document;
    document.sources = {
        {"01KT0000000000000000000001", "source.mov", {25, 1}, {250, 25}},
    };
    DocumentTrack video;
    video.id = "01KT0000000000000000000002";
    video.kind = "video";
    video.index = 0;
    DocumentClip clip;
    clip.id = "01KT0000000000000000000003";
    clip.source_id = document.sources[0].id;
    clip.source_in = {0, 25};
    clip.duration = {250, 25};
    clip.timeline_in = {0, 25};
    video.clips.push_back(clip);
    document.sequence.tracks.push_back(video);
    return document;
}

}  // namespace

int main() {
    const std::string srt =
        "\xEF\xBB\xBF"
        "1\r\n00:00:01,250 --> 00:00:02,500\r\n"
        "Bonjour\r\nle monde\r\n\r\n"
        "2\r\n00:00:03.000 --> 00:00:04,125\r\nDeuxième\r\n";
    std::vector<SubtitleCue> cues;
    std::string error;
    Check(ParseSrt(srt, cues, error), "valid SRT parses: " + error);
    Check(cues.size() == 2 && cues[0].timeline_in == RationalTime{1250, 1000} &&
              cues[0].duration == RationalTime{1250, 1000} &&
              cues[0].text == "Bonjour\nle monde" &&
              cues[1].duration == RationalTime{1125, 1000},
          "SRT timestamps and multiline text stay exact");

    Document document = Fixture();
    const std::string before = document.SaveToString();
    EditLog log;
    EditError editError = EditError::None;
    std::string message;
    AddTrackOperation add =
        BuildSubtitleTrackEdit(cues, 1, "01KT0000000000000000000004");
    Check(log.Apply(document, Operation{add}, editError, message),
          "subtitle track operation applies: " + message);
    const DocumentTrack* track =
        document.FindTrack("01KT0000000000000000000004");
    Check(track && track->kind == "caption" && track->clips.size() == 2 &&
              track->clips[0].source_id.empty(),
          "SRT becomes a dedicated source-free caption track");
    const std::string serialized =
        SerializeOperation(log.AppliedEntries().back().op);
    Operation parsed = RemoveTrackOperation{};
    Check(DeserializeOperation(serialized, parsed, editError, message) &&
              SerializeOperation(parsed) == serialized,
          "caption AddTrack operation serializes canonically");
    Document loaded;
    Check(Document::LoadFromString(document.SaveToString(), loaded, error) &&
              loaded.SaveToString() == document.SaveToString(),
          "caption track document round-trips byte-identically: " + error);
    Check(ActiveSubtitles(document, {1500, 1000}).size() == 1 &&
              ActiveSubtitles(document, {2750, 1000}).empty(),
          "monitor lookup follows exact half-open cue ranges");

    Transcript transcript;
    transcript.media_id = document.sources[0].id;
    transcript.source_rate = {25, 1};
    transcript.words = {
        {"hors", {25, 25}, {40, 25}},     {"Bonjour", {50, 25}, {60, 25}},
        {",", {60, 25}, {61, 25}},        {"le", {62, 25}, {66, 25}},
        {"monde.", {67, 25}, {75, 25}},   {"Deuxième", {100, 25}, {112, 25}},
        {"phrase", {113, 25}, {125, 25}},
    };
    DocumentClip transcriptClip = document.sequence.tracks[0].clips[0];
    transcriptClip.source_in = {50, 25};
    transcriptClip.timeline_in = {250, 25};
    transcriptClip.duration = {75, 25};
    std::vector<SubtitleCue> generated;
    Check(SubtitleCuesForClip(transcript, transcriptClip, generated, error),
          "transcript maps into subtitle cues: " + error);
    Check(generated.size() == 2 && generated[0].text == "Bonjour, le monde." &&
              generated[0].timeline_in == RationalTime{250, 25} &&
              generated[1].text == "Deuxième phrase" &&
              generated[1].timeline_in == RationalTime{300, 25} &&
              generated[1].duration == RationalTime{25, 25},
          "word grouping is deterministic and clip-relative");

    const std::filesystem::path output =
        std::filesystem::temp_directory_path() /
        ("cutmachine-subtitles-" + GenerateUlid() + ".srt");
    Check(SaveSrt(document, output.string(), error),
          "caption track exports to SRT: " + error);
    std::vector<SubtitleCue> exported;
    Check(LoadSrt(output.string(), exported, error) &&
              exported.size() == cues.size() &&
              exported[0].timeline_in == cues[0].timeline_in &&
              exported[0].text == cues[0].text,
          "exported SRT imports with exact timing and text: " + error);
    std::error_code ignored;
    std::filesystem::remove(output, ignored);

    // WriteSrt sert le même sérialiseur à un appelant qui tient déjà ses
    // cues : --export-srt les tire des transcriptions en cache, sans poser
    // de piste de sous-titres dans le document juste pour obtenir un fichier.
    const std::filesystem::path direct =
        std::filesystem::temp_directory_path() /
        ("cutmachine-subtitles-" + GenerateUlid() + ".srt");
    Check(WriteSrt(generated, direct.string(), error),
          "des cues s'écrivent en SRT sans passer par le document : " + error);
    std::vector<SubtitleCue> roundTrip;
    Check(LoadSrt(direct.string(), roundTrip, error) &&
              roundTrip.size() == generated.size() &&
              roundTrip[0].timeline_in == generated[0].timeline_in &&
              roundTrip[0].text == generated[0].text &&
              roundTrip[1].text == generated[1].text,
          "l'aller-retour garde le minutage et le texte : " + error);
    std::filesystem::remove(direct, ignored);

    std::vector<SubtitleCue> nothing;
    Check(!WriteSrt(nothing, direct.string(), error),
          "une liste vide se refuse au lieu d'écrire un fichier vide");
    Check(!std::filesystem::exists(direct, ignored),
          "et rien n'est créé sur le disque");

    Check(log.Undo(document, editError, message) &&
              document.SaveToString() == before,
          "one undo removes the imported track byte-identically");

    std::vector<SubtitleCue> invalid;
    Check(!ParseSrt("1\n00:00:02,000 --> 00:00:01,000\nNon\n", invalid, error),
          "backwards SRT timing is rejected explicitly");

    if (failures) return 1;
    std::cout << "All subtitle tests passed\n";
    return 0;
}
