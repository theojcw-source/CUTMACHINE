#include "Document.h"
#include "EditLog.h"
#include "MediaTaskManager.h"
#include "Operations.h"
#include "Transcription.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
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
                   "\"whisper_model\":\"ggml-base.en.bin\",\"source_rate\":{"
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
                      parsed.source_rate.num == 25,
                  "parsed transcript matches the cache contents exactly");

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

    if (failures > 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "All transcription tests passed\n";
    return 0;
}
