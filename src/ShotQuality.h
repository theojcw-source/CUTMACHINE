#pragma once

#include "Document.h"
#include "MediaTaskManager.h"
#include "RationalTime.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// QC-2026-08 -- deterministic picture quality analysis of a video source, so
// an editorial pass can refuse a soft or unsteady shot before it reaches a
// cut.
//
// Shaped like Waveform.h and BeatDetection.h on purpose: a Generate* pass
// that decodes with FFmpeg and writes a small cache file, and a Load* pass
// that reads the cache back without touching FFmpeg again. This is read-only
// analysis -- nothing here is an Operations.h operation and nothing here
// writes to a Document.
//
// No model is involved, deliberately. "Is this shot soft" and "is the camera
// moving" are measurements, not judgements: PHILOSOPHY.md principle 7 puts
// them in code, so the model may ask which clips are usable but never
// decides it by looking at a picture. The same reasoning that keeps
// FindDisfluencies deterministic keeps this file free of inference.
//
// Caching: callers key the cache file by the source's LibraryMedia.id, the
// same ".cutmachine/<kind>/<media_id>.<ext>" convention Waveform, Thumbnail,
// Proxy and Transcription already follow -- this module does not compute
// that path itself, matching GenerateAudioWaveform and DetectAudioBeats.

// Metrics are carried, and cached, as integers scaled by this constant. A
// cache file therefore contains no float literal at all: two runs over the
// same media produce identical bytes, and Json.h's AsInt64 is enough to read
// one back. A caller that wants a 0..1 reading divides by this scale.
constexpr int64_t kShotQualityMetricScale = 1000000;

// Q6 -- ROADMAP.md. Luma bins per sample histogram. 32 bins of 8 levels
// each, chosen against the two failure modes at the ends of the range:
// finer bins put sensor noise across bin edges and make a locked-off tripod
// look like it is cutting, coarser ones stop separating two different
// scenes that happen to share an average brightness. 256 divides by it
// exactly, so a bin index is a shift and no pixel lands out of range.
constexpr int32_t kShotQualityHistogramBins = 32;

struct ShotQualitySample {
    // Exact. Sample k sits at {k, samples_per_second} in the source's own
    // time domain -- an integer count at an integer rate, never a float
    // (PHILOSOPHY.md principle 4). This is the analysis grid's own timebase,
    // not the media's frame rate; the two are compared with
    // RationalTime::compare, which needs no conversion and so cannot round.
    RationalTime time;
    // Variance of the 3x3 Laplacian over the analysis luma plane, taken as a
    // fraction of full scale (divided by 255^2) and multiplied by
    // kShotQualityMetricScale. Higher is sharper. The classic
    // variance-of-Laplacian focus measure; see Pech-Pacheco et al.,
    // "Diatom autofocusing in brightfield microscopy: a comparative study",
    // ICPR 2000, which is where the metric's reputation comes from.
    int64_t sharpness = 0;
    // Mean absolute luma difference against the previous sample, as a
    // fraction of full scale, multiplied by kShotQualityMetricScale. Higher
    // means more changed between the two samples: camera movement, subject
    // movement, or a cut inside the source. Zero on the first sample, which
    // has no predecessor to differ from.
    int64_t motion = 0;
    // Q6 -- ROADMAP.md. Total-variation distance between this sample's luma
    // histogram and the previous sample's, as a fraction of full scale,
    // multiplied by kShotQualityMetricScale. Zero on the first sample, for
    // the same reason `motion` is.
    //
    // This exists because `motion` alone cannot tell a cut from a whip pan:
    // both move nearly every pixel. A pan carries the same scene across the
    // frame, so its luma *distribution* barely moves; a cut replaces the
    // scene, so the distribution moves with it. DetectSourceShots needs both
    // readings and neither one is sufficient alone.
    int64_t histogram_distance = 0;
};

