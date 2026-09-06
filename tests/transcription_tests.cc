#include "Document.h"
#include "EditLog.h"
#include "MediaTaskManager.h"
#include "Operations.h"
#include "TimelineTranscription.h"
#include "Transcription.h"

#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
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

template <typename Function>
void Test(const std::string& name, Function function) {
    const int before = failures;
    try {
        function();
        if (failures == before) std::cout << "PASS: " << name << '\n';
    } catch (const std::exception& exception) {
        ++failures;
        std::cerr << "FAIL: " << name << ": " << exception.what() << '\n';
    }
}

std::string Quote(const std::filesystem::path& path) {
    std::string result = "'";
    for (char character : path.string())
        result += character == '\'' ? "'\\''" : std::string(1, character);
    return result + "'";
}

// IA1 -- ROADMAP.md. Test the signal supplied to Whisper independently of
// inference, so an inaudible word cannot survive through a stale mix/cache.
Document TimelineAudioDocument(const std::string& mediaPath) {
    Document document;
    const Ulid sourceId = GenerateUlid();
    document.sequence.frame_rate = {25, 1};
    document.sources = {{sourceId, mediaPath, {25, 1}, {25, 25}}};
    document.sequence.tracks = {
        {GenerateUlid(),
         "audio",
         0,
         {{GenerateUlid(), sourceId, {0, 25}, {25, 25}, {10, 25}, true}}},
    };
    return document;
}

bool DecodeTimelineAudioForTest(const Document& document,
                                const std::filesystem::path& project,
                                const std::string& ffmpegPath,
                                std::vector<float>& samples,
                                std::string& error) {
    TimelineAudioPlan plan;
    if (!BuildTimelineAudioPlan(document, project, plan, error)) return false;
    std::atomic_bool finished{false};
    bool decoded = false;
    bool idle = false;
    {
        MediaTaskManager manager;
        manager.Enqueue(MediaTaskKind::Transcription, "timeline PCM fixture",
                        [&](MediaTaskContext& context, std::string& taskError) {
                            decoded = DecodeFfmpegAudioToPcm16k(
                                ffmpegPath, plan.ffmpeg_arguments, context,
                                samples, taskError);
                            error = taskError;
                            finished.store(true);
                            return decoded;
                        });
        idle = manager.WaitForIdle(30000);
        if (!idle) manager.CancelAll();
        // Join before inspecting captured state, including on timeout.
    }
    if (!idle) error = "timeline PCM decode timed out";
    return idle && finished.load() && decoded;
}

double PcmRms(const std::vector<float>& samples, size_t begin, size_t end) {
    if (begin >= end || end > samples.size())
        throw std::out_of_range("PCM measurement exceeds decoded samples");
    double energy = 0.0;
    for (size_t index = begin; index < end; ++index)
        energy += static_cast<double>(samples[index]) * samples[index];
    return std::sqrt(energy / static_cast<double>(end - begin));
}

// A single video track holding one clip whose source is `sourceId`, spanning
// [0, duration) of a source with the given duration and rate. Mirrors
// edit_tests.cc's EditDocument() fixture convention.
Document WordDocument(const Ulid& sourceId, const RationalTime& sourceInStart,
                      const RationalTime& clipDuration) {
    Document document;
    document.sources = {
        {sourceId, "A.MP4", {25, 1}, {10000, 25}},
    };
    document.sequence.tracks = {
        {"01K30000000000000000000001",
         "video",
         0,
         {
             {"01K30000000000000000000002",
              sourceId,
              sourceInStart,
              clipDuration,
              {0, 25}},
             {"01K30000000000000000000003",
              sourceId,
              {0, 25},
              {50, 25},
              {clipDuration.value, clipDuration.rate}},
         }},
    };
    return document;
}

Transcript FiveWordTranscript(const Ulid& mediaId) {
    Transcript transcript;
    transcript.media_id = mediaId;
    transcript.whisper_model = "ggml-base.en.bin";
    transcript.source_rate = {25, 1};
    // Five words, each 20 frames long at 25fps, back-to-back starting at
    // frame 0: [0,20) [20,40) [40,60) [60,80) [80,100).
    for (int index = 0; index < 5; ++index) {
        transcript.words.push_back({"word" + std::to_string(index),
                                    RationalTime{index * 20, 25},
                                    RationalTime{index * 20 + 20, 25}});
    }
    return transcript;
}

bool Apply(EditLog& log, Document& document, Operation operation,
           const std::string& label) {
    EditError error = EditError::None;
    std::string message;
    const bool result =
        log.Apply(document, std::move(operation), error, message);
    Check(result,
          label + " failed with " + EditErrorName(error) + ": " + message);
    return result;
}

}  // namespace

