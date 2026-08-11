#include "MediaTaskManager.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

int main() {
    int failures = 0;
    const auto check = [&](bool value, const char* message) {
        if (!value) {
            ++failures;
            std::cerr << "FAIL: " << message << '\n';
        }
    };

    MediaTaskManager manager(1);
    std::atomic_int completions{0};
    const Ulid first = manager.Enqueue(
        MediaTaskKind::Probe, "probe.mov",
        [](MediaTaskContext& context, std::string&) {
            context.SetProgress(0.5, "Headers");
            return true;
        },
        [&](const MediaTaskSnapshot& task) {
            if (task.state == MediaTaskState::Succeeded) ++completions;
        });
    check(manager.WaitForIdle(1000), "successful task reaches idle");
    auto snapshot = manager.Snapshot();
    check(snapshot.size() == 1 && snapshot[0].id == first &&
              snapshot[0].state == MediaTaskState::Succeeded &&
              snapshot[0].progress == 1.0 && completions.load() == 1,
          "success and completion are observable");

    std::atomic_bool entered{false};
    const Ulid running = manager.Enqueue(
        MediaTaskKind::Proxy, "proxy.mov",
        [&](MediaTaskContext& context, std::string&) {
            entered.store(true);
            while (!context.Cancelled())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return false;
        });
    while (!entered.load()) std::this_thread::yield();
    std::atomic_bool queuedRan{false};
    std::atomic_bool queuedCompletion{false};
    const Ulid queued = manager.Enqueue(
        MediaTaskKind::Thumbnail, "queued.mov",
        [&](MediaTaskContext&, std::string&) {
            queuedRan.store(true);
            return true;
        },
        [&](const MediaTaskSnapshot& task) {
            queuedCompletion.store(task.state == MediaTaskState::Cancelled);
        });
    check(manager.Cancel(queued), "queued task accepts cancellation");
    check(queuedCompletion.load() && !queuedRan.load(),
          "queued cancellation completes without running work");
    check(manager.Cancel(running), "running task accepts cancellation");
    check(manager.WaitForIdle(1000), "cancelled task reaches idle");
    snapshot = manager.Snapshot();
    const auto cancelled = std::find_if(
        snapshot.begin(), snapshot.end(),
        [&](const MediaTaskSnapshot& task) { return task.id == running; });
    check(cancelled != snapshot.end() &&
              cancelled->state == MediaTaskState::Cancelled,
          "running task reports cancelled state");

    const Ulid failed =
        manager.Enqueue(MediaTaskKind::Waveform, "broken.wav",
                        [](MediaTaskContext&, std::string& error) {
                            error = "decode failed";
                            return false;
                        });
    check(manager.WaitForIdle(1000), "failed task reaches idle");
    snapshot = manager.Snapshot();
    const auto failure = std::find_if(
        snapshot.begin(), snapshot.end(),
        [&](const MediaTaskSnapshot& task) { return task.id == failed; });
    check(failure != snapshot.end() &&
              failure->state == MediaTaskState::Failed &&
              failure->error == "decode failed",
          "failure reason is retained");

    if (failures) return 1;
    std::cout << "All media task manager tests passed\n";
    return 0;
}