struct ShotQualityReport {
    std::string media_id;
    uint32_t samples_per_second = 0;
    int32_t analysis_width = 0;
    int32_t analysis_height = 0;
    // Median sharpness and median motion across every sample of the source.
    // Both grades are relative to these (see ShotQualityThresholds), so they
    // are stored rather than recomputed: a caller holding one clip's summary
    // must not need the whole source's samples to interpret it.
    int64_t median_sharpness = 0;
    int64_t median_motion = 0;
    // Stored for the same reason the other two are: DetectSourceShots grades
    // a candidate boundary against the source's own baseline, and a caller
    // holding one shot list must not need the samples back to see what that
    // baseline was.
    int64_t median_histogram_distance = 0;
    std::vector<ShotQualitySample> samples;  // Ascending by time.
};

enum class SharpnessGrade { Sharp, Soft, Blurry };
enum class SteadinessGrade { Steady, Moving, Shaky };

const char* SharpnessGradeName(SharpnessGrade grade);
const char* SteadinessGradeName(SteadinessGrade grade);

struct ShotQualityThresholds {
    // Sharpness is graded *relative to the source's own median*, never
    // against an absolute number. Variance of the Laplacian has no stable
    // meaning across content: a shallow-depth-of-field portrait and a flat
    // white wall both score low and only one of them is a mistake. Within
    // one source -- one camera, one lens, one scene -- comparing a clip
    // against the rest of the take is meaningful.
    //
    // The cost of that choice is stated rather than hidden: a source that is
    // soft from end to end grades every clip Sharp, because nothing in it is
    // softer than anything else. That is why ClipShotQuality reports the
    // absolute medians too -- a caller can see the case for itself instead
    // of being told everything is fine.
    int64_t blurry_ratio_percent = 45;
    int64_t soft_ratio_percent = 72;
    // Motion is graded relative to the source too, and for a sharper reason
    // than sharpness: FrameMotionMetric measures how much the *picture*
    // changed between two samples, which conflates camera movement, subject
    // movement and plain contrast. It is not a camera-shake meter and must
    // not be read as one.
    //
    // Absolute thresholds were tried first and measured wrong. Anchored on a
    // static shot of one subject against a plain wall (body of the take
    // 5 000 to 36 000), they flagged 67% of a handheld interview as "moving"
    // and 34% of it as "shaky" -- a talking head who gesticulates sits three
    // times higher as a matter of normal operation, with nothing wrong with
    // the shot. A threshold that condemns two thirds of a source is not a
    // threshold.
    //
    // Against the source's own median, the same two takes behave: on the
    // handheld interview (median 51 616) only its genuine spikes clear 180%,
    // and on the static take (median 19 251) the three samples where the
    // camera leaves the subject -- 67 624, 70 433, 55 683 -- still clear
    // 300%.
    int64_t moving_ratio_percent = 180;
    int64_t shaky_ratio_percent = 300;
    // Floor, not a threshold: on a locked-off tripod the median approaches
    // zero and any ratio would fire on sensor noise. A segment must clear
    // this in absolute terms as well before it is called anything but
    // Steady. It only ever suppresses false positives, never creates one.
    int64_t motion_floor = 20000;
};

struct ClipShotQuality {
    Ulid clip_id;
    Ulid source_id;
    // Analysis samples that fall inside the clip's own source range. A clip
    // shorter than one sample interval has none, which is reported rather
    // than graded: see SummarizeClipShotQuality.
    int32_t samples = 0;
    int64_t sharpness_median = 0;
    // 10th percentile: the clip's worst moments rather than its typical one.
    // A shot that racks focus halfway through has a healthy median and a bad
    // p10, and it is the p10 that decides whether it can be cut into.
    int64_t sharpness_worst = 0;
    int64_t motion_median = 0;
    // 90th percentile, for the same reason in the other direction: a single
    // whip pan is what makes a shot unusable, not the average.
    int64_t motion_peak = 0;
    int64_t source_median_sharpness = 0;
    int64_t source_median_motion = 0;
    SharpnessGrade sharpness = SharpnessGrade::Sharp;
    SteadinessGrade steadiness = SteadinessGrade::Steady;
    // True when both grades are the good one. A caller picking illustration
    // shots can filter on this; a caller that wants to know *why* reads the
    // two grades.
    bool clean = true;
};

