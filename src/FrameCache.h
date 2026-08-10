#pragma once

#include "Ulid.h"

#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <mutex>
#include <set>

struct AVFrame;

class FrameCache {
public:
    using SourceId = Ulid;

    struct PrefetchWindow {
        size_t ahead = 0;
        size_t behind = 0;
        size_t totalIncludingCurrent = 1;
    };

    explicit FrameCache(size_t byteBudget);
    ~FrameCache();

    FrameCache(const FrameCache&) = delete;
    FrameCache& operator=(const FrameCache&) = delete;

    bool Put(const SourceId& sourceId, int64_t frameIndex,
             const AVFrame* frame);
    AVFrame* GetExact(const SourceId& sourceId, int64_t frameIndex);
    AVFrame* GetNearest(const SourceId& sourceId, int64_t frameIndex,
                        int64_t& outFrameIndex);
    bool Contains(const SourceId& sourceId, int64_t frameIndex) const;
    bool TouchFrame(const SourceId& sourceId, int64_t frameIndex);
    void RegisterSource(const SourceId& sourceId);
    void UnregisterSource(const SourceId& sourceId);
    PrefetchWindow WindowForSource(const SourceId& sourceId) const;

    size_t ByteBudget() const;
    size_t TotalBytes() const;
    size_t EntryCount() const;

private:
    struct Key {
        SourceId sourceId;
        int64_t frameIndex;

        bool operator<(const Key& other) const {
            return sourceId < other.sourceId || (sourceId == other.sourceId &&
                                                 frameIndex < other.frameIndex);
        }
    };

    struct Entry {
        AVFrame* frame = nullptr;
        size_t bytes = 0;
        std::list<Key>::iterator lruPosition;
    };

    using EntryMap = std::map<Key, Entry>;

    static size_t FrameBytes(const AVFrame* frame);
    void Touch(EntryMap::iterator entry);
    void EvictToBudget();

    const size_t byteBudget_;
    size_t totalBytes_ = 0;
    mutable std::mutex mutex_;
    EntryMap entries_;
    std::list<Key> lru_;
    std::set<SourceId> activeSources_;
    std::map<SourceId, size_t> sourceFrameBytes_;
};
