#include "PerformanceMetrics.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

double Percentile(const std::vector<double>& sorted, double percentile) {
    if (sorted.empty()) {
        return 0.0;
    }
    const size_t rank = std::max<size_t>(
        1, static_cast<size_t>(std::ceil(percentile * sorted.size())));
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

void PerformanceMetrics::RecordDelivery(double milliseconds, bool cacheHit) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& samples = cacheHit ? hitDeliveryMilliseconds_ : missDeliveryMilliseconds_;
    samples.push_back(milliseconds);
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
    std::vector<double> sortedHits;
    std::vector<double> sortedMisses;
    Snapshot snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sortedHits.assign(hitDeliveryMilliseconds_.begin(),
                          hitDeliveryMilliseconds_.end());
        sortedMisses.assign(missDeliveryMilliseconds_.begin(),
                            missDeliveryMilliseconds_.end());
        snapshot.hitDeliverySamples = sortedHits.size();
        snapshot.missDeliverySamples = sortedMisses.size();
        snapshot.hitRate = cacheHits_.empty()
            ? 0.0
            : static_cast<double>(hitCount_) / cacheHits_.size();
    }
    std::sort(sortedHits.begin(), sortedHits.end());
    std::sort(sortedMisses.begin(), sortedMisses.end());
    snapshot.hitP50Ms = Percentile(sortedHits, 0.50);
    snapshot.hitP95Ms = Percentile(sortedHits, 0.95);
    snapshot.hitP99Ms = Percentile(sortedHits, 0.99);
    snapshot.missP50Ms = Percentile(sortedMisses, 0.50);
    snapshot.missP95Ms = Percentile(sortedMisses, 0.95);
    snapshot.missP99Ms = Percentile(sortedMisses, 0.99);
    snapshot.drops = drops_.load(std::memory_order_relaxed);
    snapshot.framesInFlight = framesInFlight_.load(std::memory_order_relaxed);
    return snapshot;
}
