#include "MediaTaskManager.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>

struct MediaTaskManager::Task {
    MediaTaskSnapshot snapshot;
    Work work;
    Completion completion;
    std::shared_ptr<std::atomic_bool> cancelled =
        std::make_shared<std::atomic_bool>(false);
};

const char* MediaTaskKindName(MediaTaskKind kind) {
    switch (kind) {
        case MediaTaskKind::Probe:
            return "Probe";
        case MediaTaskKind::Thumbnail:
            return "Thumbnail";
        case MediaTaskKind::Waveform:
            return "Waveform";
        case MediaTaskKind::Proxy:
            return "Proxy";
        case MediaTaskKind::Relink:
            return "Relink";
    }
    return "Probe";
}

const char* MediaTaskStateName(MediaTaskState state) {
    switch (state) {
        case MediaTaskState::Queued:
            return "Queued";
        case MediaTaskState::Running:
            return "Running";
        case MediaTaskState::Succeeded:
            return "Succeeded";
        case MediaTaskState::Failed:
            return "Failed";
        case MediaTaskState::Cancelled:
            return "Cancelled";
    }
    return "Failed";
}

MediaTaskContext::MediaTaskContext(
    std::shared_ptr<std::atomic_bool> cancelled,
    std::function<void(double, std::string)> progress)
    : cancelled_(std::move(cancelled)), progress_(std::move(progress)) {}

bool MediaTaskContext::Cancelled() const { return cancelled_->load(); }

void MediaTaskContext::SetProgress(double progress, std::string detail) {
    progress_(std::clamp(progress, 0.0, 1.0), std::move(detail));
}

MediaTaskManager::MediaTaskManager(size_t concurrency) {
    if (concurrency == 0) throw std::invalid_argument("concurrency is zero");
    workers_.reserve(concurrency);
    for (size_t index = 0; index < concurrency; ++index)
        workers_.emplace_back([this] { WorkerLoop(); });
}

MediaTaskManager::~MediaTaskManager() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        for (auto& task : tasks_) task.second->cancelled->store(true);
    }
    condition_.notify_all();
    for (std::thread& worker : workers_)
        if (worker.joinable()) worker.join();
}

Ulid MediaTaskManager::Enqueue(MediaTaskKind kind, std::string label, Work work,
                               Completion completion) {
    if (!work) throw std::invalid_argument("media task has no work");
    auto task = std::make_shared<Task>();
    task->snapshot.id = GenerateUlid();
    task->snapshot.kind = kind;
    task->snapshot.label = std::move(label);
    task->work = std::move(work);
    task->completion = std::move(completion);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) throw std::runtime_error("media task manager stopped");
        tasks_.emplace(task->snapshot.id, task);
        queue_.push_back(task->snapshot.id);
    }
    condition_.notify_one();
    return task->snapshot.id;
}

bool MediaTaskManager::Cancel(const Ulid& id) {
    Completion completion;
    MediaTaskSnapshot completed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = tasks_.find(id);
        if (found == tasks_.end()) return false;
        const std::shared_ptr<Task>& task = found->second;
        if (task->snapshot.state == MediaTaskState::Succeeded ||
            task->snapshot.state == MediaTaskState::Failed ||
            task->snapshot.state == MediaTaskState::Cancelled)
            return false;
        task->cancelled->store(true);
        if (task->snapshot.state == MediaTaskState::Queued) {
            task->snapshot.state = MediaTaskState::Cancelled;
            task->snapshot.detail = "Cancelled before start";
            queue_.erase(std::remove(queue_.begin(), queue_.end(), id),
                         queue_.end());
            completion = task->completion;
            completed = task->snapshot;
            idleCondition_.notify_all();
        }
    }
    condition_.notify_all();
    if (completion) completion(completed);
    return true;
}

void MediaTaskManager::CancelAll() {
    std::vector<Ulid> ids;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& task : tasks_) ids.push_back(task.first);
    }
    for (const Ulid& id : ids) Cancel(id);
}

std::vector<MediaTaskSnapshot> MediaTaskManager::Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<MediaTaskSnapshot> result;
    result.reserve(tasks_.size());
    for (const auto& task : tasks_) result.push_back(task.second->snapshot);
    return result;
}

bool MediaTaskManager::IdleLocked() const {
    return std::none_of(tasks_.begin(), tasks_.end(), [](const auto& item) {
        return item.second->snapshot.state == MediaTaskState::Queued ||
               item.second->snapshot.state == MediaTaskState::Running;
    });
}

bool MediaTaskManager::WaitForIdle(int timeoutMilliseconds) {
    std::unique_lock<std::mutex> lock(mutex_);
    return idleCondition_.wait_for(
        lock, std::chrono::milliseconds(std::max(0, timeoutMilliseconds)),
        [&] { return IdleLocked(); });
}

void MediaTaskManager::WorkerLoop() {
    while (true) {
        std::shared_ptr<Task> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
            if (stopping_ && queue_.empty()) return;
            const Ulid id = queue_.front();
            queue_.erase(queue_.begin());
            task = tasks_.at(id);
            if (task->cancelled->load()) {
                task->snapshot.state = MediaTaskState::Cancelled;
                idleCondition_.notify_all();
                continue;
            }
            task->snapshot.state = MediaTaskState::Running;
            task->snapshot.detail = "Starting";
        }

        MediaTaskContext context(
            task->cancelled, [this, task](double progress, std::string detail) {
                std::lock_guard<std::mutex> lock(mutex_);
                task->snapshot.progress = progress;
                task->snapshot.detail = std::move(detail);
            });
        std::string error;
        bool succeeded = false;
        try {
            succeeded = task->work(context, error);
        } catch (const std::exception& exception) {
            error = exception.what();
        } catch (...) {
            error = "unknown media task failure";
        }

        MediaTaskSnapshot completed;
        Completion completion;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (task->cancelled->load()) {
                task->snapshot.state = MediaTaskState::Cancelled;
                task->snapshot.detail = "Cancelled";
            } else if (succeeded) {
                task->snapshot.state = MediaTaskState::Succeeded;
                task->snapshot.progress = 1.0;
                task->snapshot.detail = "Complete";
            } else {
                task->snapshot.state = MediaTaskState::Failed;
                task->snapshot.error = error.empty() ? "task failed" : error;
                task->snapshot.detail = "Failed";
            }
            completed = task->snapshot;
            completion = task->completion;
            idleCondition_.notify_all();
        }
        if (completion) completion(completed);
    }
}
