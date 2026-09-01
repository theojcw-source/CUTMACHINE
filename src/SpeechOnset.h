#pragma once

#include "Document.h"
#include "MediaTaskManager.h"
#include "RationalTime.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// ONSET-2026-08 -- deterministic measurement of where speech actually starts
// inside a source, so an interview cut can enter on the word instead of on
// the silence in front of it.
//
// This exists because the transcript cannot answer the question. Whisper
// places the first words of a segment on silence: on one measured rush it
// reports "Alors," at frame 0 and "moi," at frame 22 while the voice does not
// arrive until frame 34, and the intervening audio sits at the source's own
// noise floor. An edit built on those timestamps -- which is what
// InterviewShort's spans are -- opens every other clip on up to 1.3 s of dead
// air. Editors hear that as hesitation, and no transcript-driven tool can see
// it: FindDisfluencies only finds fillers that were written down, and these
// were not written down at all.
//
// Shaped like ShotQuality.h on purpose: a Generate* pass that decodes with
// FFmpeg and writes a small cache file, a Load* pass that reads it back, and
// a pure core that tests exercise on synthetic envelopes. Read-only analysis
// -- nothing here is an Operations.h operation and nothing here writes to a
// Document. The caller names the clip; this resolves the frames
// (PHILOSOPHY.md principle 7).
//
// No model is involved, for the same reason ShotQuality has none. "Where does
// the voice start" is a measurement. What the voice then says is a judgement,
// and is deliberately not answered here.

// Levels are carried, and cached, as integers scaled by this constant: a
// window's RMS amplitude as a fraction of full scale, times the scale. Two
// runs over the same media therefore produce identical bytes, and no float
// literal ever reaches a cache file. A caller wanting a 0..1 reading divides.
//
// Amplitude rather than decibels, and every threshold below expressed as a
// ratio rather than a difference in dB, because a logarithm cannot be taken
// exactly in integers. The two are the same statement: -18 dB is a factor of
// 0.126, and `speech_ratio_percent` is that factor.
//
// Defined as Document.h's kAudioLevelScale rather than repeated: A3 records
// a mean level on LibraryMedia in exactly this convention, and two copies of
// the number would be two chances for a reading here and a fact there to
// stop meaning the same thing. The names stay separate because the two are
// stored in different places -- one in a cache file, one in the document.
constexpr int64_t kSpeechLevelScale = kAudioLevelScale;

struct SpeechOnsetReport {
    std::string media_id;
    // Windows per second of the analysis grid. Window k covers
    // [k, k+1) / windows_per_second in the source's own time domain, so a
    // window's position is reconstructible from its index alone.
    uint32_t windows_per_second = 0;
    uint32_t decode_sample_rate = 0;
    // Percentiles over every window of the source, stored rather than
    // recomputed so a caller holding one clip's summary never needs the whole
    // envelope back to interpret it. See SpeechOnsetThresholds for what each
    // one is for.
    int64_t speech_level = 0;
    int64_t noise_floor = 0;
    std::vector<int64_t> levels;  // RMS per window, ascending by time.
};

struct SpeechOnsetThresholds {
    // The level a window must reach to count as voice, as a percentage of the
    // *clip's own* speech level -- never an absolute number. Recording level
    // varies by rush, by mic distance and by how loudly someone talks, so an
    // absolute floor either misses a quiet speaker or accepts a loud room.
    //
    // 25% is -12 dB below the clip's own 90th percentile.
    //
    // QC-2026-09 (A1) -- this was 13% (-17.7 dB), chosen in the middle of a
    // plateau measured on rushes where room tone sat 20 to 30 dB under the
    // voice. That plateau does not exist on every shoot. On
    // ADS213_ITW_Findetudefevr26 the room sits about 18 dB under the voice,
    // so the 13% line falls *into* the noise: room windows cross it often
    // enough to hold for the sustain, the onset lands on the room rather
    // than on the word, and the report answers `tight: true` on clips
    // carrying 1.0 to 1.2 s of dead air -- or `suggested_trim: 3` where the
    // attack is 27 frames further on. A threshold that reads room tone as
    // speech is worse than no measurement: it is a wrong answer a caller
    // trusts. -12 dB clears that floor with margin and still sits well under
    // any speech worth entering on.
    int64_t speech_ratio_percent = 25;
    // A window over the threshold is not yet an onset: a lip smack, a chair
    // creak or a single-window transient clears it and is not the voice. The
    // run must hold. 100 ms is shorter than any French syllable and longer
    // than any click.
    int64_t sustain_milliseconds = 100;
    // A clip whose speech level is not at least this many times the
    // *source's* noise floor holds no voice: what the percentile calls
    // "speech" is just the loud end of the room. Such a clip is reported as
    // unmeasured rather than given an onset -- an unmeasured clip is a fact a
    // caller can act on, an invented one is not.
    //
    // Against the source's floor and not the clip's own, because the clip's
    // own is degenerate in exactly the case that matters most: a clip that
    // already enters on the word is speech from end to end, so its internal
    // range is as flat as a clip of pure silence. Only the source, which has
    // seen both the room and the voice, can tell those two apart.
    int64_t minimum_dynamic_range_percent = 400;
    // What to leave in front of the onset when suggesting a trim. Cutting
    // exactly on the first voiced window clips the attack of the consonant
    // and sounds like a splice; two frames of air do not read as hesitation
    // and keep the word whole. Expressed in milliseconds and floored to whole
    // sequence frames by SummarizeClipSpeechOnset, because the trim has to
    // land on a frame the timeline can express.
    int64_t pre_roll_milliseconds = 80;
    // Lead-in at or below this is already tight; suggesting a trim of one or
    // two frames would churn an edit for nothing audible.
    int64_t tolerance_milliseconds = 120;
};