// ---------------------------------------------------------------------
// Pure core. No FFmpeg, no filesystem, no Document mutation -- this is what
// tests/shot_quality_tests.cc exercises directly on synthetic images.
// ---------------------------------------------------------------------

// `luma` is a tightly packed width*height plane of 8-bit samples: exactly
// what the analysis pass asks FFmpeg for, so there is no stride to get
// wrong. Both return 0 for a plane too small to have a 3x3 interior.
int64_t FrameSharpnessMetric(const uint8_t* luma, int32_t width,
                             int32_t height);
int64_t FrameMotionMetric(const uint8_t* previous, const uint8_t* current,
                          int32_t width, int32_t height);

// Q6 -- ROADMAP.md. Total-variation (L1) distance between the two planes'
// luma histograms: half the sum of absolute bin-count differences, over the
// pixel count. Reads as "the fraction of pixels that changed luma bin", and
// returns kShotQualityMetricScale for two planes with no bin in common.
//
// Total variation rather than the chi-squared distance the shot-detection
// literature usually reaches for, because chi-squared sums a per-bin
// quotient and so cannot be accumulated exactly in integers. Every figure in
// this file is an integer divided once at the end, which is what makes a
// cache file reproducible (PHILOSOPHY.md principle 6); a metric that forced
// a float here would cost more than the small separation it buys. Both
// distances answer the same question -- how far apart are these two
// distributions -- and the threshold is calibrated against whichever is
// used, not carried over from the other.
int64_t FrameHistogramDistanceMetric(const uint8_t* previous,
                                     const uint8_t* current, int32_t width,
                                     int32_t height);

// Nearest-rank percentile on a copy of `values`, with `percent` in 0..100.
// Exposed because both the source-wide median and every per-clip figure go
// through it, and because its tie-breaking is pinned by the tests rather
// than left to whichever standard library is in use.
int64_t ShotQualityPercentile(std::vector<int64_t> values, int percent);

// Summarizes the analysis samples that fall inside `clip`'s own source range.
// Fails, rather than inventing a grade, when the report belongs to another
// source or when the clip is too short to contain a single sample -- an
// ungraded clip is a fact a caller can act on, a default-graded one is not.
bool SummarizeClipShotQuality(const DocumentClip& clip,
                              const ShotQualityReport& report,
                              const ShotQualityThresholds& thresholds,
                              ClipShotQuality& summary, std::string& error);

// ---------------------------------------------------------------------
// Shot segmentation (Q6 -- ROADMAP.md).
//
// Splits one analysed *source* into the shots it already contains, which is
// a different question from everything above: SummarizeClipShotQuality
// grades a clip an editor has already cut, this finds the cuts nobody has
// made yet. A rush that came off the camera as one file but holds six takes
// is six shots, and until they are named there is nothing to select, grade
// or describe but the whole file.
//
// Still a measurement, so still no model: a cut is a discontinuity in the
// picture, and PHILOSOPHY.md principle 7 keeps discontinuities in code. What
// each shot *contains* is a judgement and deliberately not answered here.
// ---------------------------------------------------------------------

