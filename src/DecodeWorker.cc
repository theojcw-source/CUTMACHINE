#include "DecodeWorker.h"

extern "C" {
#include <libavutil/frame.h>
}

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace {

class InFlightFrame {
public:
    explicit InFlightFrame(PerformanceMetrics& metrics) : metrics_(metrics) {
        metrics_.FrameStarted();
    }
    ~InFlightFrame() { metrics_.FrameFinished(); }

private:
    PerformanceMetrics& metrics_;
};

}  // namespace

DecodeWorker::DecodeWorker(const FrameCache::SourceId& sourceId,
                           FrameCache& cache, PerformanceMetrics& metrics)
    : sourceId_(sourceId), cache_(cache), metrics_(metrics) {}

DecodeWorker::~DecodeWorker() {
    Stop();
    if (registeredSource_) {
        cache_.UnregisterSource(sourceId_);
    }
}

bool DecodeWorker::Open(const std::string& path, int threadCount) {
    if (!source_.Open(path, threadCount)) {
        return false;
    }
    path_ = path;
    cache_.RegisterSource(sourceId_);
    registeredSource_ = true;
    return true;
}

void DecodeWorker::Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_) {
        return;
    }
    started_ = true;
    stopping_ = false;
    thread_ = std::thread(&DecodeWorker::Run, this);
}

void DecodeWorker::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_) {
            return;
        }
        stopping_ = true;
    }
    wakeup_.notify_one();
    if (thread_.joinable()) {
        thread_.join();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    started_ = false;
}

void DecodeWorker::RequestFrame(int64_t frameIndex) {
    const auto requestStart = std::chrono::steady_clock::now();
    const int64_t clamped = std::clamp<int64_t>(
        frameIndex, 0, std::max<int64_t>(0, FrameCount() - 1));
    const bool cacheHit = cache_.Contains(sourceId_, clamped);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (requestGeneration_ > 0 && clamped == requestedFrame_) {
            return;
        }
        if (previousRequestedFrame_ >= 0 &&
            clamped != previousRequestedFrame_) {
            const int direction = clamped > previousRequestedFrame_ ? 1 : -1;
            const int64_t elapsedMicroseconds =
                hasPreviousRequestTime_
                    ? std::chrono::duration_cast<std::chrono::microseconds>(
                          requestStart - previousRequestAt_)
                          .count()
                    : 0;
            const int64_t frameDelta =
                std::llabs(clamped - previousRequestedFrame_);
            const bool fasterThanPlayback =
                elapsedMicroseconds > 0 &&
                static_cast<__int128>(frameDelta) * 1000000 *
                        source_.FrameRateDenominator() >
                    static_cast<__int128>(elapsedMicroseconds) *
                        source_.FrameRateNumerator();
            const int requiredSamples = fasterThanPlayback ? 1 : 2;
            if (direction == candidateDirection_) {
                ++candidateDirectionSamples_;
            } else {
                candidateDirection_ = direction;
                candidateDirectionSamples_ = 1;
            }
            if (candidateDirectionSamples_ >= requiredSamples) {
                stableDirection_ = candidateDirection_;
            }
        }
        previousRequestedFrame_ = clamped;
        previousRequestAt_ = requestStart;
        hasPreviousRequestTime_ = true;
        requestedFrame_ = clamped;
        requestWasHit_ = cacheHit;
        requestStartedAt_ = requestStart;
        ++requestGeneration_;
    }
    metrics_.RecordRequest(cacheHit);
    if (cacheHit) {
        const int64_t microseconds =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - requestStart)
                .count();
        metrics_.RecordDelivery(microseconds, true);
    }
    wakeup_.notify_one();
}

void DecodeWorker::Run() {
    uint64_t handledGeneration = 0;
    while (true) {
        int64_t targetFrame = 0;
        int direction = 1;
        bool requestWasHit = false;
        std::chrono::steady_clock::time_point requestStart;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wakeup_.wait(lock, [&] {
                return stopping_ || requestGeneration_ != handledGeneration;
            });
            if (stopping_) {
                return;
            }
            targetFrame = requestedFrame_;
            direction = stableDirection_;
            requestWasHit = requestWasHit_;
            requestStart = requestStartedAt_;
            handledGeneration = requestGeneration_;
        }

        if (!cache_.Contains(sourceId_, targetFrame) &&
            !FillAscending(targetFrame, targetFrame, handledGeneration)) {
            continue;
        }

        if (RequestChanged(handledGeneration)) {
            continue;
        }
        if (!requestWasHit) {
            const int64_t microseconds =
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - requestStart)
                    .count();
            metrics_.RecordDelivery(microseconds, false);
        }

        const FrameCache::PrefetchWindow window =
            cache_.WindowForSource(sourceId_);
        const int64_t aheadCount = static_cast<int64_t>(window.ahead);
        const int64_t behindCount = static_cast<int64_t>(window.behind);
        const int64_t lastFrame = FrameCount() - 1;
        const int64_t behindFirst =
            direction > 0 ? std::max<int64_t>(0, targetFrame - behindCount)
                          : targetFrame + 1;
        const int64_t behindLast =
            direction > 0
                ? targetFrame - 1
                : std::min<int64_t>(lastFrame, targetFrame + behindCount);
        if (!FillAscending(behindFirst, behindLast, handledGeneration)) {
            continue;
        }

        const int64_t aheadFirst =
            direction > 0 ? targetFrame + 1
                          : std::max<int64_t>(0, targetFrame - aheadCount);
        const int64_t aheadLast =
            direction > 0
                ? std::min<int64_t>(lastFrame, targetFrame + aheadCount)
                : targetFrame - 1;
        if (direction > 0) {
            FillAscending(aheadFirst, aheadLast, handledGeneration);
        } else {
            FillReverseAhead(aheadFirst, aheadLast, handledGeneration);
        }
    }
}

