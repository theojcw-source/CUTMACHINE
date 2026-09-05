#pragma once

#include "Document.h"
#include "Operations.h"
#include "RationalTime.h"
#include "SpeechOnset.h"

#include <cstdint>
#include <string>
#include <vector>

// QC-2026-09 A2 -- closes the silences inside a clip without reading a word.
//
// The measured case: two equivalent gestures removed 6.6 s of internal
// silence from a single montage, and neither needed the transcript. That
// matters beyond the time saved, because the transcript is the expensive
// artifact and the unreliable one -- an interview cut that only has to be
// tightened should not have to wait for Whisper, nor inherit its word
// boundaries. The speech envelope (SpeechOnset.h) already says where the
// voice is; a hollow in it is a pause whatever was said around it.
//
// Same division of labour as Transcription.h's ResolveWordRemoval, for the
// same reason (PHILOSOPHY.md principle 7): the caller states an intent --
// "no pause longer than half a second, leave six frames of it" -- and this
// resolves the exact source ranges. It produces a RemoveWordsOperation
// rather than an operation of its own, because that operation already does
// precisely what is needed and does it reversibly: cut exact spans out of a
// clip's source range, ripple-close each cut, carry the A/V pair through
// Q4b's linked_clip_ids, and shift sync-locked tracks by the same delta.
// Adding a second operation that ripple-closes source spans would be a
// second implementation of one thing.
//
// Deliberately internal pauses only. A hollow that touches the clip's head
// or its tail is not a pause between two phrases; it is lead-in or run-out,
// which SpeechOnset.h measures and a head ripple trim removes. Cutting it
// here would silently do a different edit from the one asked for, and would
// fight the tool that owns that question. Head and tail air are reported, so
// a caller sees what was left rather than assuming there was none.

struct PauseTighteningSettings {
    // Below this, a hollow is a breath or the space between two words in one
    // phrase. Removing those is what makes an interview sound clipped rather
    // than tight, so the default sits above ordinary sentence-internal
    // pauses and below the hesitation an editor would cut by hand.
    int64_t minimum_gap_milliseconds = 400;
    // What is left of each pause, in whole frames of the clip's own source
    // rate. Frames rather than milliseconds because this is the unit the
    // decision is actually made in, and because the cut has to land on a
    // frame either way. Zero butts the two phrases together, which almost
    // always reads as a mistake.
    int64_t keep_frames = 6;
    // Speech/silence line, as a percentage of the clip's own 90th
    // percentile. Same measurement and same reasoning as
    // SpeechOnsetThresholds::speech_ratio_percent, and deliberately the same
    // number: a hollow this calls silence must be a hollow that one calls
    // silence too, or two tools would disagree about the same audio.
    int64_t speech_ratio_percent = 25;
};

// One pause that was closed, in the clip's own source time domain. Published
// so a caller can see what the cut actually did without diffing the
// document -- the operation itself carries only the ranges to remove.
struct TightenedPause {
    RationalTime gap_start;
    RationalTime gap_end;
    RationalTime removed;
};

struct PauseTighteningReport {
    std::vector<TightenedPause> pauses;
    RationalTime removed_total{0, 1};
    // Hollows found and left alone, by reason. Separated because they say
    // different things: `skipped_short` is the healthy majority, while a
    // large `skipped_already_tight` means the clip has been through this
    // once already and a caller lowering `keep_frames` would only churn it.
    int32_t skipped_short = 0;
    int32_t skipped_already_tight = 0;
    // Air at the clip's own edges, measured and not touched. See the header
    // comment: this is SpeechOnset.h's question, and answering it here would
    // do a different edit from the one asked for.
    RationalTime head_air{0, 1};
    RationalTime tail_air{0, 1};
};

// ---------------------------------------------------------------------
// Pure core. No FFmpeg, no filesystem -- what tests exercise directly.
// ---------------------------------------------------------------------

// Resolves the pauses inside `clip`'s own source range against `envelope`
// into the exact RemoveWordsOperation ApplyOperation can execute.
// `sourceRate` is the frame grid every produced boundary lands on, and the
// grid `keep_frames` is counted in; it is the source's own MediaRate.
//
// Rounding is one-directional on purpose: a gap's start rounds forward and
// its end rounds back, so a boundary the envelope places between two frames
// always resolves *inside* the silence. The alternative -- rounding to the
// nearest -- would occasionally take a frame of the consonant that opens the
// next word, which is the one error this must not make.
//
// Fails, rather than inventing a cut, when the envelope belongs to another
// source, has no analysis grid, or when the settings are not usable. A clip
// with no pause worth closing is a success with an empty `operation.ranges`
// -- a caller must check that before applying, because ApplyOperation
// rejects an empty range list.
bool ResolvePauseTightening(const DocumentClip& clip,
                            const SpeechOnsetReport& envelope,
                            const PauseTighteningSettings& settings,
                            const MediaRate& sourceRate,
                            const std::vector<Ulid>& syncTrackIds,
                            RemoveWordsOperation& operation,
                            PauseTighteningReport& report, std::string& error);

// The report as the agent- and CLI-facing JSON both surfaces return.
std::string SerializePauseTighteningReport(const PauseTighteningReport& report);
