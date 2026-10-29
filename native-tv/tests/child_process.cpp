#include "daemon/child_process.hpp"
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>

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
} // namespace

int main() {
    try {
        testCapturedExit();
        testTimeoutAndOutputLimit();
        testSingleTurnSpacingAndUrgentOrder();
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "child process test failed: %s\n", error.what());
        return 1;
    }
}