struct ClipSpeechOnset {
    Ulid clip_id;
    Ulid source_id;
    // Windows of the envelope that fall inside the clip's own source range.
    int32_t windows = 0;
    // 90th percentile inside the clip: what the voice measures here.
    int64_t speech_level = 0;
    // 5th percentile over the whole source: what the room measures. See
    // SpeechOnsetThresholds::minimum_dynamic_range_percent for why the
    // reference is the source and not the clip.
    int64_t noise_floor = 0;
    int64_t threshold = 0;
    // Where the voice starts, in the source's own time domain. Only
    // meaningful when `measured`.
    RationalTime onset{0, 1};
    // Silence between the clip's in point and that onset: the dead air an
    // editor hears as hesitation.
    RationalTime lead_in{0, 1};
    // Exactly what to pass to a head ripple trim, in whole sequence frames,
    // pre-roll already deducted and floored at zero. This is the number the
    // caller must not compute: publishing it is what keeps a model from
    // turning a measurement back into arithmetic.
    RationalTime suggested_trim{0, 1};
    // False when the clip holds no window, or when the source has too little
    // dynamic range to measure, or when no run of windows ever clears the
    // threshold. `detail` says which.
    bool measured = false;
    // True when the lead-in is within tolerance, so nothing needs doing.
    bool tight = false;
    std::string detail;
};

// ---------------------------------------------------------------------
// Pure core. No FFmpeg, no filesystem -- what tests exercise directly.
// ---------------------------------------------------------------------

// Root mean square over a stream fed block by block, for a caller that
// cannot hold every sample at once -- an ingest measuring a whole rush, as
// against an analysis pass measuring one 20 ms window. The answer is
// identical to WindowRmsLevel over the concatenation of the blocks: there is
// one implementation, and this is it.
//
// Integer throughout: the squares accumulate exactly and the single division
// and square root happen at the end, so the same samples always give the
// same number on any standard library.
class RunningRmsLevel {
public:
    void Add(const int16_t* samples, size_t count);
    // Zero when nothing has been added, which is also what digital silence
    // measures -- a caller that needs to tell those apart tracks Samples().
    int64_t Level() const;
    uint64_t Samples() const { return count_; }

private:
    unsigned __int128 total_ = 0;
    uint64_t count_ = 0;
};

// Root mean square of a block of interleaved-free 16-bit samples, as a
// fraction of full scale times kSpeechLevelScale.
int64_t WindowRmsLevel(const int16_t* samples, size_t count);

// Nearest-rank percentile on a copy of `values`, `percent` in 0..100.
// Deliberately its own copy rather than ShotQuality's: the two modules answer
// unrelated questions, and a shared helper would tie a change in picture
// grading to a change in audio measurement. Tie-breaking is pinned by tests.
int64_t SpeechLevelPercentile(std::vector<int64_t> values, int percent);

// Index of the first window of the first run that clears `threshold` and
// holds for `sustainWindows`, searching `levels` from `firstWindow`. Returns
// -1 when no such run exists. `sustainWindows` below 1 is treated as 1.
int64_t FirstSustainedWindow(const std::vector<int64_t>& levels,
                             int64_t firstWindow, int64_t threshold,
                             int64_t sustainWindows);

// Summarizes the envelope windows that fall inside `clip`'s own source range
// and resolves the trim that would put the clip's in point on the word.
// `sequenceRate` is what the suggested trim is expressed in, because that is
// the domain a ripple trim moves the timeline in. Fails, rather than
// inventing a figure, when the report belongs to another source.
bool SummarizeClipSpeechOnset(const DocumentClip& clip,
                              const SpeechOnsetReport& report,
                              const SpeechOnsetThresholds& thresholds,
                              const MediaRate& sequenceRate,
                              ClipSpeechOnset& summary, std::string& error);

// ---------------------------------------------------------------------
// Analysis pass and cache.
// ---------------------------------------------------------------------

struct SpeechOnsetSettings {
    // 50 windows per second is 20 ms, the usual short-time analysis frame for
    // speech: long enough that a pitch period fits, short enough to place an
    // onset within one video frame at 25 i/s.
    uint32_t windows_per_second = 50;
    // Speech energy lives below 4 kHz, so 16 kHz mono is enough and keeps the
    // decode cheap. The rate is recorded in the cache because changing it
    // changes the numbers.
    uint32_t decode_sample_rate = 16000;
    std::string ffmpeg_path = "ffmpeg";
};

bool GenerateSpeechOnset(const std::string& inputPath,
                         const std::string& outputPath,
                         const std::string& mediaId,
                         const SpeechOnsetSettings& settings,
                         MediaTaskContext& context, std::string& error);

bool LoadSpeechOnset(const std::string& path, SpeechOnsetReport& report,
                     std::string& error);

// Exposed so tests can round-trip a report without spawning FFmpeg, and so
// the write path has exactly one implementation.
std::string SerializeSpeechOnset(const SpeechOnsetReport& report);
bool DeserializeSpeechOnset(const std::string& json, SpeechOnsetReport& report,
                            std::string& error);

// ---------------------------------------------------------------------
// Agent- and CLI-facing view.
// ---------------------------------------------------------------------

// Reports every clip of every audible audio track, because the question is
// where the *voice* starts and the voice is what an audio track carries. A
// cutaway laid over someone else's words has no onset of its own and is not
// listed. Each entry carries its clip's link group so a caller can issue the
// head ripple trim on the whole A/V pair rather than tearing sync.
//
// Sources with no cached analysis are listed apart, under "unanalyzed": that
// is an unknown, not a pass.
std::string DescribeSpeechOnsetForAgent(
    const Document& document, const std::map<Ulid, SpeechOnsetReport>& reports,
    const SpeechOnsetThresholds& thresholds);
