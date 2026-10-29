#pragma once

#include "daemon/luna.hpp"
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace nativetv {
namespace daemon {

struct FirmwareVersion {
    unsigned major = 0;
    unsigned minor = 0;
    unsigned patch = 0;
    bool valid = false;
};

// Accepts major.minor[.patch], optionally prefixed by v and followed by a
// -build or +build suffix. A failed parse clears result and returns false.
bool parseFirmwareVersion(const std::string &text, FirmwareVersion &result);
int compareFirmwareVersions(const FirmwareVersion &left, const FirmwareVersion &right);

struct FirmwareGuard {
    bool hasMinimum = false;
    FirmwareVersion minimum;
    bool hasMaximum = false;
    FirmwareVersion maximum;
};

struct CommandPolicySpec {
    std::string action;
    std::string uri;
    Json payload = Json::object();
    std::string appId;
    std::chrono::milliseconds timeout{3500};
    bool urgent = false;
    bool requiresControl = true;
    bool requiresPower = false;
    bool requiresConfirmation = false;
    FirmwareGuard firmware;
    // Commands sharing a resource key cannot run concurrently. This is useful
    // for firmware settings APIs that do not safely accept overlapping writes.
    std::string resource;
};

struct CommandPolicyContext {
    bool allowControl = true;
    bool allowPower = true;
    std::string firmwareVersion;
};

enum class CommandDecisionKind {
    Allowed,
    Queued,
    Denied,
    ConfirmationRequired,
    ResourceBusy,
    Invalid,
    TransportUnavailable
};

struct CommandDecision {
    CommandDecisionKind kind = CommandDecisionKind::Invalid;
    std::string reason;
};

// This is a side-effect-free policy check. A caller must use submit() to
// dispatch commands so admission and audit behavior stay consistent.
CommandDecision evaluateCommandPolicy(const CommandPolicySpec &command,
                                     const CommandPolicyContext &context,
                                     bool confirmed);

class CommandPolicyExecutor {
  public:
    explicit CommandPolicyExecutor(LunaClient &luna, Logger *logger = nullptr);
    CommandPolicyExecutor(const CommandPolicyExecutor &) = delete;
    CommandPolicyExecutor &operator=(const CommandPolicyExecutor &) = delete;

    // Policy refusals are returned synchronously and are never sent to Luna.
    // Accepted commands complete on LunaClient's event-loop thread. The
    // executor and logger must outlive all accepted callbacks.
    CommandDecision submit(const CommandPolicySpec &command,
                          const CommandPolicyContext &context, bool confirmed,
                          LunaClient::Callback completion);

    // Runs a bounded native side effect through the same admission, resource
    // locking and audit policy as Luna writes. The caller must keep the work
    // synchronous and within command.timeout.
    CommandDecision executeLocal(const CommandPolicySpec &command,
                                 const CommandPolicyContext &context, bool confirmed,
                                 const std::function<bool()> &work,
                                 LunaReply &result);

  private:
    bool acquire(const std::string &resource, unsigned long long &lease);
    void release(const std::string &resource, unsigned long long lease);
    void audit(const std::string &action, const std::string &outcome,
               const std::string &reason = std::string(), long elapsedMs = -1) const;

    LunaClient &luna_;
    Logger *logger_;
    std::mutex mutex_;
    unsigned long long nextLease_ = 1;
    std::unordered_map<std::string, unsigned long long> resources_;
};

} // namespace daemon
} // namespace nativetv
