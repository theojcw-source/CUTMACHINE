#pragma once

#include "RationalTime.h"

#include <cstdint>
#include <string>

// QC-2026-08 -- One frame of a source, encoded so a model can actually look
// at it.
//
// This exists because measurement is not enough, and the gap was found the
// hard way. ShotQuality.h grades sharpness and steadiness; on a real edit it
// passed a shot as Sharp/Steady that was unusable as a cutaway, because it
// showed a person visibly speaking words other than the ones on the
// soundtrack. No threshold on Laplacian variance can catch that. A human
// monteur catches it in one glance, and so can a model -- but only if
// something hands it the picture.
//
// So the rule this file lives under is narrower than "give the agent
// vision": what is *provable* stays in ShotQuality.cc and is decided by
// code, and what needs a glance gets a frame. Nothing here decides anything;
// it renders.
//
// Deliberately not a Document-mutating path and not an operation: capturing
// a frame changes nothing, and the result is never cached -- a frame is
// cheap to re-render and stale ones would outlive a relink.

struct FrameCaptureSettings {
    // Long edge of the returned image. Small on purpose: this is for
    // recognition ("is someone talking", "is the subject in frame"), not
    // inspection, and every pixel costs tokens on the way to a model.
    int32_t max_dimension = 512;
    // JPEG, not PNG: a photographic frame compresses an order of magnitude
    // smaller, and the artifacts are far below what any of these judgements
    // turn on.
    int32_t jpeg_quality = 6;  // FFmpeg -q:v scale, 2 (best) to 31 (worst).
    std::string ffmpeg_path = "ffmpeg";
};

// Renders the frame containing `time` in `inputPath`'s own time domain, as
// JPEG bytes. Rotation metadata is honoured, so a vertical rush comes back
// vertical rather than on its side.
bool CaptureSourceFrame(const std::string& inputPath, const RationalTime& time,
                        const FrameCaptureSettings& settings,
                        std::string& jpegBytes, std::string& error);

// Standard base64, no line breaks, padded. Written here rather than pulled
// in because this is the only place in the project that needs it.
std::string EncodeBase64(const std::string& bytes);
