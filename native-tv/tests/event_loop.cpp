#include "daemon/event_loop.hpp"
#include "daemon/heartbeat.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

using nativetv::daemon::EventLoop;
using namespace std::chrono;

namespace {
void check(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

void testBlockingWorkDoesNotBlockLoop() {
    EventLoop loop(1, 1, 8);
    std::atomic<unsigned> ticks(0);
    std::atomic<bool> timerOnLoop(false), completionOnLoop(false), workFailed(false);
    EventLoop::TimerId heartbeat = loop.scheduleEvery(milliseconds(5), [&]() {
        if (loop.isLoopThread())
            timerOnLoop = true;
        ++ticks;
    });
    check(heartbeat != 0, "repeating timer was not scheduled");
    check(loop.submitBlocking([]() {
              std::this_thread::sleep_for(milliseconds(70));
              throw std::runtime_error("worker failure");
          },
          [&](std::exception_ptr failure) {
              completionOnLoop = loop.isLoopThread();
              workFailed = static_cast<bool>(failure);
              loop.cancelTimer(heartbeat);
              loop.stop();
          }),
          "blocking work was not accepted");
    loop.run();
    check(ticks.load() >= 3, "loop timers stalled while a worker was busy");
    check(timerOnLoop.load(), "timer callback ran outside the loop thread");
    check(completionOnLoop.load(), "worker completion ran outside the loop thread");
    check(workFailed.load(), "worker exception did not reach its completion");
    check(!loop.isLoopThread(), "loop thread identity remained set after run returned");
}

void testBoundedQueuesAndDrainOnStop() {
    EventLoop loop(1, 1, 3);
    std::promise<void> started;
    std::promise<void> release;
    std::shared_future<void> gate(release.get_future());
    std::atomic<unsigned> completions(0), posted(0);
    std::atomic<bool> callbacksOnLoop(true);

    check(loop.post([&]() { ++posted; }), "loop task should fit the ready queue");
    check(loop.submitBlocking(
              [&]() {
                  started.set_value();
                  gate.wait();
              },
              [&](std::exception_ptr error) {
                  callbacksOnLoop = callbacksOnLoop && loop.isLoopThread() && !error;
                  ++completions;
              }),
          "first worker item was not accepted");
    started.get_future().wait();
    check(loop.submitBlocking([]() {}, [&](std::exception_ptr error) {
              callbacksOnLoop = callbacksOnLoop && loop.isLoopThread() && !error;
              if (++completions == 2)
                  loop.stop();
          }),
          "one queued worker item should fit");
    check(!loop.submitBlocking([]() {}, EventLoop::Completion()),
          "work queue accepted more than its configured limit");
    check(!loop.post([]() {}), "ready plus reserved completion slots exceeded the limit");
    check(loop.scheduleAfter(milliseconds(10), [&]() { release.set_value(); }) != 0,
          "release timer was not scheduled");

    loop.run();
    check(completions.load() == 2, "stop did not drain accepted worker completions");
    check(posted.load() == 1, "accepted loop task did not run");
    check(callbacksOnLoop.load(), "completion callback ran outside the loop thread");
}

void testTimerCancellation() {
    EventLoop loop(1, 1, 4);
    std::atomic<unsigned> cancelledCalls(0), stopCalls(0);
    EventLoop::TimerId cancelled = loop.scheduleAfter(milliseconds(5), [&]() {
        ++cancelledCalls;
    });
    check(cancelled != 0 && loop.cancelTimer(cancelled), "timer cancellation failed");
    check(!loop.cancelTimer(cancelled), "cancelled timer remained registered");
    check(loop.scheduleAfter(milliseconds(15), [&]() {
              ++stopCalls;
              loop.stop();
          }) != 0,
          "stop timer was not scheduled");
    loop.run();
    check(cancelledCalls.load() == 0, "cancelled timer callback ran");
    check(stopCalls.load() == 1, "one-shot timer did not run exactly once");
}

void testInjectedClock() {
    std::mutex clockMutex;
    std::chrono::steady_clock::time_point fakeNow;
    EventLoop loop(1, 2, 4, EventLoop::ErrorHandler(), [&]() {
        std::lock_guard<std::mutex> lock(clockMutex);
        return fakeNow;
    });
    std::promise<void> scheduled;
    std::atomic<bool> fired(false);
    check(loop.post([&]() {
              check(loop.scheduleAfter(seconds(5), [&]() {
                        fired = true;
                        loop.stop();
                    }) != 0,
                    "injected-clock timer could not be scheduled");
              scheduled.set_value();
          }),
          "injected-clock setup task was not accepted");
    std::thread loopThread([&]() { loop.run(); });
    scheduled.get_future().wait();
    {
        std::lock_guard<std::mutex> lock(clockMutex);
        fakeNow += seconds(5);
    }
    loop.notifyClockChanged();
    loopThread.join();
    check(fired.load(), "advancing the injected clock did not release the timer");
}

void testLoopThreadHeartbeat() {
    char pattern[] = "/tmp/nativetv-heartbeat-XXXXXX";
    char *root = mkdtemp(pattern);
    check(root != nullptr, "could not create heartbeat test directory");
    std::string path = std::string(root) + "/tvweb.beat";
    {
        EventLoop loop(1, 2, 4);
        nativetv::daemon::Heartbeat heartbeat(loop, path, milliseconds(5));
        check(heartbeat.start(), "heartbeat did not start");
        check(loop.scheduleAfter(milliseconds(35), [&]() {
                  heartbeat.stop();
                  loop.stop();
              }) != 0,
              "heartbeat test stop timer was not scheduled");
        loop.run();
        check(!heartbeat.running() && !heartbeat.failed(),
              "heartbeat stopped unexpectedly or failed to write");
        check(heartbeat.writes() >= 3, "heartbeat did not repeat on the loop timer");
    }
    std::ifstream input(path.c_str());
    std::string value;
    input >> value;
    check(!value.empty() && std::all_of(value.begin(), value.end(),
                                        [](unsigned char c) { return std::isdigit(c); }),
          "heartbeat file does not contain an epoch timestamp");
    struct stat info;
    check(stat(path.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600,
          "heartbeat file permissions are not 0600");
    std::remove(path.c_str());
    rmdir(root);
}
} // namespace

int main() {
    try {
        testBlockingWorkDoesNotBlockLoop();
        testBoundedQueuesAndDrainOnStop();
        testTimerCancellation();
        testInjectedClock();
        testLoopThreadHeartbeat();
        std::cout << "event loop: timers, bounded workers, completion affinity, shutdown and heartbeat pass\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "event loop: " << error.what() << "\n";
        return 1;
    }
}