struct ShotSegmentationSettings {
    // A boundary must clear three conditions. What each one is for, and why
    // the obvious two-condition rule does not work, is worth stating because
    // it was measured rather than reasoned:
    //
    // The design this started from was "motion is high AND the histogram
    // moved", on the theory that a pan carries the same scene across the
    // frame while a cut replaces it. Measured against a real handheld rush
    // (C8022, 6.7 s at 4 samples/s) that theory is simply false. Where the
    // camera leaves its subject, motion reaches 70 433 and histogram
    // distance 292 944; a real cut spliced into the same footage measures
    // 48 489 and 30 900. The camera move scores *higher than the cut on
    // both metrics*. No pair of absolute thresholds separates them, and a
    // synthetic pan across colour bars confirms it -- sustained motion of
    // 36 000 to 105 000 and histogram distance of 137 000 to 196 000, all
    // of it ordinary movement inside one shot.
    //
    // What does separate them is not the size of the change but its shape.
    // A camera has inertia, so a move is spread over several samples; a cut
    // is instantaneous, so it lands on exactly one. Measured as a ratio
    // against the median of the samples around it, the same events split
    // cleanly: camera moves reach 153%, 194%, 201%, 202%, 210%, 232% and
    // 299%, while the two real cuts measure 408% and 3 956%. 350% sits
    // between them with the same margin on each side, and 162 of the
    // parameter combinations swept around that point score identically on
    // all four fixtures -- the rule sits on a plateau, not on an edge.
    //
    // So: `cut_motion_local_percent` is the condition that does the
    // separating, and the two absolute conditions below it only keep the
    // ratio from firing on nothing. Removing either absolute one lets
    // sensor noise on a locked-off take clear a ratio taken against a
    // near-zero neighbourhood.
    int64_t cut_motion_floor = 40000;
    int64_t cut_histogram_floor = 20000;
    // The histogram condition keeps a ratio against the source median as
    // well, so a source whose every sample is busy does not have the floor
    // do all the work. Motion has no such ratio: the local window replaced
    // it, and keeping both was what made the rule miss the 48 489 cut in a
    // source whose motion median was 26 263.
    //
    // Stated because it was checked: this is the one number here that no
    // fixture in tests/shot_quality_tests.cc currently depends on. Changing
    // it to 50 leaves every test passing, because on the measured rushes
    // the floor above is the larger of the two, and on the pan fixture the
    // local ratio rejects the source anyway. It is kept as a guard for
    // uniformly busy footage -- heavy grain, strobing -- that the corpus
    // does not yet contain. Whoever adds such a source should either
    // confirm this earns its place or delete it.
    int64_t cut_histogram_ratio_percent = 250;
    int64_t cut_motion_local_percent = 350;
    // Half-width of the neighbourhood the local ratio is taken against, in
    // samples. Four is one second either side at the default analysis rate:
    // long enough to contain a whole camera move, short enough that a
    // second cut nearby does not raise the baseline out of range.
    int64_t cut_local_window_samples = 4;
    // Shortest shot the segmentation will emit, in analysis samples. A
    // candidate boundary closer than this to the previous one is dropped
    // rather than allowed to start a shot: at four samples a second the
    // default is one second, and the alternative -- letting a two-frame
    // flash split a take in three -- produces shots no one can cut with.
    //
    // The cost is stated rather than hidden: a genuine cut inside that
    // window is missed, so a rapid-fire montage re-cut from its own export
    // will under-segment. Lower this for that case; it is a caller's
    // decision, not something the detector should guess from the content.
    int64_t minimum_samples = 4;
    // Known limits, from the same measurements. A dissolve is not found at
    // all: it has no single sample to spike on, which is what the local
    // ratio looks for. A cut between two shots that are genuinely similar
    // -- same framing, same lighting, a few frames apart -- can fall under
    // the absolute floors. And the calibration above rests on four
    // fixtures, one of them real footage; a wider corpus is the thing that
    // would let these numbers stop being provisional.
};

// One detected shot inside a source. Times are on the analysis grid, in the
// source's own time domain -- the same domain DocumentClip::source_in lives
// in, so a caller can cut with these directly and RationalTime::compare will
// never have to convert.
struct SourceShot {
    // Half-open: [start, end). `end` is the start of the next shot, and for
    // the last shot it is one sample interval past the final sample -- the
    // grid knows nothing finer, so this is the source's end rounded up to
    // it, and never the exact media duration. A caller that needs the true
    // tail reads the source's duration instead of this field.
    RationalTime start;
    RationalTime end;
    int32_t first_sample = 0;
    int32_t sample_count = 0;
    // Absolute index into ShotQualityReport::samples of the sharpest sample
    // in the shot: the one frame to pull when a still has to stand for the
    // whole shot. Ties take the earliest, so the choice is reproducible.
    int32_t keyframe_sample = 0;
    // Time of that sample, carried so a caller extracting the frame does not
    // have to hold the report to convert an index back into a timestamp.
    RationalTime keyframe;
    int64_t median_sharpness = 0;
    int64_t median_motion = 0;
};

