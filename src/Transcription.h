#pragma once

#include "Document.h"
#include "MediaTaskManager.h"
#include "Operations.h"
#include "RationalTime.h"
#include "Ulid.h"

#include <cstdint>
#include <string>
#include <vector>

// F1.4 -- transcription and word-level cutting. See PHILOSOPHY.md's "pas de
// service central": transcription runs a local whisper.cpp model against
// media already on disk. There is no network call, no account, no cloud API
// in this file.

// One transcribed word. `start`/`end` are exact positions in the source
// media's own time domain -- the same domain as DocumentSource::duration and
// DocumentClip::source_in/duration, at `Transcript::source_rate`. See
// RoundToSourceFrame below for the single explicit rounding rule that gets
// them there from whisper.cpp's native centisecond timestamps.
struct TranscriptWord {
    std::string text;
    RationalTime start;
    RationalTime end;
};

// A transcript is a cache artifact, not document state (PHILOSOPHY.md
// principle 1: the document is only ever a JSON file describing the edit).
// It is keyed on `media_id`, the same LibraryMedia/DocumentSource ULID the
// rest of the per-media cache is keyed on -- see main.mm's
// loadOrEnqueueWaveformForMediaIdentifier and its neighbors for the existing
// `.cutmachine/<kind>/<media_id>.<ext>` convention this mirrors instead of
// inventing a new one.
struct Transcript {
    std::string media_id;
    // Recorded so a transcript produced by a different local model is never
    // silently reused as if it were this one -- e.g. a project migrated to a
    // larger local model should re-transcribe, not read stale words.
    std::string whisper_model;
    // The exact timebase TranscriptWord::start/end are expressed in. This is
    // normally the source media's own MediaRate (DocumentSource::rate), so a
    // word's RationalTime is already frame-exact in the domain
    // DocumentClip::source_in/duration use -- no further rescale needed by
    // callers.
    MediaRate source_rate;
    std::vector<TranscriptWord> words;
};

struct WhisperSettings {
    // Path to a local ggml model file (e.g. "ggml-base.en.bin"). Never
    // downloaded or resolved from a remote service by this code; the caller
    // supplies a path already on disk.
    std::string whisper_model_path;
    std::string ffmpeg_path = "ffmpeg";
    // ISO 639-1 code, or "auto" to let whisper.cpp detect the language.
    std::string language = "auto";
};

// Decodes `inputPath`'s audio locally via FFmpeg (mirrors Waveform.cc's
// decode pipeline), runs local whisper.cpp inference against the PCM
// samples, and writes the resulting transcript as JSON to `outputPath`
// (creating parent directories as needed). No network access, no account:
// `settings.whisper_model_path` must already be a local file.
bool GenerateAudioTranscript(const std::string& inputPath,
                             const std::string& outputPath,
                             const std::string& mediaId,
                             const MediaRate& sourceRate,
                             const WhisperSettings& settings,
                             MediaTaskContext& context, std::string& error);

// Loads a transcript previously written by GenerateAudioTranscript. Rejects
// (rather than silently repairing) a cache file with out-of-order or
// zero/negative-duration words: a hand-edited or corrupted cache must not be
// read as if it were exact.
bool LoadAudioTranscript(const std::string& path, Transcript& transcript,
                         std::string& error);

// One inclusive span of transcript word indices to remove. Deliberately not
// a frame range: this is the granularity an agent or a UI names ("remove
// words 12 through 15"), never a frame number it computed itself.
struct WordRange {
    size_t start_word_index;
    size_t end_word_index;
};

// The single explicit rounding rule this file uses to place a transcribed
// word boundary onto the exact frame grid of `frameRate` (PHILOSOPHY.md
// principle 4: conversions that cannot be exact are refused elsewhere, but a
// word boundary is inherently continuous input -- whisper.cpp's centisecond
// timestamp -- being intentionally snapped to a discrete cut grid, so the
// rule must be an explicit rounding, not a refusal). `roundUp` false rounds
// down to the frame containing or preceding `time`; true rounds up to the
// frame containing or following it. GenerateAudioTranscript applies this
// once, at generation time, rounding every word's start down and its end up
// before the transcript is ever cached -- so a cut removes the *whole* frame
// a word's audio touches, and every TranscriptWord read back by
// LoadAudioTranscript is already frame-exact with no further rounding left
// for a caller to get wrong or skip.
RationalTime RoundToSourceFrame(const RationalTime& time,
                                const MediaRate& frameRate, bool roundUp);

// Resolves word index ranges against `transcript` into the exact
// RemoveWordsOperation ApplyOperation can execute -- a straight lookup, not a
// conversion, since the transcript's words are already frame-exact. This is
// the deterministic computation PHILOSOPHY.md principle 7 asks for: the
// caller names *which words* (semantic, agent- or UI-sized); this function
// resolves *which frames* (exact, code-guaranteed) -- it is the only
// intended producer of WordRemovalRange. `wordRanges` must already be sorted
// by `start_word_index` and non-overlapping; `clip` only supplies clip_id,
// its own source bounds are re-validated by ApplyOperation, not trusted
// here. `transcript.media_id` must match `clip.source_id` -- the same
// mounted-source identity DocumentSource::id shares with LibraryMedia::id --
// or this is refused rather than resolving a mismatched clip's cut against
// another source's words.
bool ResolveWordRemoval(const DocumentClip& clip, const Transcript& transcript,
                        const std::vector<WordRange>& wordRanges,
                        const RationalTime& gapPadding,
                        const std::vector<Ulid>& syncTrackIds,
                        RemoveWordsOperation& operation, std::string& error);
