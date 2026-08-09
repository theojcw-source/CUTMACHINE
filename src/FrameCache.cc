#include "FrameCache.h"

extern "C" {
#include <libavutil/frame.h>
}

#include <algorithm>
#include <cstdlib>
#include <cmath>

namespace {

AVFrame* RetainFrame(const AVFrame* source) {
    AVFrame* retained = av_frame_alloc();
    if (!retained || av_frame_ref(retained, source) < 0) {
        av_frame_free(&retained);
        return nullptr;
    }
    return retained;
}

}  // namespace

FrameCache::FrameCache(size_t byteBudget) : byteBudget_(byteBudget) {}

FrameCache::~FrameCache() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& item : entries_) {
        av_frame_free(&item.second.frame);
    }
}

size_t FrameCache::FrameBytes(const AVFrame* frame) {
    size_t bytes = 0;
    for (const AVBufferRef* buffer : frame->buf) {
        if (buffer) {
            bytes += buffer->size;
        }
    }
    for (int index = 0; index < frame->nb_extended_buf; ++index) {
        if (frame->extended_buf[index]) {
            bytes += frame->extended_buf[index]->size;
        }
    }
    return bytes;
}

void FrameCache::Touch(EntryMap::iterator entry) {
    lru_.splice(lru_.begin(), lru_, entry->second.lruPosition);
    entry->second.lruPosition = lru_.begin();
}

void FrameCache::EvictToBudget() {
    while (totalBytes_ > byteBudget_ && !lru_.empty()) {
        const Key key = lru_.back();
        auto entry = entries_.find(key);
        if (entry != entries_.end()) {
            totalBytes_ -= entry->second.bytes;
            av_frame_free(&entry->second.frame);
            entries_.erase(entry);
        }
        lru_.pop_back();
    }
}

bool FrameCache::Put(SourceId sourceId, int64_t frameIndex, const AVFrame* frame) {
    if (!frame) {
        return false;
    }
    AVFrame* retained = RetainFrame(frame);
    if (!retained) {
        return false;
    }
    const size_t bytes = FrameBytes(retained);

    std::lock_guard<std::mutex> lock(mutex_);
    auto& knownFrameBytes = sourceFrameBytes_[sourceId];
    knownFrameBytes = std::max(knownFrameBytes, bytes);
    const Key key = {sourceId, frameIndex};
    auto existing = entries_.find(key);
    if (existing != entries_.end()) {
        Touch(existing);
        av_frame_free(&retained);
        return true;
    }

    lru_.push_front(key);
    entries_.emplace(key, Entry{retained, bytes, lru_.begin()});
    totalBytes_ += bytes;
    EvictToBudget();
    return entries_.find(key) != entries_.end();
}

AVFrame* FrameCache::GetExact(SourceId sourceId, int64_t frameIndex) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto entry = entries_.find(Key{sourceId, frameIndex});
    if (entry == entries_.end()) {
        return nullptr;
    }
    Touch(entry);
    return RetainFrame(entry->second.frame);
}

AVFrame* FrameCache::GetNearest(SourceId sourceId, int64_t frameIndex,
                                int64_t& outFrameIndex) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto next = entries_.lower_bound(Key{sourceId, frameIndex});
    auto best = entries_.end();

    if (next != entries_.end() && next->first.sourceId == sourceId) {
        best = next;
    }
    if (next != entries_.begin()) {
        auto previous = std::prev(next);
        if (previous->first.sourceId == sourceId) {
            if (best == entries_.end() ||
                std::llabs(previous->first.frameIndex - frameIndex) <=
                    std::llabs(best->first.frameIndex - frameIndex)) {
                best = previous;
            }
        }
    }
    if (best == entries_.end()) {
        return nullptr;
    }

    outFrameIndex = best->first.frameIndex;
    Touch(best);
    return RetainFrame(best->second.frame);
}

bool FrameCache::Contains(SourceId sourceId, int64_t frameIndex) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.find(Key{sourceId, frameIndex}) != entries_.end();
}

bool FrameCache::TouchFrame(SourceId sourceId, int64_t frameIndex) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto entry = entries_.find(Key{sourceId, frameIndex});
    if (entry == entries_.end()) {
        return false;
    }
    Touch(entry);
    return true;
}

void FrameCache::RegisterSource(SourceId sourceId) {
    std::lock_guard<std::mutex> lock(mutex_);
    activeSources_.insert(sourceId);
}

void FrameCache::UnregisterSource(SourceId sourceId) {
    std::lock_guard<std::mutex> lock(mutex_);
    activeSources_.erase(sourceId);
}

FrameCache::PrefetchWindow FrameCache::WindowForSource(SourceId sourceId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto bytes = sourceFrameBytes_.find(sourceId);
    if (bytes == sourceFrameBytes_.end() || bytes->second == 0) {
        return {};
    }

    const size_t sourceCount = std::max<size_t>(1, activeSources_.size());
    const double framesFunded = static_cast<double>(byteBudget_) / bytes->second;
    const size_t total = std::max<size_t>(
        1, static_cast<size_t>(std::ceil(framesFunded * 0.70 / sourceCount)));
    if (total == 1) {
        return {};
    }
    const size_t ahead = std::min<size_t>(
        total - 1, static_cast<size_t>(std::lround(total * 0.75)));
    return PrefetchWindow{ahead, total - 1 - ahead, total};
}

size_t FrameCache::ByteBudget() const { return byteBudget_; }

size_t FrameCache::TotalBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return totalBytes_;
}

size_t FrameCache::EntryCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}
