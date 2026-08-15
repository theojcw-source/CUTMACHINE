#pragma once

#include <cstdint>
#include <string>

struct AVFrame;

class MediaSource {
public:
    MediaSource();
    ~MediaSource();

    bool Open(const std::string& path, int threadCount = 0,
              int threadType = -1);
    bool DecodeFrame(int64_t frameIndex, const AVFrame*& outFrame,
                     int64_t& outPts);
    bool DecodeNextFrame(const AVFrame*& outFrame, int64_t& outPts);

    int Width() const;
    int Height() const;
    int64_t FrameCount() const;
    int32_t FrameRateNumerator() const;
    int32_t FrameRateDenominator() const;
    int64_t FrameIndexForPts(int64_t pts) const;

private:
    struct Impl;
    Impl* impl_;
};
