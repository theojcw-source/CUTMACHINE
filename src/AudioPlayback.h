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
    void RebuildTimeline(const Document& document);
    bool PlayFrom(RationalTime position, int direction, std::string& error);
    bool ScrubAt(RationalTime position, std::string& error);
    void Stop();
    size_t DecodedSourceCount() const;
    size_t PlannedClipCount() const;
    uint64_t ScrubTriggerCount() const;
    int ShuttleSpeed() const;

private:
    struct Impl;
    Impl* impl_;
};
