#pragma once

#include "SpeechOnset.h"
#include "Transcription.h"

#include <cstdint>
#include <string>
#include <vector>

// ALIGN-2026-08 -- pulls a transcript's word boundaries onto the speech they
// describe, before anything is cut from them.
//
// Measured, not assumed. Benchmarked against DaVinci Resolve's own engine on
// nine rushes: Whisper's word starts are excellent typically (40 ms median
// error against the signal) and occasionally catastrophic (1.4 s). Resolve's
// word-level captions behave the same way with a different profile (140 ms
// median, 1.0 s worst). Neither is a timing instrument -- both optimise what
// was said, not when -- and a boundary that lands a second early opens a cut
// on room tone, which an editor hears as hesitation.
//
// So this does not re-align anything. It corrects the boundaries that are
// demonstrably wrong -- a word that claims to start where the signal is
// silent -- and leaves every other one alone. On the benchmark corpus that
// is roughly twenty words in a thousand.
//
// The rules refuse rather than repair, in the manner of the rest of this
// project: an ambiguous boundary, one with no speech edge nearby, or one
// asking for a move beyond the cap is reported and left where it is. A
// silently repaired transcript would be worse than the original, because a
// caller would trust it.
//
// What it cannot do, stated so nobody expects it: a word misplaced inside
// continuous speech has both boundaries in speech, and nothing in the
// envelope says it is wrong. Only silence-adjacent errors are correctable.
// Those are the damaging ones, which is why this is worth doing, but this is
// not forced alignment.

struct TranscriptAlignmentSettings {
    // Speech/silence threshold, as a percentage of the source's own speech
    // level. Deliberately lower than SpeechOnsetThresholds' 13%: that one
    // answers "is this a clear attack", which is the right question for a
    // clip's head and the wrong one here. Quiet speech mid-sentence sits well
    // under it, and classifying it as silence would move boundaries that were
    // already right.
    int64_t speech_ratio_percent = 4;
    // Below this, a boundary is not moved. A correction of one or two
    // analysis windows is quantisation, not error, and moving for it would
    // drift the transcript away from its text for no audible gain.
    int64_t minimum_move_milliseconds = 100;
    // Beyond this, the boundary is refused rather than dragged. A word that
    // needs half a second of correction is not a rounding problem; it is a
    // transcript that put a word somewhere the audio does not support, and
    // guessing where it belongs is not this function's job.
    int64_t maximum_move_milliseconds = 400;
    // Two candidate edges whose distances differ by less than this are
    // treated as indistinguishable, and the boundary is refused.
    int64_t ambiguity_milliseconds = 50;
};

struct AlignedWord {
    size_t index = 0;
    std::string text;
    RationalTime before{0, 1};
    RationalTime after{0, 1};
    int64_t moved_milliseconds = 0;
};

struct TranscriptAlignmentReport {
    std::vector<AlignedWord> moved;
    // Boundaries left alone, by reason. Published separately because they
    // mean different things: `kept_in_speech` is the healthy majority, while
    // a large `refused_no_edge` says the transcript claims words where this
    // envelope hears nothing -- which is a finding about one of the two, and
    // a caller should look rather than trust either.
    int32_t kept_in_speech = 0;
    int32_t refused_ambiguous = 0;
    int32_t refused_no_edge = 0;
    int32_t refused_too_small = 0;
    int32_t refused_too_far = 0;
    // A snap that would put a word at or before its predecessor, or at or
    // after its successor. Measured on the benchmark corpus: 3 of 15
    // otherwise-valid corrections did exactly that, because the nearest
    // speech edge belonged to the region the previous word already occupies.
    // Words are ordered; a transcript that stops being ordered is corrupt,
    // and a caller reading it would have no way to notice.
    int32_t refused_out_of_order = 0;
};

// Start of the first speech window at or after `fromWindow`, and the start of
// each speech region, exposed because the tests pin the segmentation itself
// rather than only its consequences.
std::vector<int64_t> SpeechRegionStarts(const SpeechOnsetReport& envelope,
                                        int64_t threshold);

// Fails only when the two artifacts do not describe the same source, or when
// the envelope has no analysis grid. A transcript with no correctable word is
// a success with an empty `moved`.
bool AlignTranscriptToSpeech(const Transcript& transcript,
                             const SpeechOnsetReport& envelope,
                             const TranscriptAlignmentSettings& settings,
                             Transcript& aligned,
                             TranscriptAlignmentReport& report,
                             std::string& error);

std::string SerializeTranscriptAlignmentReport(
    const TranscriptAlignmentReport& report);
