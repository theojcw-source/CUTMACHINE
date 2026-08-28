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
    const ShotQualityThresholds& thresholds);
