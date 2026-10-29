#include "daemon/command_policy.hpp"
#include <algorithm>
#include <cctype>
#include <limits>

namespace nativetv {
namespace daemon {
namespace {
const std::chrono::milliseconds kMaximumCommandTimeout(60000);

std::string trim(const std::string &value) {
    size_t begin = 0;
    while (begin < value.size() &&
           std::isspace(static_cast<unsigned char>(value[begin])))
        ++begin;
    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])))
        --end;
    return value.substr(begin, end - begin);
}

bool safeToken(const std::string &value, size_t maximum) {
    if (value.empty() || value.size() > maximum)
        return false;
    for (unsigned char character : value)
        if (!(std::isalnum(character) || character == '_' || character == '-' ||
              character == '.' || character == ':'))
            return false;
    return true;
}

bool validUri(const std::string &value) {
    const std::string uri = value.compare(0, 7, "luna://") == 0 ? value.substr(7) : value;
    const size_t separator = uri.find('/');
    if (separator == std::string::npos || separator == 0 || separator + 1 >= uri.size())
        return false;
    for (unsigned char character : uri)
        if (std::isspace(character) || std::iscntrl(character))
            return false;
    return true;
}

bool hasFirmwareGuard(const FirmwareGuard &guard) {
    return guard.hasMinimum || guard.hasMaximum;
}

std::string decisionOutcome(CommandDecisionKind kind) {
    switch (kind) {
    case CommandDecisionKind::Allowed: return "allowed";
    case CommandDecisionKind::Queued: return "queued";
    case CommandDecisionKind::Denied: return "denied";
    case CommandDecisionKind::ConfirmationRequired: return "confirmation-required";
    case CommandDecisionKind::ResourceBusy: return "resource-busy";
    case CommandDecisionKind::Invalid: return "invalid";
    case CommandDecisionKind::TransportUnavailable: return "transport-unavailable";
    }
    return "invalid";
}

std::string safeReason(const std::string &value) {
    std::string result;
    result.reserve(std::min<size_t>(value.size(), 160));
    for (unsigned char character : value) {
        if (result.size() >= 160)
            break;
        result.push_back(std::iscntrl(character) ? ' ' : static_cast<char>(character));
    }
    return result;
}

bool parseComponent(const std::string &text, size_t &offset, unsigned &result) {
    if (offset >= text.size() || !std::isdigit(static_cast<unsigned char>(text[offset])))
        return false;
    unsigned value = 0;
    do {
        const unsigned digit = static_cast<unsigned>(text[offset] - '0');
        if (value > (std::numeric_limits<unsigned>::max() - digit) / 10)
            return false;
        value = value * 10 + digit;
        ++offset;
    } while (offset < text.size() &&
             std::isdigit(static_cast<unsigned char>(text[offset])));
    result = value;
    return true;
}

bool validVersion(const FirmwareVersion &version) {
    return version.valid;
}
} // namespace

bool parseFirmwareVersion(const std::string &text, FirmwareVersion &result) {
    result = FirmwareVersion{};
    std::string normalized = trim(text);
    if (!normalized.empty() && (normalized[0] == 'v' || normalized[0] == 'V'))
        normalized.erase(0, 1);

    size_t offset = 0;
    if (!parseComponent(normalized, offset, result.major) || offset >= normalized.size() ||
        normalized[offset++] != '.' || !parseComponent(normalized, offset, result.minor)) {
        result = FirmwareVersion{};
        return false;
    }
    if (offset < normalized.size() && normalized[offset] == '.') {
        ++offset;
        if (!parseComponent(normalized, offset, result.patch)) {
            result = FirmwareVersion{};
            return false;
        }
    }
    if (offset < normalized.size() && normalized[offset] != '-' && normalized[offset] != '+') {
        result = FirmwareVersion{};
        return false;
    }
    if (offset < normalized.size() && offset + 1 == normalized.size()) {
        result = FirmwareVersion{};
        return false;
    }
    result.valid = true;
    return true;
}

