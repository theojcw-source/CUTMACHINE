#pragma once

#include "Document.h"

#include <cstddef>
#include <cstdint>
#include <string>

// Realtime timeline audio for macOS. Media is decoded once to a common float
// format; the output callback only reads immutable mix plans and never touches
// the editable Document.
class AudioPlayback {
public:
    AudioPlayback();
    ~AudioPlayback();

    bool Open(const Document& document, const std::string& baseDirectory,
              std::string& error);
    // Returns as soon as the mix plan is published for the sources already
    // decoded. Sources this timeline needs and does not have yet are decoded
    // on a background thread and join the mix as they land -- see
    // PERF-2026-09 in AudioPlayback.mm for why that is the tolerated state
    // rather than a new one.
    void RebuildTimeline(const Document& document);
    // Blocks until nothing is left to decode, or the timeout elapses. For
    // tests and for shutdown paths that need a settled mix; the editor never
    // waits on this.
    bool WaitForDecodes(int timeoutMilliseconds);
    bool PlayFrom(RationalTime position, int direction, std::string& error);
    bool ScrubAt(RationalTime position, std::string& error);
    void Stop();
    size_t DecodedSourceCount() const;
    size_t PlannedClipCount() const;
    uint64_t ScrubTriggerCount() const;
    int ShuttleSpeed() const;

private:
    void DecodeLoop();
    void EnsureDecoderStarted();

    struct Impl;
    Impl* impl_;
};
