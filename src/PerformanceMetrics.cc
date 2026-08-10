#include "PerformanceMetrics.h"

#include <algorithm>
#include <vector>

namespace {

int64_t Percentile(const std::vector<int64_t>& sorted, size_t percent) {
    if (sorted.empty()) {
        return 0;
    }
    const size_t rank =
        std::max<size_t>(1, (percent * sorted.size() + 99) / 100);
    return sorted[std::min(rank - 1, sorted.size() - 1)];
}

}  // namespace

void PerformanceMetrics::RecordRequest(bool cacheHit) {
    std::lock_guard<std::mutex> lock(mutex_);
    cacheHits_.push_back(cacheHit);
    hitCount_ += cacheHit ? 1 : 0;
    if (cacheHits_.size() > kWindowSize) {
        hitCount_ -= cacheHits_.front() ? 1 : 0;
        cacheHits_.pop_front();
    }
}

void PerformanceMetrics::RecordDelivery(int64_t microseconds, bool cacheHit) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& samples =
        cacheHit ? hitDeliveryMicroseconds_ : missDeliveryMicroseconds_;
    samples.push_back(microseconds);
    if (samples.size() > kWindowSize) {
        samples.pop_front();
    }
}

void PerformanceMetrics::RecordDrop() {
    drops_.fetch_add(1, std::memory_order_relaxed);
}

void PerformanceMetrics::FrameStarted() {
    framesInFlight_.fetch_add(1, std::memory_order_relaxed);
}

void PerformanceMetrics::FrameFinished() {
    framesInFlight_.fetch_sub(1, std::memory_order_relaxed);
}

PerformanceMetrics::Snapshot PerformanceMetrics::GetSnapshot() const {
    std::vector<int64_t> sortedHits;
    std::vector<int64_t> sortedMisses;
    Snapshot snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sortedHits.assign(hitDeliveryMicroseconds_.begin(),
                          hitDeliveryMicroseconds_.end());
        sortedMisses.assign(missDeliveryMicroseconds_.begin(),
                            missDeliveryMicroseconds_.end());
        snapshot.hitDeliverySamples = sortedHits.size();
        snapshot.missDeliverySamples = sortedMisses.size();
        snapshot.hitRate = cacheHits_.empty() ? 0.0
                                              : static_cast<double>(hitCount_) /
                                                    cacheHits_.size();
    }
    std::sort(sortedHits.begin(), sortedHits.end());
    std::sort(sortedMisses.begin(), sortedMisses.end());
    snapshot.hitP50Us = Percentile(sortedHits, 50);
    snapshot.hitP95Us = Percentile(sortedHits, 95);
    snapshot.hitP99Us = Percentile(sortedHits, 99);
    snapshot.missP50Us = Percentile(sortedMisses, 50);
    snapshot.missP95Us = Percentile(sortedMisses, 95);
    snapshot.missP99Us = Percentile(sortedMisses, 99);
    snapshot.drops = drops_.load(std::memory_order_relaxed);
    snapshot.framesInFlight = framesInFlight_.load(std::memory_order_relaxed);
    return snapshot;
}