int compareFirmwareVersions(const FirmwareVersion &left, const FirmwareVersion &right) {
    if (left.major != right.major)
        return left.major < right.major ? -1 : 1;
    if (left.minor != right.minor)
        return left.minor < right.minor ? -1 : 1;
    if (left.patch != right.patch)
        return left.patch < right.patch ? -1 : 1;
    return 0;
}

CommandDecision evaluateCommandPolicy(const CommandPolicySpec &command,
                                      const CommandPolicyContext &context,
                                      bool confirmed) {
    if (!safeToken(command.action, 64) || !validUri(command.uri) ||
        command.timeout.count() <= 0 || command.timeout > kMaximumCommandTimeout ||
        (!command.appId.empty() && !safeToken(command.appId, 128)) ||
        (!command.resource.empty() && !safeToken(command.resource, 64)))
        return {CommandDecisionKind::Invalid, "invalid command descriptor"};

    if (command.firmware.hasMinimum && !validVersion(command.firmware.minimum))
        return {CommandDecisionKind::Invalid, "invalid minimum firmware guard"};
    if (command.firmware.hasMaximum && !validVersion(command.firmware.maximum))
        return {CommandDecisionKind::Invalid, "invalid maximum firmware guard"};
    if (command.firmware.hasMinimum && command.firmware.hasMaximum &&
        compareFirmwareVersions(command.firmware.minimum, command.firmware.maximum) > 0)
        return {CommandDecisionKind::Invalid, "firmware guard range is inverted"};

    if (command.requiresControl && !context.allowControl)
        return {CommandDecisionKind::Denied, "control is disabled"};
    if (command.requiresPower && !context.allowPower)
        return {CommandDecisionKind::Denied, "power control is disabled"};

    if (hasFirmwareGuard(command.firmware)) {
        FirmwareVersion firmware;
        if (!parseFirmwareVersion(context.firmwareVersion, firmware))
            return {CommandDecisionKind::Denied, "firmware version is unknown"};
        if (command.firmware.hasMinimum &&
            compareFirmwareVersions(firmware, command.firmware.minimum) < 0)
            return {CommandDecisionKind::Denied, "firmware is below the supported range"};
        if (command.firmware.hasMaximum &&
            compareFirmwareVersions(firmware, command.firmware.maximum) > 0)
            return {CommandDecisionKind::Denied, "firmware is above the supported range"};
    }

    if (command.requiresConfirmation && !confirmed)
        return {CommandDecisionKind::ConfirmationRequired, "explicit confirmation is required"};
    return {CommandDecisionKind::Allowed, std::string()};
}

CommandPolicyExecutor::CommandPolicyExecutor(LunaClient &luna, Logger *logger)
    : luna_(luna), logger_(logger) {}

CommandDecision CommandPolicyExecutor::submit(const CommandPolicySpec &command,
                                              const CommandPolicyContext &context,
                                              bool confirmed,
                                              LunaClient::Callback completion) {
    CommandDecision decision = evaluateCommandPolicy(command, context, confirmed);
    if (decision.kind != CommandDecisionKind::Allowed) {
        audit(command.action, decisionOutcome(decision.kind), decision.reason);
        return decision;
    }
    if (!completion) {
        decision = {CommandDecisionKind::Invalid, "completion callback is required"};
        audit(command.action, decisionOutcome(decision.kind), decision.reason);
        return decision;
    }

    unsigned long long lease = 0;
    if (!acquire(command.resource, lease)) {
        decision = {CommandDecisionKind::ResourceBusy, "command resource is already in use"};
        audit(command.action, decisionOutcome(decision.kind), decision.reason);
        return decision;
    }

    const std::chrono::steady_clock::time_point submittedAt =
        std::chrono::steady_clock::now();
    audit(command.action, "started");
    const bool accepted = luna_.call(
        command.uri, command.payload,
        [this, action = command.action, resource = command.resource, lease, submittedAt,
         completion = std::move(completion)](const LunaReply &reply) {
            release(resource, lease);
            const long elapsedMs = static_cast<long>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - submittedAt).count());
            std::string outcome = "success";
            std::string reason;
            if (reply.timedOut) {
                outcome = "timeout";
                reason = "Luna call exceeded its timeout";
            } else if (!reply.error.empty()) {
                outcome = "failure";
                reason = "Luna transport failed";
            } else if (reply.response["returnValue"].isBoolean() &&
                       !reply.response["returnValue"].boolean()) {
                outcome = "failure";
                reason = "Luna service rejected the command";
            }
            audit(action, outcome, reason, elapsedMs);
            completion(reply);
        },
        command.appId, command.urgent, command.timeout);
    if (!accepted) {
        release(command.resource, lease);
        decision = {CommandDecisionKind::TransportUnavailable,
                    "Luna command queue is unavailable"};
        audit(command.action, decisionOutcome(decision.kind), decision.reason);
        return decision;
    }
    return {CommandDecisionKind::Queued, std::string()};
}

