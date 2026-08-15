#pragma once

#include "Ulid.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

enum class MediaTaskKind { Probe, Thumbnail, Waveform, Proxy, Relink, Beat };
enum class MediaTaskState { Queued, Running, Succeeded, Failed, Cancelled };

struct MediaTaskSnapshot {
    Ulid id;
    MediaTaskKind kind = MediaTaskKind::Probe;
    MediaTaskState state = MediaTaskState::Queued;
    std::string label;
    std::string detail;
    double progress = 0.0;
    std::string error;
};

const char* MediaTaskKindName(MediaTaskKind kind);
const char* MediaTaskStateName(MediaTaskState state);

class MediaTaskContext {
public:
    bool Cancelled() const;
    void SetProgress(double progress, std::string detail = {});

private:
    friend class MediaTaskManager;
    MediaTaskContext(std::shared_ptr<std::atomic_bool> cancelled,
                     std::function<void(double, std::string)> progress);
    std::shared_ptr<std::atomic_bool> cancelled_;
    std::function<void(double, std::string)> progress_;
};

class MediaTaskManager {
public:
    using Work = std::function<bool(MediaTaskContext&, std::string&)>;
    using Completion = std::function<void(const MediaTaskSnapshot&)>;

    explicit MediaTaskManager(size_t concurrency = 1);
    ~MediaTaskManager();
    MediaTaskManager(const MediaTaskManager&) = delete;
    MediaTaskManager& operator=(const MediaTaskManager&) = delete;

    Ulid Enqueue(MediaTaskKind kind, std::string label, Work work,
                 Completion completion = {});
    bool Cancel(const Ulid& id);
    void CancelAll();
    std::vector<MediaTaskSnapshot> Snapshot() const;
    bool WaitForIdle(int timeoutMilliseconds);

private:
    struct Task;
    void WorkerLoop();
    bool IdleLocked() const;

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::condition_variable idleCondition_;
    std::map<Ulid, std::shared_ptr<Task>> tasks_;
    std::vector<Ulid> queue_;
    std::vector<std::thread> workers_;
    bool stopping_ = false;
};