bool DecodeWorker::DecodeAt(int64_t frameIndex, uint64_t generation) {
    if (RequestChanged(generation)) {
        return false;
    }
    const AVFrame* decoded = nullptr;
    int64_t pts = 0;
    InFlightFrame inFlight(metrics_);
    if (!source_.DecodeFrame(frameIndex, decoded, pts)) {
        nextSequentialFrame_ = -1;
        return false;
    }
    int64_t actualFrame = source_.FrameIndexForPts(pts);
    if (actualFrame < 0) {
        actualFrame = frameIndex;
    }
    cache_.Put(sourceId_, actualFrame, decoded);
    nextSequentialFrame_ = actualFrame + 1;
    return !RequestChanged(generation);
}

bool DecodeWorker::FillAscending(int64_t firstFrame, int64_t lastFrame,
                                 uint64_t generation) {
    if (firstFrame > lastFrame) {
        return !RequestChanged(generation);
    }

    int64_t cursor = firstFrame;
    while (cursor <= lastFrame) {
        if (RequestChanged(generation)) {
            return false;
        }
        if (cache_.TouchFrame(sourceId_, cursor)) {
            ++cursor;
            continue;
        }

        // After a direction change, the cache can contain a long contiguous
        // run while the codec cursor still points near the old reverse chunk.
        // Re-decoding that whole cached run loses the prefetch race; seek to
        // the first actual hole once the catch-up is no longer cheap.
        constexpr int64_t kMaximumSequentialCatchUp = 4;
        if (nextSequentialFrame_ < 0 || nextSequentialFrame_ > cursor ||
            cursor - nextSequentialFrame_ > kMaximumSequentialCatchUp) {
            if (!DecodeAt(cursor, generation)) {
                return false;
            }
        } else {
            while (nextSequentialFrame_ <= cursor) {
                const AVFrame* decoded = nullptr;
                int64_t pts = 0;
                InFlightFrame inFlight(metrics_);
                if (!source_.DecodeNextFrame(decoded, pts)) {
                    nextSequentialFrame_ = -1;
                    return false;
                }
                int64_t actualFrame = source_.FrameIndexForPts(pts);
                if (actualFrame < 0) {
                    actualFrame = nextSequentialFrame_;
                }
                cache_.Put(sourceId_, actualFrame, decoded);
                nextSequentialFrame_ = actualFrame + 1;
                if (RequestChanged(generation)) {
                    return false;
                }
            }
        }
        ++cursor;
    }
    return true;
}

bool DecodeWorker::FillReverseAhead(int64_t firstFrame, int64_t lastFrame,
                                    uint64_t generation) {
    // Reverse playback necessarily seeks to the start of a forward-decodable
    // chunk. A larger chunk amortizes that cold seek and lets frame threading
    // reach its sequential regime before the next slider request arrives.
    constexpr int64_t kChunkSize = 6;
    while (firstFrame <= lastFrame) {
        if (IsStopping()) {
            return false;
        }

        int64_t chunkEnd = lastFrame;
        while (chunkEnd >= firstFrame &&
               cache_.TouchFrame(sourceId_, chunkEnd)) {
            --chunkEnd;
        }
        bool speculative = false;
        if (chunkEnd < firstFrame) {
            chunkEnd = firstFrame - 1;
            if (chunkEnd < 0) {
                return !RequestChanged(generation);
            }
            // A previous speculative chunk may already cover the next frame
            // outside the logical window. Do not produce another chunk until
            // the scrub position has consumed that reserve.
            if (cache_.TouchFrame(sourceId_, chunkEnd)) {
                return !RequestChanged(generation);
            }
            speculative = true;
        }
        const int64_t chunkStart =
            std::max<int64_t>(0, chunkEnd - kChunkSize + 1);

        const AVFrame* decoded = nullptr;
        int64_t pts = 0;
        InFlightFrame inFlight(metrics_);
        if (!source_.DecodeFrame(chunkStart, decoded, pts)) {
            nextSequentialFrame_ = -1;
            return false;
        }
        int64_t actualFrame = source_.FrameIndexForPts(pts);
        if (actualFrame < 0) {
            actualFrame = chunkStart;
        }
        cache_.Put(sourceId_, actualFrame, decoded);
        nextSequentialFrame_ = actualFrame + 1;

        while (actualFrame < chunkEnd) {
            if (IsStopping() || !source_.DecodeNextFrame(decoded, pts)) {
                nextSequentialFrame_ = -1;
                return false;
            }
            actualFrame = source_.FrameIndexForPts(pts);
            if (actualFrame < 0) {
                actualFrame = nextSequentialFrame_;
            }
            cache_.Put(sourceId_, actualFrame, decoded);
            nextSequentialFrame_ = actualFrame + 1;
        }

        if (RequestChanged(generation)) {
            return false;
        }
        if (speculative) {
            return true;
        }
    }
    return true;
}

bool DecodeWorker::RequestChanged(uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    return stopping_ || requestGeneration_ != generation;
}

bool DecodeWorker::IsStopping() {
    std::lock_guard<std::mutex> lock(mutex_);
    return stopping_;
}

int DecodeWorker::Width() const { return source_.Width(); }
int DecodeWorker::Height() const { return source_.Height(); }
int64_t DecodeWorker::FrameCount() const { return source_.FrameCount(); }
int32_t DecodeWorker::FrameRateNumerator() const {
    return source_.FrameRateNumerator();
}
int32_t DecodeWorker::FrameRateDenominator() const {
    return source_.FrameRateDenominator();
}