CommandDecision CommandPolicyExecutor::executeLocal(
    const CommandPolicySpec &command, const CommandPolicyContext &context,
    bool confirmed, const std::function<bool()> &work, LunaReply &result) {
    CommandDecision decision = evaluateCommandPolicy(command, context, confirmed);
    if (decision.kind != CommandDecisionKind::Allowed) {
        audit(command.action, decisionOutcome(decision.kind), decision.reason);
        return decision;
    }
    if (!work) {
        decision = {CommandDecisionKind::Invalid, "local command work is required"};
        audit(command.action, decisionOutcome(decision.kind), decision.reason);
        return decision;
    }

    unsigned long long lease = 0;
    if (!acquire(command.resource, lease)) {
        decision = {CommandDecisionKind::ResourceBusy,
                    "command resource is already in use"};
        audit(command.action, decisionOutcome(decision.kind), decision.reason);
        return decision;
    }

    result = LunaReply();
    result.startedAt = std::chrono::steady_clock::now();
    audit(command.action, "started");
    bool succeeded = false;
    try {
        succeeded = work();
    } catch (...) {
        result.error = "local command failed";
    }
    result.endedAt = std::chrono::steady_clock::now();
    const long elapsedMs = static_cast<long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            result.endedAt - result.startedAt).count());
    result.timedOut = result.endedAt - result.startedAt > command.timeout;
    if (result.timedOut)
        result.error = "local command exceeded its timeout";
    result.response = Json::object();
    result.response.set("returnValue", Json::flag(succeeded && !result.timedOut));
    const std::string outcome = result.timedOut ? "timeout"
        : succeeded ? "success" : "failure";
    const std::string reason = result.timedOut ? result.error
        : succeeded ? std::string() : "local command failed";
    audit(command.action, outcome, reason, elapsedMs);
    release(command.resource, lease);
    return {CommandDecisionKind::Allowed, std::string()};
}

bool CommandPolicyExecutor::acquire(const std::string &resource,
                                    unsigned long long &lease) {
    if (resource.empty())
        return true;
    std::lock_guard<std::mutex> lock(mutex_);
    if (resources_.find(resource) != resources_.end())
        return false;
    lease = nextLease_++;
    if (lease == 0)
        lease = nextLease_++;
    resources_[resource] = lease;
    return true;
}

void CommandPolicyExecutor::release(const std::string &resource,
                                    unsigned long long lease) {
    if (resource.empty())
        return;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = resources_.find(resource);
    if (found != resources_.end() && found->second == lease)
        resources_.erase(found);
}

void CommandPolicyExecutor::audit(const std::string &action, const std::string &outcome,
                                  const std::string &reason, long elapsedMs) const {
    if (!logger_)
        return;
    std::string message = "command-policy: action=" + safeReason(action) +
                          " result=" + safeReason(outcome);
    if (!reason.empty())
        message += " reason=" + safeReason(reason);
    if (elapsedMs >= 0)
        message += " elapsed_ms=" + std::to_string(elapsedMs);
    if (outcome == "denied" || outcome == "confirmation-required" ||
        outcome == "resource-busy" || outcome == "invalid" ||
        outcome == "transport-unavailable" || outcome == "failure" ||
        outcome == "timeout")
        logger_->warning(message);
    else
        logger_->info(message);
}

} // namespace daemon
} // namespace nativetv
