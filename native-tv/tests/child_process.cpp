#include "daemon/child_process.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>
#include <sys/resource.h>

using namespace nativetv::daemon;

namespace {
void check(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}

ChildOptions shell(const std::string &script, std::chrono::milliseconds timeout) {
    ChildOptions options;
    options.arguments = {"/bin/sh", "-c", script};
    options.timeout = timeout;
    return options;
}

void testCapturedExit() {
    ChildProcessRunner runner;
    ChildResult result = runner.run(shell("printf 'stdout-line'; printf 'stderr-line' >&2; exit 7",
                                          std::chrono::milliseconds(1000)));
    check(result.error.empty() && result.exitCode == 7 && !result.timedOut,
          "runner should return a child exit code");
    check(result.standardOutput == "stdout-line" && result.standardError == "stderr-line",
          "runner should capture stdout and stderr independently");
    check(result.endedAt >= result.startedAt, "runner should expose monotonic start/end times");
}

void testTimeoutAndOutputLimit() {
    ChildProcessRunner runner;
    ChildOptions slow;
    slow.arguments = {"/bin/sleep", "2"};
    slow.timeout = std::chrono::milliseconds(40);
    ChildResult timed = runner.run(slow);
    check(timed.timedOut && timed.signal != 0,
          "runner should terminate a child that exceeds its timeout");

    ChildOptions excessive = shell("printf '0123456789abcdef'", std::chrono::milliseconds(1000));
    excessive.maximumOutputBytes = 8;
    ChildResult overflow = runner.run(excessive);
    check(overflow.outputLimitExceeded && overflow.standardOutput.size() == 8,
          "runner should cap child output and stop the child");
}

void testSingleTurnSpacingAndUrgentOrder() {
    ChildRunnerOptions scheduler;
    scheduler.startupGap = std::chrono::milliseconds(35);
    scheduler.regularGap = std::chrono::milliseconds(5);
    scheduler.startupWindow = std::chrono::seconds(30);
    ChildProcessRunner runner(scheduler);

    ChildOptions blocker = shell("sleep 0.12", std::chrono::seconds(1));
    ChildOptions normal = shell("sleep 0.02", std::chrono::seconds(1));
    ChildOptions urgent = shell("sleep 0.02", std::chrono::seconds(1));
    urgent.urgent = true;
    ChildResult blocked;
    ChildResult first;
    ChildResult second;
    std::thread a([&]() { blocked = runner.run(blocker); });
    std::this_thread::sleep_for(std::chrono::milliseconds(8));
    std::thread b([&]() { first = runner.run(normal); });
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    std::thread c([&]() { second = runner.run(urgent); });
    a.join();
    b.join();
    c.join();
    check(blocked.succeeded() && first.succeeded() && second.succeeded(),
          "all queued children should complete");
    check(second.startedAt - blocked.startedAt >= std::chrono::milliseconds(25) &&
              first.startedAt - second.startedAt >= std::chrono::milliseconds(25),
          "serialized child starts should respect the startup spacing");
    check(second.startedAt < first.startedAt,
          "urgent queued child should run ahead of an unstarted normal child");
}

// A poll that times out counts as voluntary on Linux and involuntary on macOS.
long contextSwitches() {
    rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_nvcsw + usage.ru_nivcsw;
}

// A resident child (no deadline) must not wake its reader 20 times a second: eight of them sit
// in the daemon at idle. At the old 50 ms poll this 2 s run switched more than 40 times.
void testResidentChildWakesRarely() {
    ChildProcessRunner runner;
    const long before = contextSwitches();
    ChildResult result = runner.run(shell("sleep 2", std::chrono::milliseconds(0)));
    const long switches = contextSwitches() - before;
    check(result.succeeded(), "a child without a deadline should run to its exit");
    check(switches < 25, "a resident child woke " + std::to_string(switches) + " times in 2 s");
}

void testCancelWithoutDeadlineIsNoticed() {
    ChildProcessRunner runner;
    ChildOptions resident = shell("sleep 5", std::chrono::milliseconds(0));
    resident.cancellation = std::make_shared<std::atomic<bool>>(false);
    std::thread canceller([flag = resident.cancellation] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        flag->store(true);
    });
    const auto started = std::chrono::steady_clock::now();
    ChildResult result = runner.run(resident);
    canceller.join();
    const auto took = std::chrono::steady_clock::now() - started;
    check(result.cancelled, "a cancelled resident child should report it");
    // The child sleeps 5 s: ending well before that is the cancel. A second was too tight for a
    // sanitizer build on a CI runner that runs four tests at once (it took 7 s there).
    check(took < std::chrono::seconds(3), "a cancel should end the child within a few seconds");
}

// Both pipes close while the child lives on; the exit must still be noticed.
void testExitAfterPipesClose() {
    ChildProcessRunner runner;
    ChildResult result = runner.run(shell("exec >&- 2>&-; sleep 0.2; exit 3",
                                          std::chrono::seconds(2)));
    check(result.error.empty() && result.exitCode == 3 && !result.timedOut,
          "a child that closed its pipes should still be reaped with its exit code");
}
} // namespace

int main() {
    try {
        testCapturedExit();
        testTimeoutAndOutputLimit();
        testSingleTurnSpacingAndUrgentOrder();
        testResidentChildWakesRarely();
        testCancelWithoutDeadlineIsNoticed();
        testExitAfterPipesClose();
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "child process test failed: %s\n", error.what());
        return 1;
    }
}