// Returns the shots `report` contains, in ascending order, always covering
// every sample with no gap and no overlap. A report with samples always
// yields at least one shot -- a source with no cut in it is one shot, which
// is the answer, not a failure. An empty or rateless report yields none.
std::vector<SourceShot> DetectSourceShots(
    const ShotQualityReport& report, const ShotSegmentationSettings& settings);

// ---------------------------------------------------------------------
// FFmpeg analysis pass and its cache.
// ---------------------------------------------------------------------

struct ShotQualitySettings {
    // Four samples a second is enough to catch a soft take or a whip pan
    // while keeping a one-hour rush's cache in the low hundreds of
    // kilobytes. It is not enough to catch a single dropped frame, and is
    // not meant to be.
    uint32_t samples_per_second = 4;
    // Fixed analysis size, aspect deliberately not preserved. Letterboxing
    // to preserve it would add flat bars that lower the Laplacian variance
    // by a constant amount per source, which is exactly the quantity being
    // compared. Anisotropic scaling rescales edges without creating or
    // destroying them, so it leaves the comparison intact.
    int32_t analysis_width = 384;
    int32_t analysis_height = 216;
    std::string ffmpeg_path = "ffmpeg";
};

// Decodes `inputPath`'s first video stream through FFmpeg at the analysis
// grid's rate, measures every sampled frame, and writes the report as JSON
// to `outputPath` (creating parent directories as needed). Mirrors
// GenerateAudioWaveform's process handling, including cancellation.
bool GenerateShotQuality(const std::string& inputPath,
                         const std::string& outputPath,
                         const std::string& mediaId,
                         const RationalTime& duration,
                         const ShotQualitySettings& settings,
                         MediaTaskContext& context, std::string& error);

bool LoadShotQuality(const std::string& path, ShotQualityReport& report,
                     std::string& error);

// Serialization is exposed so tests can round-trip a report without
// spawning FFmpeg, and so the write path has exactly one implementation.
std::string SerializeShotQuality(const ShotQualityReport& report);
bool DeserializeShotQuality(const std::string& json, ShotQualityReport& report,
                            std::string& error);

// ---------------------------------------------------------------------
// Agent- and CLI-facing view.
// ---------------------------------------------------------------------

// Builds the read-only quality view of every video clip on `document`'s
// active timeline, from reports the caller has already loaded and keyed by
// source id. Takes loaded reports rather than a directory so the MCP tool
// (which reads them back through McpBackend, wherever that backend keeps
// them) and the CLI (which reads them from the project package) produce one
// shape from one implementation instead of two that drift.
//
// Clips whose source has no cached report are listed separately under
// "unanalyzed", never omitted: a caller filtering on `clean` must be able to
// tell "measured and fine" from "never measured", and silently dropping the
// second would read as the first. That distinction is the same one
// list_disfluencies makes by reporting whether the transcript was verbatim.
//
// Alongside the per-clip view, and kept separate because it answers a
// different question, every analysed source reports the shots
// DetectSourceShots found in it (Q6 -- ROADMAP.md). The clip list says
// whether what is already cut holds up; the source list says what else is
// in the rushes to cut with. An agent asked to find a better take needs the
// second and cannot derive it from the first.
//
// Occlusion is reported alongside the grade, and kept separate from it. A
// soft shot that a cutaway covers end to end is never seen, so it is not a
// problem to fix -- but "clean" must keep meaning "measured well", not
// "hidden". So each entry carries the quality grade, the visible duration
// after clips on higher-index video tracks are subtracted, and a
// `needs_attention` flag that is the conjunction of the two. Without this
// the view reports covered clips as defects and sends a caller re-cutting
// something no one can see.
std::string DescribeShotQualityForAgent(
    const Document& document, const std::map<Ulid, ShotQualityReport>& reports,
    const ShotQualityThresholds& thresholds,
    const ShotSegmentationSettings& segmentation);
