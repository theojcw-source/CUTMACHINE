#pragma once

#include "Document.h"

#include <cstdint>
#include <string>

// Reads container/stream headers without decoding frames. The caller owns
// identity/path fields; technical metadata is filled on success.
bool ProbeMediaMetadata(const std::string& path, LibraryMedia& media,
                        std::string& reason);

// QC-2026-09 A3 -- mean level of a media's first audio stream, in Document.h's
// kAudioLevelScale. Separate from ProbeMediaMetadata, which is a header scan
// and stays one: this decodes the audio, so it is the one part of an ingest
// that costs real time. It buys back far more than it costs -- 29 of the 71
// rushes of one measured project were mute cutaways at -74 dBFS, and each
// went to Whisper at eleven times its own runtime before anyone noticed.
//
// Decoded through the FFmpeg binary, mono at a low rate, exactly as
// Waveform.cc and SpeechOnset.cc already do: one decode pipeline for audio
// analysis in this project, not three. RMS is accumulated in integers so two
// runs over the same file give the same number.
//
// Downmixed and band-limited, so this is close to but not identical to
// FFmpeg's own volumedetect on the original streams -- measured 0.7 dB under
// it on a stereo rush, which is the decorrelation between the two channels
// disappearing into the mono sum. That does not matter for what the figure
// is for: it separates -74 dBFS from -30 dBFS, and both readings agree about
// which side of that line a rush is on.
//
// Returns false with a reason when the media has no audio stream, or when
// FFmpeg cannot be run -- neither of which is a reason to fail an ingest, so
// the caller records the entry as unmeasured and moves on.
bool MeasureMediaAudioLevel(const std::string& path,
                            const std::string& ffmpegPath, int64_t& level,
                            std::string& reason);

// Scans either one regular file or every file in a directory, then measures
// the mean audio level of every media that has an audio stream (see
// MeasureMediaAudioLevel). No renderer, AppKit, or Metal object is created by
// this command; the only decoding is that of the audio, through FFmpeg.
int IngestCommand(const std::string& documentPath,
                  const std::string& mediaPath, bool recursive,
                  std::string& output);
