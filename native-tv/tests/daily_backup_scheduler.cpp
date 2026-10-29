#include "daemon/daily_backup_scheduler.hpp"
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace {
void require(bool condition, const char *message) {
    if (condition) return;
    std::fprintf(stderr, "%s\n", message);
    std::exit(1);
}
}

int main() {
    using namespace nativetv::daemon;
    using Clock = std::chrono::steady_clock;
    std::mutex clockMutex;
    Clock::time_point fakeNow;
    EventLoop loop(1, 4, 16, EventLoop::ErrorHandler(), [&]() {
        std::lock_guard<std::mutex> lock(clockMutex);
        return fakeNow;
    });

    std::mutex mutex;
    std::condition_variable condition;
    int checks = 0;
    bool releaseCheck = false;
    bool firstCheckStarted = false;
    bool earlyCheckpoint = false;
    int checksAtEarlyCheckpoint = -1;
    int processedTicks = 0;
    bool schedulerStopped = false;
    DailyBackupTimings timings;
    timings.initialDelay = EventLoop::Milliseconds(10);
    timings.interval = EventLoop::Milliseconds(10);
    DailyBackupScheduler scheduler(
        loop,
        [&](std::string &) {
            std::unique_lock<std::mutex> lock(mutex);
            ++checks;
            firstCheckStarted = true;
            condition.notify_all();
            condition.wait(lock, [&]() { return releaseCheck; });
            return true;
        },
        nullptr, timings);

    require(scheduler.start(), "daily backup scheduler did not start");
    require(loop.scheduleAfter(EventLoop::Milliseconds(5), [&]() {
                std::lock_guard<std::mutex> lock(mutex);
                earlyCheckpoint = true;
                checksAtEarlyCheckpoint = checks;
                condition.notify_all();
            }) != 0,
            "could not schedule the pre-delay checkpoint");
    std::thread loopThread([&]() { loop.run(); });

    auto advanceClockAndWait = [&](int milliseconds) {
        int expected = 0;
        {
            std::lock_guard<std::mutex> lock(mutex);
            expected = processedTicks + 1;
        }
        {
            std::lock_guard<std::mutex> lock(clockMutex);
            fakeNow = Clock::time_point() + std::chrono::milliseconds(milliseconds);
        }
        loop.notifyClockChanged();
        require(loop.post([&]() {
                    std::lock_guard<std::mutex> lock(mutex);
                    ++processedTicks;
                    condition.notify_all();
                }),
                "could not post a scheduler clock checkpoint");
        std::unique_lock<std::mutex> lock(mutex);
        require(condition.wait_for(lock, std::chrono::seconds(2), [&]() {
                    return processedTicks >= expected;
                }),
                "event loop did not process an advanced scheduler clock");
    };

    advanceClockAndWait(5);
    {
        std::unique_lock<std::mutex> lock(mutex);
        require(condition.wait_for(lock, std::chrono::seconds(2), [&]() {
                    return earlyCheckpoint;
                }),
                "pre-delay checkpoint did not run");
        require(checksAtEarlyCheckpoint == 0,
                "daily backup check ran before the initial delay");
    }
    advanceClockAndWait(10);
    {
        std::unique_lock<std::mutex> lock(mutex);
        require(condition.wait_for(lock, std::chrono::seconds(2), [&]() {
                    return firstCheckStarted;
                }),
                "initial daily backup check did not run after its delay");
    }
    advanceClockAndWait(20);
    advanceClockAndWait(30);
    advanceClockAndWait(40);
    require(loop.post([&]() {
                scheduler.stop();
                loop.stop();
                std::lock_guard<std::mutex> lock(mutex);
                schedulerStopped = true;
                condition.notify_all();
            }),
            "could not schedule scheduler shutdown");
    {
        std::unique_lock<std::mutex> lock(mutex);
        require(condition.wait_for(lock, std::chrono::seconds(2), [&]() {
                    return schedulerStopped;
                }),
                "daily backup scheduler did not stop");
        require(checks == 1, "scheduler ran overlapping daily backup checks");
        releaseCheck = true;
        condition.notify_all();
    }
    loopThread.join();
    require(checks == 1, "scheduler dispatched another check while stopping");
    return 0;
}
