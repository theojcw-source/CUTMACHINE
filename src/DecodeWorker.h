#pragma once

#include "FrameCache.h"
#include "MediaSource.h"
#include "PerformanceMetrics.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

class DecodeWorker {
public:
    DecodeWorker(const FrameCache::SourceId& sourceId, FrameCache& cache,
                 PerformanceMetrics& metrics);
    ~DecodeWorker();

    DecodeWorker(const DecodeWorker&) = delete;
    DecodeWorker& operator=(const DecodeWorker&) = delete;

    bool Open(const std::string& path, int threadCount);
    // The media this worker actually opened, proxy substitution included.
    // Lets a caller tell a worker that is still correct from one that has to
    // be rebuilt, instead of rebuilding every worker on every change.
    const std::string& Path() const { return path_; }
    void Start();
    void Stop();
    void RequestFrame(int64_t frameIndex);

    int Width() const;
    int Height() const;
    int64_t FrameCount() const;
    int32_t FrameRateNumerator() const;
    int32_t FrameRateDenominator() const;

private:
    void Run();
    bool DecodeAt(int64_t frameIndex, uint64_t generation);
    bool FillAscending(int64_t firstFrame, int64_t lastFrame,
                       uint64_t generation);
    bool FillReverseAhead(int64_t firstFrame, int64_t lastFrame,
                          uint64_t generation);
    bool RequestChanged(uint64_t generation);
    bool IsStopping();

    const FrameCache::SourceId sourceId_;
    std::string path_;
    FrameCache& cache_;
    PerformanceMetrics& metrics_;
    MediaSource source_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable wakeup_;
    bool started_ = false;
    bool stopping_ = false;
    uint64_t requestGeneration_ = 0;
    int64_t requestedFrame_ = 0;
    int64_t previousRequestedFrame_ = -1;
    int candidateDirection_ = 0;
    int candidateDirectionSamples_ = 0;
    int stableDirection_ = 1;
    int64_t nextSequentialFrame_ = -1;
    bool registeredSource_ = false;
    bool requestWasHit_ = false;
    std::chrono::steady_clock::time_point requestStartedAt_;
    std::chrono::steady_clock::time_point previousRequestAt_;
    bool hasPreviousRequestTime_ = false;
};
