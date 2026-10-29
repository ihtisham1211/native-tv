#include "daemon/command_policy.hpp"
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace nativetv;
using namespace nativetv::daemon;

namespace {
void check(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}

ChildRunnerOptions fastStarts() {
    ChildRunnerOptions options;
    options.startupGap = std::chrono::milliseconds(0);
    options.regularGap = std::chrono::milliseconds(0);
    options.startupWindow = std::chrono::milliseconds(0);
    return options;
}

bool hasAudit(const Logger &logger, const std::string &needle) {
    const std::vector<LogRecord> records = logger.snapshot(100, "command-policy:");
    for (const LogRecord &record : records)
        if (record.message.find(needle) != std::string::npos)
            return true;
    return false;
}

void testPolicyEvaluation() {
    FirmwareVersion parsed;
    check(parseFirmwareVersion(" v10.3.1-01 ", parsed) && parsed.valid &&
              parsed.major == 10 && parsed.minor == 3 && parsed.patch == 1,
          "firmware versions accept a v prefix and build suffix");
    FirmwareVersion minorOnly;
    check(parseFirmwareVersion("9.2", minorOnly) && minorOnly.patch == 0 &&
              compareFirmwareVersions(minorOnly, parsed) < 0,
          "two-part firmware versions compare with an implicit zero patch");
    check(!parseFirmwareVersion("10.x.1", parsed) && !parsed.valid &&
              !parseFirmwareVersion("10.3.", parsed) &&
              !parseFirmwareVersion("10.3.1suffix", parsed),
          "malformed firmware versions fail closed");

    CommandPolicySpec command;
    command.action = "power.off";
    command.uri = "com.webos.service.tvpower/power/powerOff";
    command.requiresPower = true;
    command.requiresConfirmation = true;
    command.firmware.hasMinimum = true;
    check(parseFirmwareVersion("9.0", command.firmware.minimum),
          "test minimum firmware version parses");
    command.firmware.hasMaximum = true;
    check(parseFirmwareVersion("10.3.1", command.firmware.maximum),
          "test maximum firmware version parses");
    CommandPolicyContext context;
    context.firmwareVersion = "10.3.1";
    check(evaluateCommandPolicy(command, context, false).kind ==
              CommandDecisionKind::ConfirmationRequired,
          "dangerous commands request explicit confirmation");
    check(evaluateCommandPolicy(command, context, true).kind == CommandDecisionKind::Allowed,
          "confirmed commands within the firmware range are admitted");
    context.allowControl = false;
    check(evaluateCommandPolicy(command, context, true).kind == CommandDecisionKind::Denied,
          "the global control gate rejects writes");
    context.allowControl = true;
    context.allowPower = false;
    check(evaluateCommandPolicy(command, context, true).kind == CommandDecisionKind::Denied,
          "the power gate rejects power transitions");
    context.allowPower = true;
    context.firmwareVersion = "8.4.0";
    check(evaluateCommandPolicy(command, context, true).kind == CommandDecisionKind::Denied,
          "firmware below the minimum is rejected");
    context.firmwareVersion.clear();
    check(evaluateCommandPolicy(command, context, true).kind == CommandDecisionKind::Denied,
          "a guarded command is rejected when firmware is unknown");

    command.firmware.minimum = command.firmware.maximum;
    command.timeout = std::chrono::milliseconds(0);
    check(evaluateCommandPolicy(command, context, true).kind == CommandDecisionKind::Invalid,
          "non-positive timeouts are invalid policy descriptors");
    command.timeout = std::chrono::milliseconds(3500);
    command.firmware.minimum.major = 11;
    check(evaluateCommandPolicy(command, context, true).kind == CommandDecisionKind::Invalid,
          "inverted firmware ranges are rejected");
}

void testExecutorTimeoutLocksAndAudit() {
    EventLoop loop(4, 16, 32);
    ChildProcessRunner children(fastStarts());
    LunaClientOptions options;
    options.executable = NATIVETV_FAKE_LUNA_SEND;
    options.environment.push_back("NATIVETV_FAKE_LUNA_MODE=slow");
    options.timeout = std::chrono::milliseconds(3000);
    LunaClient luna(loop, children, nullptr, options);

    LoggerOptions loggerOptions;
    loggerOptions.sink = [](LogLevel, const std::string &, bool) {};
    Logger logger(loggerOptions);
    CommandPolicyExecutor executor(luna, &logger);
    CommandPolicyContext context;

    CommandPolicySpec command;
    command.action = "audio.set-volume";
    command.uri = "luna://com.webos.audio/setVolume";
    command.payload.set("volume", Json::numeric(22));
    command.resource = "audio-settings";
    command.timeout = std::chrono::milliseconds(70);

    CommandPolicyContext deniedContext = context;
    deniedContext.allowControl = false;
    const CommandDecision denied = executor.submit(
        command, deniedContext, false, [](const LunaReply &) {});
    check(denied.kind == CommandDecisionKind::Denied,
          "disabled controls are refused before dispatch");

    std::mutex mutex;
    std::condition_variable condition;
    std::vector<LunaReply> replies;
    std::thread loopThread([&]() { loop.run(); });
    const CommandDecision first = executor.submit(command, context, false,
        [&](const LunaReply &reply) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                replies.push_back(reply);
            }
            condition.notify_one();
        });
    check(first.kind == CommandDecisionKind::Queued,
          "allowed commands enter the Luna queue");
    const CommandDecision busy = executor.submit(command, context, false,
        [](const LunaReply &) {});
    check(busy.kind == CommandDecisionKind::ResourceBusy,
          "matching resource locks prevent overlapping writes");

    {
        std::unique_lock<std::mutex> lock(mutex);
        check(condition.wait_for(lock, std::chrono::seconds(2), [&]() {
                  return replies.size() == 1;
              }), "timed command did not complete");
    }
    check(replies[0].timedOut && !replies[0].error.empty(),
          "per-command timeout is applied by Luna and reported to the caller");

    command.timeout = std::chrono::milliseconds(1000);
    const CommandDecision next = executor.submit(command, context, false,
        [&](const LunaReply &reply) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                replies.push_back(reply);
            }
            condition.notify_one();
        });
    check(next.kind == CommandDecisionKind::Queued,
          "resource lock is released after completion");
    {
        std::unique_lock<std::mutex> lock(mutex);
        check(condition.wait_for(lock, std::chrono::seconds(2), [&]() {
                  return replies.size() == 2;
              }), "second command did not complete");
    }
    loop.stop();
    loopThread.join();

    check(replies[1].error.empty() && replies[1].response["returnValue"].boolean(),
          "a command with a sufficient timeout completes normally");
    check(hasAudit(logger, "result=denied") && hasAudit(logger, "result=resource-busy") &&
              hasAudit(logger, "result=timeout") && hasAudit(logger, "result=success"),
          "policy decisions and command outcomes are written to the audit logger");

    CommandPolicySpec local;
    local.action = "control.media-key";
    local.uri = "native/input/writeKey";
    local.resource = "remote-input";
    local.timeout = std::chrono::milliseconds(100);
    unsigned executions = 0;
    LunaReply localReply;
    const CommandDecision localDecision = executor.executeLocal(
        local, context, false, [&executions]() { ++executions; return true; },
        localReply);
    check(localDecision.kind == CommandDecisionKind::Allowed && executions == 1 &&
              localReply.response["returnValue"].boolean() &&
              hasAudit(logger, "action=control.media-key result=success"),
          "bounded native commands share policy admission and success auditing");

    CommandPolicyContext localDenied = context;
    localDenied.allowControl = false;
    const CommandDecision localDeniedDecision = executor.executeLocal(
        local, localDenied, false, [&executions]() { ++executions; return true; },
        localReply);
    check(localDeniedDecision.kind == CommandDecisionKind::Denied && executions == 1,
          "local side effects honor the same global control gate as Luna writes");

    CommandDecision nestedDecision;
    localReply = LunaReply();
    (void)executor.executeLocal(local, context, false, [&]() {
        LunaReply nestedReply;
        nestedDecision = executor.executeLocal(
            local, context, false, [&executions]() { ++executions; return true; },
            nestedReply);
        return true;
    }, localReply);
    check(nestedDecision.kind == CommandDecisionKind::ResourceBusy && executions == 1,
          "local commands hold the shared resource lock while their side effect runs");

    local.timeout = std::chrono::milliseconds(15);
    const CommandDecision localTimeout = executor.executeLocal(
        local, context, false, []() {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            return true;
        }, localReply);
    check(localTimeout.kind == CommandDecisionKind::Allowed && localReply.timedOut &&
              !localReply.response["returnValue"].boolean() &&
              hasAudit(logger, "action=control.media-key result=timeout"),
          "local side effects report and audit work that exceeds its command deadline");
}
} // namespace

int main() {
    try {
        testPolicyEvaluation();
        testExecutorTimeoutLocksAndAudit();
        std::puts("Command policy checks passed");
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "Command policy test failed: %s\n", error.what());
        return 1;
    }
}
