#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>

class PerformanceMetrics {
public:
    struct Snapshot {
        int64_t hitP50Us = 0;
        int64_t hitP95Us = 0;
        int64_t hitP99Us = 0;
        int64_t missP50Us = 0;
        int64_t missP95Us = 0;
        int64_t missP99Us = 0;
        double hitRate = 0.0;
        uint64_t drops = 0;
        int framesInFlight = 0;
        size_t hitDeliverySamples = 0;
        size_t missDeliverySamples = 0;
    };

    void RecordRequest(bool cacheHit);
    void RecordDelivery(int64_t microseconds, bool cacheHit);
    void RecordDrop();
    void FrameStarted();
    void FrameFinished();
    Snapshot GetSnapshot() const;

private:
    static constexpr size_t kWindowSize = 120;

    mutable std::mutex mutex_;
    std::deque<int64_t> hitDeliveryMicroseconds_;
    std::deque<int64_t> missDeliveryMicroseconds_;
    std::deque<bool> cacheHits_;
    size_t hitCount_ = 0;
    std::atomic<uint64_t> drops_{0};
    std::atomic<int> framesInFlight_{0};
};
