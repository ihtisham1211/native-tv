#include "daemon/child_process.hpp"
#include <atomic>
#include <chrono>
#include <csignal>
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

// A resident child (no deadline) must not wake its reader while it is silent: seven of them sit
// in the daemon at idle. The old 250 ms poll, kept only to notice a cancel, switched about 8
// times in this 2 s run (50 ms: more than 40); blocked on the cancel's pipe it switches a few.
void testResidentChildWakesRarely() {
    ChildProcessRunner runner;
    ChildOptions resident = shell("sleep 2", std::chrono::milliseconds(0));
    resident.cancellation = std::make_shared<ChildCancellation>();
    const long before = contextSwitches();
    ChildResult result = runner.run(resident);
    const long switches = contextSwitches() - before;
    check(result.succeeded() && !result.cancelled, "a child without a deadline should run to its exit");
    check(switches < 6, "a resident child woke " + std::to_string(switches) + " times in 2 s");
}

void testCancelWithoutDeadlineIsNoticed() {
    ChildProcessRunner runner;
    // exec: dash (Ubuntu's /bin/sh) keeps the shell as the parent of sleep, so a SIGTERM to the
    // shell would leave sleep holding the pipes open, and the run would last the full 5 s.
    ChildOptions resident = shell("exec sleep 5", std::chrono::milliseconds(0));
    resident.cancellation = std::make_shared<ChildCancellation>();
    std::thread canceller([cancellation = resident.cancellation] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        cancellation->cancel();
    });
    const auto started = std::chrono::steady_clock::now();
    ChildResult result = runner.run(resident);
    canceller.join();
    const auto took = std::chrono::steady_clock::now() - started;
    check(result.cancelled, "a cancelled resident child should report it");
    check(result.signal == SIGTERM, "a cancel should end the child with SIGTERM");
    // poll has no timeout here, so only the cancel's pipe can end the wait this soon: 100 ms to
    // the cancel, then the SIGTERM. The 1 s bound leaves room for a loaded host.
    check(took < std::chrono::seconds(1), "a cancel should end a resident child at once");
}

// After a cancel the cancel's pipe leaves the poll set and the kill deadline bounds the wait, so
// a child that ignores SIGTERM is still killed after the grace.
void testCancelEscalatesToKill() {
    ChildRunnerOptions quick;
    quick.terminateGrace = std::chrono::milliseconds(100);
    ChildProcessRunner runner(quick);
    ChildOptions stubborn = shell("trap '' TERM; exec sleep 5", std::chrono::milliseconds(0));
    stubborn.cancellation = std::make_shared<ChildCancellation>();
    std::thread canceller([cancellation = stubborn.cancellation] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        cancellation->cancel();
    });
    const auto started = std::chrono::steady_clock::now();
    ChildResult result = runner.run(stubborn);
    canceller.join();
    const auto took = std::chrono::steady_clock::now() - started;
    check(result.cancelled && result.signal == SIGKILL,
          "a child that ignores SIGTERM should be killed after the grace");
    check(took < std::chrono::seconds(1), "the kill should follow the grace, not the child's exit");
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
        testCancelEscalatesToKill();
        testExitAfterPipesClose();
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "child process test failed: %s\n", error.what());
        return 1;
    }
}
