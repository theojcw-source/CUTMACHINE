#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>

class PerformanceMetrics {
public:
    struct Snapshot {
        double hitP50Ms = 0.0;
        double hitP95Ms = 0.0;
        double hitP99Ms = 0.0;
        double missP50Ms = 0.0;
        double missP95Ms = 0.0;
        double missP99Ms = 0.0;
        double hitRate = 0.0;
        uint64_t drops = 0;
        int framesInFlight = 0;
        size_t hitDeliverySamples = 0;
        size_t missDeliverySamples = 0;
    };

    void RecordRequest(bool cacheHit);
    void RecordDelivery(double milliseconds, bool cacheHit);
    void RecordDrop();
    void FrameStarted();
    void FrameFinished();
    Snapshot GetSnapshot() const;

private:
    static constexpr size_t kWindowSize = 120;

    mutable std::mutex mutex_;
    std::deque<double> hitDeliveryMilliseconds_;
    std::deque<double> missDeliveryMilliseconds_;
    std::deque<bool> cacheHits_;
    size_t hitCount_ = 0;
    std::atomic<uint64_t> drops_{0};
    std::atomic<int> framesInFlight_{0};
};
