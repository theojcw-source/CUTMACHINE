#pragma once

#include "MediaTaskManager.h"
#include "RationalTime.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Onset (transient) and tempo detection over a source's audio track. Shaped
// like Waveform.h on purpose: a Generate* pass that decodes with FFmpeg and
// writes a small cache file, and a Load* pass that reads the cache back
// without touching FFmpeg again. This is read-only analysis, not a document
// mutation — nothing here is an Operations.h operation, nothing here writes
// to a Document. It exists so an agent tool (and later, UI) can read beat
// positions to line cuts up against, per ROADMAP.md F1.6.
//
// Method: spectral flux onset detection — Dixon, "Onset Detection
// Revisited", DAFx 2006. STFT magnitude spectrum, half-wave-rectified
// frame-to-frame difference summed across bins (the "positive spectral
// difference" that gives the technique its name), then adaptive-threshold
// peak picking on the resulting novelty function. Tempo is estimated by
// autocorrelating that same novelty function and taking the strongest
// periodicity inside a plausible tempo range — a standard tempo-induction
// technique built directly on the onset detection function (see e.g. Ellis,
// "Beat Tracking by Dynamic Programming", 2007). No ML model: ROADMAP.md is
// explicit that DSP is the right choice for this first pass.
//
// Caching: callers key the cache file by the source's LibraryMedia.id, the
// same convention Waveform/Thumbnail/Proxy use elsewhere in the app (see
// main.mm's ".cutmachine/waveforms/<mediaId>.waveform" pattern) — this
// module does not compute that path itself, matching
// GenerateAudioWaveform/GenerateMediaThumbnail.

// One detected onset. `time` is exact: it is an analysis frame's sample
// position — an integer count of samples at the decode sample rate — wrapped
// in a RationalTime at the single conversion point documented in
// BeatDetection.cc. Never a rounded float, per PHILOSOPHY.md principle 4.
struct AudioOnset {
    RationalTime time;
    float strength = 0.0f;  // Novelty peak height, normalized to 0..1.
};

struct BeatDetectionResult {
    std::vector<AudioOnset> onsets;  // Ascending by time.
    std::optional<double> bpm;       // Absent when too few onsets to estimate.
    double bpm_confidence = 0.0;     // 0..1; only meaningful when bpm is set.
};

struct BeatDetectionSettings {
    uint32_t decode_sample_rate = 44100;
    uint32_t fft_size =
        1024;  // STFT window, in samples. Must be a power of two.
    uint32_t hop_size = 512;  // STFT hop, in samples. Must be <= fft_size.
    // A frame is accepted as an onset when it is a local maximum of the
    // novelty function AND exceeds the local mean by this many local
    // standard deviations (adaptive threshold; Dixon 2006 §3.1).
    float onset_sensitivity = 1.5f;
    // Minimum spacing enforced between accepted onsets. Discards secondary
    // peaks produced by the same transient's decay.
    double minimum_onset_interval_seconds = 0.05;
    // Search range for the autocorrelation-based tempo estimate.
    double minimum_bpm = 40.0;
    double maximum_bpm = 240.0;
    std::string ffmpeg_path = "ffmpeg";
};

bool DetectAudioBeats(const std::string& inputPath,
                      const std::string& outputPath,
                      const RationalTime& duration,
                      const BeatDetectionSettings& settings,
                      MediaTaskContext& context, std::string& error);

bool LoadAudioBeats(const std::string& path, BeatDetectionResult& result,
                    std::string& error);
