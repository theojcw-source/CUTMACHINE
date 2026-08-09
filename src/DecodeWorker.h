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
    DecodeWorker(FrameCache::SourceId sourceId, FrameCache& cache,
                 PerformanceMetrics& metrics);
    ~DecodeWorker();

    DecodeWorker(const DecodeWorker&) = delete;
    DecodeWorker& operator=(const DecodeWorker&) = delete;

    bool Open(const std::string& path, int threadCount);
    void Start();
    void Stop();
    void RequestFrame(int64_t frameIndex);

    int Width() const;
    int Height() const;
    int64_t FrameCount() const;
    double FrameRate() const;

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