int main() {
    Test("B7 plans the exact audible timeline and invalidates its cache", [] {
        Document document;
        document.sequence.id = "01K300000000000000000000D1";
        document.sequence.frame_rate = {25, 1};
        document.sources = {
            {"01K300000000000000000000D2", "Media/one.wav", {25, 1}, {100, 25}},
            {"01K300000000000000000000D3", "Media/two.wav", {25, 1}, {100, 25}},
        };
        document.sequence.tracks = {
            {"01K300000000000000000000D4",
             "audio",
             1,
             {{"01K300000000000000000000D5",
               "01K300000000000000000000D2",
               {10, 25},
               {25, 25},
               {0, 25}},
              {"01K300000000000000000000D6",
               "01K300000000000000000000D3",
               {5, 25},
               {10, 25},
               {30, 25}}}},
        };
        TimelineAudioPlan first;
        std::string error;
        const std::filesystem::path project =
            std::filesystem::temp_directory_path() /
            "B7.cutmachine-project/project.cutmachine.json";
        Check(BuildTimelineAudioPlan(document, project, first, error) &&
                  first.audio_clips == 2 &&
                  first.duration == RationalTime{40, 25} &&
                  first.ffmpeg_arguments.back() == "pipe:1" &&
                  first.cache_identity.find(document.sequence.id) !=
                      std::string::npos,
              "the audio plan preserves two source-bounded clips, their gap "
              "and a pipe-only output: " +
                  error);
        const auto graph =
            std::find(first.ffmpeg_arguments.begin(),
                      first.ffmpeg_arguments.end(), "-filter_complex");
        Check(graph != first.ffmpeg_arguments.end() &&
                  graph + 1 != first.ffmpeg_arguments.end() &&
                  (graph + 1)->find("adelay=57600S") != std::string::npos &&
                  (graph + 1)->find("amix=inputs=2") != std::string::npos,
              "the second clip is placed at frame 30 on the 48 kHz mix grid");

        const std::string originalIdentity = first.cache_identity;
        document.sequence.tracks[0].clips[0].duration = {24, 25};
        TimelineAudioPlan trimmed;
        Check(BuildTimelineAudioPlan(document, project, trimmed, error) &&
                  trimmed.cache_identity != originalIdentity,
              "changing an audible source bound invalidates the timeline "
              "transcript cache: " +
                  error);
        document.sequence.tracks[0].clips[0].duration = {25, 25};
        document.sequence.tracks[0].clips[1].timeline_in = {31, 25};
        TimelineAudioPlan moved;
        Check(BuildTimelineAudioPlan(document, project, moved, error) &&
                  moved.cache_identity != originalIdentity,
              "changing audible placement invalidates the timeline transcript "
              "cache: " +
                  error);
    });

    Test(
        "IA1 gain and each fade independently invalidate transcript cache", [] {
            Document document = TimelineAudioDocument("tone.wav");
            const std::filesystem::path project =
                std::filesystem::temp_directory_path() / "IA1-project.json";
            std::string error;
            TimelineAudioPlan original;
            const bool planned =
                BuildTimelineAudioPlan(document, project, original, error);
            Check(planned, "baseline audio plan builds: " + error);
            if (!planned) return;
            const auto checkIdentity = [&](bool changed,
                                           const std::string& label) {
                TimelineAudioPlan candidate;
                const bool built =
                    BuildTimelineAudioPlan(document, project, candidate, error);
                Check(built && (candidate.cache_identity !=
                                original.cache_identity) == changed,
                      label + ": " + error);
            };
            DocumentClip& clip = document.sequence.tracks[0].clips[0];
            clip.audio_gain_db = {-20, 1};
            checkIdentity(true, "gain alone invalidates the transcript");
            clip.audio_gain_db = {0, 1};
            checkIdentity(false, "restoring gain restores cache identity");
            clip.audio_fade_in = {1, 5};
            checkIdentity(true, "fade-in alone invalidates the transcript");
            clip.audio_fade_in = {0, 1};
            checkIdentity(false, "restoring fade-in restores cache identity");
            clip.audio_fade_out = {1, 5};
            checkIdentity(true, "fade-out alone invalidates the transcript");
            clip.audio_fade_out = {0, 1};
            checkIdentity(false, "restoring fade-out restores cache identity");
        });

    Test("RoundToSourceFrame rounds outward and is exact on frame boundaries",
         [] {
             const MediaRate rate25{25, 1};
             // 37 centiseconds at 25fps = 9.25 frames: floor=9, ceil=10.
             const RationalTime midway{37, 100};
             Check(RoundToSourceFrame(midway, rate25, false) ==
                       (RationalTime{9, 25}),
                   "rounds down to the containing frame");
             Check(RoundToSourceFrame(midway, rate25, true) ==
                       (RationalTime{10, 25}),
                   "rounds up to the following frame");
             // Exactly on a frame boundary: both directions return it as-is,
             // no drift introduced by an unnecessary rounding.
             const RationalTime onBoundary{40, 100};  // 10 frames at 25fps
             Check(RoundToSourceFrame(onBoundary, rate25, false) ==
                       (RationalTime{10, 25}),
                   "exact boundary rounds down to itself");
             Check(RoundToSourceFrame(onBoundary, rate25, true) ==
                       (RationalTime{10, 25}),
                   "exact boundary rounds up to itself, not the next frame");
             // A fractional frame rate (NTSC-style 30000/1001) exercises the
             // to_frames(num, den) path, not just the common den=1 case.
             const MediaRate ntsc{30000, 1001};
             const RationalTime ntscTime{123, 100};
             const RationalTime down =
                 RoundToSourceFrame(ntscTime, ntsc, false);
             const RationalTime up = RoundToSourceFrame(ntscTime, ntsc, true);
             Check(down.rate == 30000 && up.rate == 30000,
                   "fractional frame rate output uses the frame numerator as "
                   "its RationalTime rate");
             Check(down <= ntscTime && up >= ntscTime,
                   "outward rounding never moves a boundary inward");
         });

    Test("ResolveWordRemoval turns word indices into exact source ranges", [] {
        const Ulid sourceId = "01K30000000000000000000009";
        const Transcript transcript = FiveWordTranscript(sourceId);
        DocumentClip clip;
        clip.id = "01K3000000000000000000000A";
        clip.source_id = sourceId;
        clip.source_in = {0, 25};
        clip.duration = {100, 25};

        RemoveWordsOperation operation;
        std::string error;
        const bool resolvedInterior = ResolveWordRemoval(
            clip, transcript, {{1, 2}}, {5, 25}, {}, operation, error);
        Check(resolvedInterior,
              "resolves a valid interior word range: " + error);
        Check(operation.clip_id == clip.id, "operation targets the right clip");
        Check(operation.ranges.size() == 1 &&
                  operation.ranges[0].source_start == (RationalTime{20, 25}) &&
                  operation.ranges[0].source_end == (RationalTime{60, 25}),
              "range spans word 1's start through word 2's end");
        Check(operation.gap_padding == (RationalTime{5, 25}),
              "gap padding is carried through unchanged");

        RemoveWordsOperation multi;
        const bool resolvedDisjoint = ResolveWordRemoval(
            clip, transcript, {{0, 0}, {3, 4}}, {0, 1}, {}, multi, error);
        Check(resolvedDisjoint, "resolves two disjoint word ranges: " + error);
        Check(multi.ranges.size() == 2 &&
                  multi.ranges[0].source_end <= multi.ranges[1].source_start,
              "resolved ranges stay in ascending, non-overlapping order");

        RemoveWordsOperation mismatch;
        Transcript wrongMedia = transcript;
        wrongMedia.media_id = "01K3000000000000000000000Z";
        Check(!ResolveWordRemoval(clip, wrongMedia, {{0, 0}}, {0, 1}, {},
                                  mismatch, error),
              "rejects a transcript for a different source");

        RemoveWordsOperation outOfBounds;
        Check(!ResolveWordRemoval(clip, transcript, {{4, 10}}, {0, 1}, {},
                                  outOfBounds, error),
              "rejects an out-of-bounds word index");

        RemoveWordsOperation unsorted;
        Check(!ResolveWordRemoval(clip, transcript, {{2, 3}, {0, 1}}, {0, 1},
                                  {}, unsorted, error),
              "rejects word ranges that are not sorted ascending");

        RemoveWordsOperation overlapping;
        Check(!ResolveWordRemoval(clip, transcript, {{0, 2}, {2, 3}}, {0, 1},
                                  {}, overlapping, error),
              "rejects word ranges that overlap on a shared word index");
    });

    Test(
        "LoadAudioTranscript accepts a well-formed cache and rejects a "
        "malformed one",
        [] {
            const std::filesystem::path root =
                std::filesystem::temp_directory_path() /
                (GenerateUlid() + "-transcript-cache");
            std::filesystem::create_directories(root);

            const std::filesystem::path good = root / "good.transcript";
            std::ofstream(good)
                << "{\"version\":1,\"media_id\":\"01K300000000000000000000AA\","
                   "\"whisper_model\":\"ggml-base.en.bin\","
                   "\"verbatim\":true,\"source_rate\":{"
                   "\"num\":25,\"den\":1},\"words\":["
                   "{\"text\":\"hi\",\"start\":{\"value\":0,\"rate\":25},"
                   "\"end\":{\"value\":10,\"rate\":25}},"
                   "{\"text\":\"there\",\"start\":{\"value\":10,\"rate\":25},"
                   "\"end\":{\"value\":25,\"rate\":25}}]}\n";
            Transcript parsed;
            std::string error;
            const bool loadedGood =
                LoadAudioTranscript(good.string(), parsed, error);
            Check(loadedGood, "loads a well-formed transcript cache: " + error);
            Check(parsed.words.size() == 2 && parsed.words[0].text == "hi" &&
                      parsed.source_rate.num == 25 && parsed.verbatim &&
                      parsed.language == "auto",
                  "parsed transcript matches the cache contents exactly");

            const std::filesystem::path rounded =
                root / "legacy-rounded.transcript";
            std::ofstream(rounded)
                << "{\"version\":1,\"media_id\":\"01K300000000000000000000AA\","
                   "\"whisper_model\":\"ggml-base.en.bin\",\"source_rate\":{"
                   "\"num\":25,\"den\":1},\"words\":["
                   "{\"text\":\"first\",\"start\":{\"value\":0,\"rate\":25},"
                   "\"end\":{\"value\":3,\"rate\":25}},"
                   "{\"text\":\"tiny\",\"start\":{\"value\":2,\"rate\":25},"
                   "\"end\":{\"value\":2,\"rate\":25}},"
                   "{\"text\":\"next\",\"start\":{\"value\":2,\"rate\":25},"
                   "\"end\":{\"value\":5,\"rate\":25}}]}\n";
            Transcript normalized;
            Check(LoadAudioTranscript(rounded.string(), normalized, error) &&
                      normalized.words.size() == 2 && !normalized.verbatim &&
                      normalized.words[0].end == RationalTime{3, 25} &&
                      normalized.words[1].start == RationalTime{3, 25} &&
                      normalized.words[1].text == "tiny next",
                  "normalizes legacy frame collisions without losing text: " +
                      error);

            const std::filesystem::path outOfOrder =
                root / "out-of-order.transcript";
            std::ofstream(outOfOrder)
                << "{\"version\":1,\"media_id\":\"01K300000000000000000000AA\","
                   "\"whisper_model\":\"ggml-base.en.bin\",\"source_rate\":{"
                   "\"num\":25,\"den\":1},\"words\":["
                   "{\"text\":\"hi\",\"start\":{\"value\":10,\"rate\":25},"
                   "\"end\":{\"value\":20,\"rate\":25}},"
                   "{\"text\":\"there\",\"start\":{\"value\":0,\"rate\":25},"
                   "\"end\":{\"value\":5,\"rate\":25}}]}\n";
            Transcript rejected;
            Check(!LoadAudioTranscript(outOfOrder.string(), rejected, error),
                  "rejects a cache whose words are not in chronological order");

            std::filesystem::remove_all(root);
        });

    // QC-2026-09 (A1) -- the alignment pass writes the transcript cache back,
    // so the write path and the marker on it have to survive a round trip. An
    // older cache has no marker and must still load, as unaligned.
    Test("SaveAudioTranscript round-trips the speech alignment marker", [] {
        const std::filesystem::path root =
            std::filesystem::temp_directory_path() /
            (GenerateUlid() + "-transcript-align");
        std::filesystem::create_directories(root);
        const std::filesystem::path path = root / "aligned.transcript";

        Transcript transcript;
        transcript.media_id = "01K300000000000000000000AA";
        transcript.whisper_model = "ggml-large-v3.bin";
        transcript.language = "fr";
        transcript.verbatim = true;
        transcript.speech_aligned = true;
        transcript.source_rate = {25, 1};
        transcript.words = {{"bonjour", {0, 25}, {10, 25}},
                            {"tout", {10, 25}, {14, 25}}};
        std::string error;
        Check(SaveAudioTranscript(path.string(), transcript, error),
              "the transcript cache is written: " + error);
        Transcript reloaded;
        Check(LoadAudioTranscript(path.string(), reloaded, error) &&
                  reloaded.speech_aligned && reloaded.verbatim &&
                  reloaded.language == "fr" && reloaded.words.size() == 2 &&
                  reloaded.words[1].text == "tout",
              "an aligned transcript reads back aligned: " + error);

        WhisperSettings matching;
        matching.whisper_model_path = "/models/ggml-large-v3.bin";
        matching.language = "fr";
        matching.verbatim = true;
        Check(TranscriptCacheMatches(reloaded, transcript.media_id, matching),
              "cache identity accepts the exact model, language and mode");
        matching.language = "en";
        Check(!TranscriptCacheMatches(reloaded, transcript.media_id, matching),
              "cache identity rejects a transcript from another language");

        transcript.speech_aligned = false;
        Check(SaveAudioTranscript(path.string(), transcript, error),
              "rewriting the same path succeeds: " + error);
        std::ifstream stored(path);
        std::ostringstream contents;
        contents << stored.rdbuf();
        Check(contents.str().find("speech_aligned") == std::string::npos,
              "an unaligned transcript keeps the bytes it had before the "
              "marker existed");
        Transcript legacy;
        Check(LoadAudioTranscript(path.string(), legacy, error) &&
                  !legacy.speech_aligned,
              "and reads back as not aligned: " + error);

        std::filesystem::remove_all(root);
    });

    Test("B12 compares transcript words with measured speech groups", [] {
        Transcript transcript;
        transcript.media_id = "01K300000000000000000000AB";
        transcript.whisper_model = "ggml-large-v3.bin";
        transcript.language = "fr";
        transcript.source_rate = {25, 1};
        transcript.words = {
            {"Sous-titrage", {0, 25}, {5, 25}},
            {"Société", {5, 25}, {10, 25}},
            {"Radio-Canada", {10, 25}, {15, 25}},
        };
        SpeechOnsetReport envelope;
        envelope.media_id = transcript.media_id;
        envelope.windows_per_second = 50;
        envelope.decode_sample_rate = 16000;

        TranscriptSpeechAssessment assessment;
        std::string error;
        Check(AssessTranscriptAgainstSpeech(transcript, envelope, assessment,
                                            error) &&
                  assessment.word_count == 3 &&
                  assessment.measured_speech_duration == RationalTime{0, 50} &&
                  assessment.known_hallucination_phrase &&
                  assessment.likely_hallucinated,
              "words over no measured speech are flagged, with the known "
              "caption phrase as supporting evidence: " +
                  error);

        envelope.groups = {
            {{0, 50}, {25, 50}, -18, -10},
            {{50, 50}, {100, 50}, -16, -8},
        };
        Check(AssessTranscriptAgainstSpeech(transcript, envelope, assessment,
                                            error) &&
                  assessment.measured_speech_duration == RationalTime{75, 50} &&
                  assessment.known_hallucination_phrase &&
                  !assessment.likely_hallucinated &&
                  !assessment.likely_incomplete,
              "a known phrase carried by real measured speech is never "
              "blacklisted: " +
                  error);

        transcript.words.assign(6, TranscriptWord{"mot", {0, 25}, {1, 25}});
        envelope.groups = {{{0, 50}, {1600, 50}, -19, -11}};
        Check(
            AssessTranscriptAgainstSpeech(transcript, envelope, assessment,
                                          error) &&
                assessment.measured_speech_duration == RationalTime{1600, 50} &&
                assessment.word_count == 6 && assessment.likely_incomplete &&
                !assessment.likely_hallucinated,
            "six words over 32 seconds of measured speech reproduce the "
            "A9 incomplete-transcript signal: " +
                error);
        transcript.words.assign(85, TranscriptWord{"mot", {0, 25}, {1, 25}});
        Check(AssessTranscriptAgainstSpeech(transcript, envelope, assessment,
                                            error) &&
                  !assessment.likely_incomplete,
              "85 words over the same measured speech clear the sparse-text "
              "guard: " +
                  error);

        transcript.words = {
            {"Sous-titrage", {0, 25}, {5, 25}},
            {"Société", {5, 25}, {10, 25}},
            {"Radio-Canada", {10, 25}, {15, 25}},
        };
        envelope.groups.clear();
        Check(AssessTranscriptAgainstSpeech(transcript, envelope, assessment,
                                            error),
              "the unvoiced assessment succeeds: " + error);
        ApplyTranscriptSpeechAssessment(transcript, assessment);
        const std::filesystem::path root =
            std::filesystem::temp_directory_path() /
            (GenerateUlid() + "-transcript-speech-assessment");
        std::filesystem::create_directories(root);
        const std::filesystem::path path = root / "assessed.transcript";
        Check(SaveAudioTranscript(path.string(), transcript, error),
              "the assessed transcript cache is written: " + error);
        Transcript reloaded;
        Check(LoadAudioTranscript(path.string(), reloaded, error) &&
                  reloaded.speech_assessed &&
                  reloaded.measured_speech_duration == RationalTime{0, 50} &&
                  reloaded.likely_hallucinated &&
                  reloaded.known_hallucination_phrase,
              "the exact speech comparison and guard flags survive the "
              "cache round trip: " +
                  error);
        std::filesystem::remove_all(root);

        envelope.media_id = "01K300000000000000000000AC";
        Check(!AssessTranscriptAgainstSpeech(transcript, envelope, assessment,
                                             error),
              "a speech envelope for another source is refused");
    });

    Test("RemoveWordsOperation cuts a linked A/V pair in one reversible step",
         [] {
             // The shape a real interview has: one source, its picture on a
             // video track and its detached sound on an audio track, linked,
             // covering the same span, each followed by another clip.
             const Ulid sourceId = "01K30000000000000000000020";
             const Ulid linkGroup = "01K30000000000000000000021";
             Document document;
             document.sources = {{sourceId, "A.MP4", {25, 1}, {10000, 25}}};
             const auto pair = [&](const Ulid& trackId, const std::string& kind,
                                   int32_t index, const Ulid& firstId,
                                   const Ulid& secondId) {
                 DocumentTrack track;
                 track.id = trackId;
                 track.kind = kind;
                 track.index = index;
                 DocumentClip first{
                     firstId, sourceId, {0, 25}, {100, 25}, {0, 25}};
                 first.link_group_id = linkGroup;
                 DocumentClip second{
                     secondId, sourceId, {0, 25}, {50, 25}, {100, 25}};
                 track.clips = {first, second};
                 return track;
             };
             document.sequence.tracks = {
                 pair("01K30000000000000000000022", "video", 0,
                      "01K30000000000000000000023",
                      "01K30000000000000000000024"),
                 pair("01K30000000000000000000025", "audio", 1,
                      "01K30000000000000000000026",
                      "01K30000000000000000000027"),
             };
             const std::string before = document.SaveToString();

             EditLog log;
             RemoveWordsOperation remove{
                 "01K30000000000000000000026",  // aim at the sound
                 {{{20, 25}, {60, 25}}},
                 {0, 1},
                 {"01K30000000000000000000023"},  // take the picture with it
                 {},
                 {}};
             Check(Apply(log, document, remove, "clean a linked pair"),
                   "RemoveWords applies across a linked A/V pair");

             const DocumentTrack& video = document.sequence.tracks[0];
             const DocumentTrack& audio = document.sequence.tracks[1];
             Check(video.clips.size() == 3 && audio.clips.size() == 3,
                   "both tracks split into two fragments plus their next clip");
             // The whole point: picture and sound must still line up. Cutting
             // only the clip named would have left the video 40 frames longer
             // and every later frame out of sync with its own sound.
             for (size_t index = 0; index < 3; ++index) {
                 Check(video.clips[index].timeline_in ==
                           audio.clips[index].timeline_in,
                       "fragment " + std::to_string(index) +
                           " starts at the same timeline position on both "
                           "tracks");
                 Check(
                     video.clips[index].duration == audio.clips[index].duration,
                     "fragment " + std::to_string(index) +
                         " has the same duration on both tracks");
             }
             Check(video.clips[2].timeline_in == (RationalTime{60, 25}),
                   "downstream material closes up by the 40 removed frames on "
                   "both tracks at once");

             const std::string serialized =
                 SerializeOperation(log.AppliedEntries().back().op);
             Check(serialized.find("linked_clip_ids") != std::string::npos,
                   "the linked clips are part of the serialized operation");
             Operation parsedOperation = RemoveClipOperation{};
             EditError error = EditError::None;
             std::string message;
             Check(DeserializeOperation(serialized, parsedOperation, error,
                                        message) &&
                       SerializeOperation(parsedOperation) == serialized,
                   "a linked RemoveWords JSON round-trips exactly");

             Check(
                 log.Undo(document, error, message) &&
                     document.SaveToString() == before,
                 "undoing the pair cut restores both tracks byte-identically");
             Check(log.Redo(document, error, message),
                   "and it redoes from the recorded snapshot");
         });

    Test(
        "RemoveWordsOperation refuses a linked clip that does not cover the "
        "cut, instead of cutting part of the pair",
        [] {
            const Ulid sourceId = "01K30000000000000000000030";
            Document document;
            document.sources = {{sourceId, "A.MP4", {25, 1}, {10000, 25}}};
            DocumentTrack video;
            video.id = "01K30000000000000000000031";
            video.kind = "video";
            video.index = 0;
            // Deliberately reads a different part of the source, so the ranges
            // resolved against the audio clip fall outside it.
            video.clips = {{"01K30000000000000000000032",
                            sourceId,
                            {500, 25},
                            {100, 25},
                            {0, 25}}};
            DocumentTrack audio;
            audio.id = "01K30000000000000000000033";
            audio.kind = "audio";
            audio.index = 1;
            audio.clips = {{"01K30000000000000000000034",
                            sourceId,
                            {0, 25},
                            {100, 25},
                            {0, 25}}};
            document.sequence.tracks = {video, audio};
            const std::string before = document.SaveToString();

            EditLog log;
            RemoveWordsOperation remove{"01K30000000000000000000034",
                                        {{{20, 25}, {60, 25}}},
                                        {0, 1},
                                        {"01K30000000000000000000032"},
                                        {},
                                        {}};
            EditError error = EditError::None;
            std::string message;
            Check(!log.Apply(document, remove, error, message),
                  "a linked clip outside the ranges is refused");
            Check(error == EditError::SourceOutOfBounds,
                  "and it is refused by name, not by a generic failure");
            Check(document.SaveToString() == before,
                  "the refusal leaves both tracks untouched: no half-applied "
                  "cut");
        });

    Test(
        "RemoveWordsOperation ripple-closes an interior cut with padding "
        "and is byte-exactly reversible",
        [] {
            const Ulid sourceId = "01K30000000000000000000010";
            Document document = WordDocument(sourceId, {0, 25}, {100, 25});
            const std::string before = document.SaveToString();
            const Ulid clipId = document.sequence.tracks[0].clips[0].id;
            const Ulid nextClipId = document.sequence.tracks[0].clips[1].id;

            EditLog log;
            RemoveWordsOperation remove{
                clipId,
                {{{20, 25}, {60, 25}}},  // remove the middle 40 frames
                {5, 25},                 // keep a 5-frame padding gap
                {},
                {}};
            Check(Apply(log, document, remove, "remove interior words"),
                  "RemoveWords applies");

            const DocumentTrack& track = document.sequence.tracks[0];
            Check(track.clips.size() == 3,
                  "the clip split into two kept fragments plus the "
                  "unaffected next clip");
            const DocumentClip& head = track.clips[0];
            const DocumentClip& tail = track.clips[1];
            Check(head.id == clipId,
                  "the first fragment keeps the original clip's identity");
            Check(head.timeline_in == (RationalTime{0, 25}) &&
                      head.duration == (RationalTime{20, 25}) &&
                      head.source_in == (RationalTime{0, 25}),
                  "head fragment covers the untouched source before the cut");
            Check(tail.timeline_in == (RationalTime{25, 25}),
                  "tail fragment starts after the head fragment plus the "
                  "5-frame padding gap (20 + 5)");
            Check(tail.duration == (RationalTime{40, 25}) &&
                      tail.source_in == (RationalTime{60, 25}),
                  "tail fragment covers the untouched source after the cut");
            Check(track.clips[2].id == nextClipId &&
                      track.clips[2].timeline_in == (RationalTime{65, 25}),
                  "the next clip ripples left by (removed 40) - (padding 5) "
                  "= 35 frames, from 100 to 65");

            const std::string serialized =
                SerializeOperation(log.AppliedEntries().back().op);
            Operation parsedOperation = RemoveClipOperation{};
            EditError error = EditError::None;
            std::string message;
            Check(DeserializeOperation(serialized, parsedOperation, error,
                                       message) &&
                      SerializeOperation(parsedOperation) == serialized,
                  "RemoveWords JSON round-trips exactly");

            Check(log.Undo(document, error, message) &&
                      document.SaveToString() == before,
                  "undo restores the document byte-identically");
            Check(log.Redo(document, error, message),
                  "redo re-applies from the exact recorded snapshot");
            const std::string afterRedo = document.SaveToString();
            Check(log.Undo(document, error, message) &&
                      document.SaveToString() == before,
                  "a second undo is still byte-identical to the original");
            Check(log.Redo(document, error, message) &&
                      document.SaveToString() == afterRedo,
                  "redo is deterministic across repeated undo/redo cycles");
        });

    Test(
        "RemoveWordsOperation removing the clip's head behaves like an "
        "ordinary head ripple trim",
        [] {
            const Ulid sourceId = "01K30000000000000000000011";
            Document document = WordDocument(sourceId, {0, 25}, {100, 25});
            const std::string before = document.SaveToString();
            const Ulid clipId = document.sequence.tracks[0].clips[0].id;

            EditLog log;
            RemoveWordsOperation remove{
                clipId, {{{0, 25}, {20, 25}}}, {0, 1}, {}, {}};
            Check(Apply(log, document, remove, "remove head words"),
                  "RemoveWords applies at the clip's head");
            const DocumentTrack& track = document.sequence.tracks[0];
            Check(track.clips.size() == 2,
                  "a head removal keeps one fragment, not two");
            Check(track.clips[0].id == clipId &&
                      track.clips[0].timeline_in == (RationalTime{0, 25}) &&
                      track.clips[0].source_in == (RationalTime{20, 25}) &&
                      track.clips[0].duration == (RationalTime{80, 25}),
                  "the clip's timeline position is unchanged, only its "
                  "source_in/duration shrink -- exactly a head ripple trim");
            Check(track.clips[1].timeline_in == (RationalTime{80, 25}),
                  "the next clip ripples left by the full 20 removed frames "
                  "(no padding at the clip's own head edge)");

            EditError error = EditError::None;
            std::string message;
            Check(log.Undo(document, error, message) &&
                      document.SaveToString() == before,
                  "undo restores the original document byte-identically");
        });

    Test(
        "RemoveWordsOperation removing the clip's entire content ripples "
        "like RemoveClip",
        [] {
            const Ulid sourceId = "01K30000000000000000000012";
            Document document = WordDocument(sourceId, {0, 25}, {100, 25});
            const Ulid clipId = document.sequence.tracks[0].clips[0].id;

            EditLog log;
            RemoveWordsOperation remove{
                clipId, {{{0, 25}, {100, 25}}}, {5, 25}, {}, {}};
            Check(Apply(log, document, remove, "remove the clip's full span"),
                  "RemoveWords applies across the clip's entire span");
            const DocumentTrack& track = document.sequence.tracks[0];
            Check(track.clips.size() == 1,
                  "the fully-removed clip disappears rather than leaving an "
                  "empty fragment");
            Check(track.clips[0].timeline_in == (RationalTime{0, 25}),
                  "the following clip ripples left to close the gap, with "
                  "no trailing padding once the clip itself is gone");
        });

    Test(
        "RemoveWordsOperation rejects out-of-order or out-of-bounds ranges "
        "without mutating the document",
        [] {
            const Ulid sourceId = "01K30000000000000000000013";
            Document document = WordDocument(sourceId, {0, 25}, {100, 25});
            const Ulid clipId = document.sequence.tracks[0].clips[0].id;
            const std::string before = document.SaveToString();

            EditLog log;
            EditError error = EditError::None;
            std::string message;
            RemoveWordsOperation overlapping{
                clipId,
                {{{0, 25}, {30, 25}}, {{20, 25}, {40, 25}}},
                {0, 1},
                {},
                {}};
            Check(!log.Apply(document, overlapping, error, message),
                  "overlapping ranges are rejected");
            Check(error == EditError::InvalidOperation,
                  "overlap is reported as InvalidOperation");
            Check(document.SaveToString() == before,
                  "a rejected RemoveWords leaves the document untouched");

            RemoveWordsOperation outOfBounds{
                clipId, {{{-10, 25}, {5, 25}}}, {0, 1}, {}, {}};
            Check(!log.Apply(document, outOfBounds, error, message),
                  "a range before the clip's source_in is rejected");
            Check(error == EditError::SourceOutOfBounds,
                  "out-of-bounds is reported as SourceOutOfBounds");
            Check(document.SaveToString() == before,
                  "the document is still untouched after the second "
                  "rejection");
            Check(log.AppliedCount() == 0,
                  "neither rejected operation reached the edit log");
        });

    const char* ffmpegPath = std::getenv("FFMPEG_EXECUTABLE_FOR_TESTS");
#ifdef FFMPEG_EXECUTABLE
    if (!ffmpegPath) ffmpegPath = FFMPEG_EXECUTABLE;
#endif
    if (ffmpegPath) {
        Test(
            "IA1 transcription PCM follows clip gain, fades and mix limiter",
            [ffmpegPath] {
                const std::filesystem::path root =
                    std::filesystem::temp_directory_path() /
                    (GenerateUlid() + "-timeline-pcm");
                std::filesystem::create_directories(root);
                const std::filesystem::path source = root / "tone.wav";
                const std::filesystem::path project = root / "project.json";
                const std::string generate =
                    Quote(ffmpegPath) +
                    " -hide_banner -loglevel error -f lavfi -i "
                    "'sine=frequency=1000:sample_rate=48000:duration=1' "
                    "-c:a pcm_f32le -y " +
                    Quote(source);
                const bool generated = std::system(generate.c_str()) == 0;
                Check(generated, "generates the timeline PCM fixture");
                if (!generated) {
                    std::filesystem::remove_all(root);
                    return;
                }

                Document document = TimelineAudioDocument(source.string());
                std::vector<float> neutral;
                std::vector<float> processed;
                std::vector<float> limited;
                std::string error;
                bool decoded = DecodeTimelineAudioForTest(
                    document, project, ffmpegPath, neutral, error);
                Check(decoded, "neutral timeline PCM decodes: " + error);
                if (!decoded) {
                    std::filesystem::remove_all(root);
                    return;
                }
                DocumentClip& clip = document.sequence.tracks[0].clips[0];
                clip.audio_gain_db = {-20, 1};
                clip.audio_fade_in = {1, 5};
                clip.audio_fade_out = {1, 5};
                decoded = DecodeTimelineAudioForTest(
                    document, project, ffmpegPath, processed, error);
                Check(decoded, "processed timeline PCM decodes: " + error);
                if (!decoded) {
                    std::filesystem::remove_all(root);
                    return;
                }

                constexpr size_t kExpectedSamples = 22400;
                const bool correctLength = neutral.size() == kExpectedSamples &&
                                           processed.size() == kExpectedSamples;
                Check(correctLength,
                      "gain and fades preserve the exact 1.4 s timeline");
                if (correctLength) {
                    Check(PcmRms(processed, 0, 6000) < 1e-7,
                          "audio processing preserves the leading gap");
                    const double plateau = PcmRms(processed, 11200, 16000);
                    const double neutralPlateau = PcmRms(neutral, 11200, 16000);
                    Check(neutralPlateau > 0.01 &&
                              std::abs(plateau / neutralPlateau - 0.1) < 0.002,
                          "a -20 dB edit attenuates audible PCM by ten");
                    const double attack = PcmRms(processed, 7840, 8160);
                    const double release = PcmRms(processed, 20640, 20960);
                    Check(plateau > 0.0 &&
                              std::abs(attack / plateau - 0.5) < 0.03,
                          "fade-in attenuates the beginning of the clip");
                    Check(plateau > 0.0 &&
                              std::abs(release / plateau - 0.5) < 0.03,
                          "fade-out attenuates the end of the clip");
                }

                clip.audio_gain_db = {40, 1};
                clip.audio_fade_in = {0, 1};
                clip.audio_fade_out = {0, 1};
                decoded = DecodeTimelineAudioForTest(
                    document, project, ffmpegPath, limited, error);
                Check(decoded, "high-gain timeline PCM decodes: " + error);
                if (decoded && limited.size() == kExpectedSamples) {
                    double peak = 0.0;
                    for (size_t index = 11200; index < 16000; ++index) {
                        const double amplitude =
                            std::abs(static_cast<double>(limited[index]));
                        peak = std::max(peak, amplitude);
                    }
                    // The export limits each stereo channel before FFmpeg's
                    // final mono downmix sums them with equal-power weights.
                    const double expectedPeak = 0.668344 * std::sqrt(2.0);
                    Check(std::abs(peak - expectedPeak) < 0.01,
                          "Whisper receives the limited stereo delivery mix "
                          "after downmix; measured peak=" +
                              std::to_string(peak));
                } else {
                    Check(false, "limiting preserves the timeline duration");
                }
                std::filesystem::remove_all(root);
            });

        Test(
            "GenerateAudioTranscript decodes real audio locally, then "
            "fails cleanly on a local model path that does not resolve "
            "(no cloud fallback, no network access attempted)",
            [ffmpegPath] {
                const std::filesystem::path root =
                    std::filesystem::temp_directory_path() /
                    (GenerateUlid() + "-transcription-source");
                std::filesystem::create_directories(root);
                const std::filesystem::path source = root / "tone.wav";
                const std::filesystem::path cache =
                    root / "cache" / "tone.transcript";
                const std::string generate =
                    Quote(ffmpegPath) +
                    " -hide_banner -loglevel error -f lavfi -i "
                    "'sine=frequency=440:sample_rate=48000:duration=1' -y " +
                    Quote(source);
                Check(std::system(generate.c_str()) == 0,
                      "generates a synthetic audio fixture with FFmpeg");

                MediaTaskManager manager;
                WhisperSettings settings;
                settings.ffmpeg_path = ffmpegPath;
                settings.whisper_model_path =
                    (root / "no-such-model.bin").string();
                std::string capturedError;
                manager.Enqueue(
                    MediaTaskKind::Waveform, "transcription fixture",
                    [&](MediaTaskContext& context, std::string& error) {
                        const bool result = GenerateAudioTranscript(
                            source.string(), cache.string(), "01Kmedia",
                            {25, 1}, settings, context, error);
                        capturedError = error;
                        return result;
                    });
                Check(manager.WaitForIdle(30000),
                      "transcription task completes without hanging");
                const auto tasks = manager.Snapshot();
                Check(tasks.size() == 1 &&
                          tasks[0].state == MediaTaskState::Failed,
                      "the task fails (no real model is available in this "
                      "sandbox) rather than crashing or hanging");
                Check(capturedError.find("whisper.cpp model") !=
                          std::string::npos,
                      "failure is specifically local model load, proving "
                      "FFmpeg decode ran to completion first: " +
                          capturedError);
                Check(!std::filesystem::exists(cache),
                      "no partial transcript cache is left behind on "
                      "failure");
                std::filesystem::remove_all(root);
            });
    } else {
        std::cout << "SKIP: GenerateAudioTranscript FFmpeg smoke test (no "
                     "FFmpeg executable configured for this build)\n";
    }

    Test("FindDisfluencies isolates fillers and stutters", [] {
        const auto word = [](const std::string& text, int64_t start,
                             int64_t end) {
            return TranscriptWord{text, RationalTime{start, 25},
                                  RationalTime{end, 25}};
        };
        const std::vector<TranscriptWord> words = {
            word("Donc", 0, 1),    word("euuuh", 1, 2),
            word("je", 2, 3),      word("je", 3, 4),
            word("voulais", 4, 5), word("m'inspirer", 5, 6),
            word("de", 6, 7),      word("l'Attaque", 7, 8),
        };
        const std::vector<Disfluency> found = FindDisfluencies(words);
        Check(found.size() == 2,
              "one filler and one stutter are found, and nothing else");
        if (found.size() == 2) {
            Check(found[0].kind == DisfluencyKind::Filler &&
                      found[0].range.start_word_index == 1 &&
                      found[0].range.end_word_index == 1,
                  "'euuuh' is folded onto 'euh' and reported as a filler");
            // The second "je" is the one that runs into "voulais", so the cut
            // has to land on the first.
            Check(found[1].kind == DisfluencyKind::Repetition &&
                      found[1].range.start_word_index == 2 &&
                      found[1].range.end_word_index == 2,
                  "a doubled word drops its first occurrence, not its last");
        }
        // Sorted and non-overlapping is ResolveWordRemoval's precondition,
        // so the detector owes it directly rather than by convention.
        for (size_t index = 1; index < found.size(); ++index)
            Check(found[index].range.start_word_index >
                      found[index - 1].range.end_word_index,
                  "detected ranges are sorted and non-overlapping");
    });

    Test("FindDisfluenciesInClip only names words the clip plays", [] {
        Transcript transcript;
        transcript.media_id = "01K300000000000000000000AA";
        transcript.source_rate = {25, 1};
        transcript.words = {
            {"euh", RationalTime{0, 25}, RationalTime{5, 25}},
            {"bonjour", RationalTime{5, 25}, RationalTime{25, 25}},
            {"euh", RationalTime{50, 25}, RationalTime{55, 25}},
        };
        DocumentClip clip;
        clip.id = "01K300000000000000000000AB";
        clip.source_id = transcript.media_id;
        clip.source_in = RationalTime{0, 25};
        clip.duration = RationalTime{30, 25};

        const std::vector<Disfluency> found =
            FindDisfluenciesInClip(clip, transcript);
        // The second "euh" is real, but it lives past this clip's out point:
        // ApplyOperation would refuse it, so proposing it would only produce
        // an error the caller has to guess its way out of.
        Check(found.size() == 1 && found[0].range.start_word_index == 0,
              "a disfluency outside the clip's source span is not proposed");

        // Indices stay those of the whole transcript -- that is the domain
        // ResolveWordRemoval looks them up in.
        RemoveWordsOperation operation;
        std::string error;
        Check(ResolveWordRemoval(clip, transcript, {found[0].range},
                                 RationalTime{0, 1}, {}, operation, error),
              "the proposed range resolves against the full transcript: " +
                  error);
    });

    Test("FindDisfluencies keeps real words that look like fillers", [] {
        const auto word = [](const std::string& text) {
            return TranscriptWord{text, RationalTime{0, 25},
                                  RationalTime{1, 25}};
        };
        // "eu" is what collapsing "euu" lands on, but it is also the past
        // participle of "avoir"; "bien" starts like "ben". Cutting either
        // would remove meaning, which is the failure this detector must not
        // have.
        const std::vector<TranscriptWord> words = {
            word("j'ai"), word("eu"), word("bien"), word("des"), word("des"),
        };
        const std::vector<Disfluency> found = FindDisfluencies(words);
        Check(found.size() == 1 &&
                  found[0].kind == DisfluencyKind::Repetition &&
                  found[0].range.start_word_index == 3,
              "only the doubled 'des' is reported; 'eu' and 'bien' survive");
    });

    Test("FindDisfluencies ignores punctuation glued to words", [] {
        const auto word = [](const std::string& text) {
            return TranscriptWord{text, RationalTime{0, 25},
                                  RationalTime{1, 25}};
        };
        // Whisper attaches punctuation to the preceding word and sometimes
        // emits two words in one token, so folding has to see through both.
        const std::vector<TranscriptWord> words = {
            word("partiel,"), word("Euh..."), word("c'\u00e9tait"),
            word("? Donc"),   word("donc"),   word("voil\u00e0"),
        };
        const std::vector<Disfluency> found = FindDisfluencies(words);
        Check(found.size() == 2,
              "punctuation does not hide a filler or a "
              "repetition");
        if (found.size() == 2) {
            Check(found[0].kind == DisfluencyKind::Filler &&
                      found[0].range.start_word_index == 1,
                  "'Euh...' is recognised through its trailing punctuation");
            Check(found[1].kind == DisfluencyKind::Repetition &&
                      found[1].range.start_word_index == 3,
                  "'? Donc' and 'donc' are seen as the same repeated word");
        }
    });

    Test("the configured Whisper model resolves from the local settings", [] {
        const std::filesystem::path root =
            std::filesystem::temp_directory_path() /
            ("cutmachine-whisper-" + std::to_string(::getpid()));
        std::filesystem::create_directories(root);
        const std::filesystem::path envFile = root / ".env";
        const std::filesystem::path model = root / "ggml-test.bin";
        { std::ofstream(model) << "not a real model, but a real file"; }
        ::setenv("CUTMACHINE_ENV_FILE", envFile.string().c_str(), 1);
        ::unsetenv("CUTMACHINE_WHISPER_MODEL");

        // Nothing configured at all: the message has to name both the
        // variable and the file, or the user is told "no" with nowhere to go.
        std::string path;
        std::string reason;
        Check(!ResolveConfiguredWhisperModel(path, reason),
              "an unconfigured model is refused");
        Check(reason.find("CUTMACHINE_WHISPER_MODEL") != std::string::npos &&
                  reason.find(envFile.string()) != std::string::npos,
              "the refusal names the setting and the file to edit: " + reason);

        // Configured through the file.
        {
            std::ofstream(envFile)
                << "# commentaire\nCUTMACHINE_WHISPER_MODEL=" << model.string()
                << "\n";
        }
        path.clear();
        Check(ResolveConfiguredWhisperModel(path, reason) &&
                  path == model.string(),
              "a model configured in the local .env resolves: " + reason);

        // The real environment wins over the file, so a one-off override in
        // front of a command works without editing anything.
        const std::filesystem::path other = root / "ggml-other.bin";
        { std::ofstream(other) << "another real file"; }
        ::setenv("CUTMACHINE_WHISPER_MODEL", other.string().c_str(), 1);
        path.clear();
        Check(ResolveConfiguredWhisperModel(path, reason) &&
                  path == other.string(),
              "an exported variable overrides the file");

        // Configured but wrong: refused before any inference is attempted.
        ::setenv("CUTMACHINE_WHISPER_MODEL",
                 (root / "absent.bin").string().c_str(), 1);
        path.clear();
        Check(!ResolveConfiguredWhisperModel(path, reason),
              "a configured path that is not a file is refused");
        Check(reason.find("absent.bin") != std::string::npos,
              "the refusal names the offending path: " + reason);

        ::unsetenv("CUTMACHINE_WHISPER_MODEL");
        ::unsetenv("CUTMACHINE_ENV_FILE");
        std::error_code cleanup;
        std::filesystem::remove_all(root, cleanup);
    });

    if (failures > 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "All transcription tests passed\n";
    return 0;
}
