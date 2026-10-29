#pragma once

#include "core/json.hpp"
#include "daemon/command_policy.hpp"
#include <chrono>
#include <mutex>
#include <string>

namespace nativetv {
namespace daemon {
class Logger;
class ChildProcessRunner;
// Owns SAM's managed nudge, visibility verification, launch-point settling and
// restart recovery beside the privileged Luna calls. Node keeps public app
// lifecycle decisions and the standalone fallback.
class SamRescanManager {
  public:
    SamRescanManager(CommandPolicyExecutor &policy, ChildProcessRunner &children,
                     std::string systemRoot,
                     Logger *logger = nullptr);

    Json canNudge(const CommandPolicyContext &context);
    Json nudge(const CommandPolicyContext &context);
    Json rereadApp(const std::string &appId,
                   const CommandPolicyContext &context);
    bool releaseBlockedApp(const std::string &appId,
                           const CommandPolicyContext &context);
    bool reportsAppType(const std::string &appId, const std::string &type,
                        const CommandPolicyContext &context,
                        unsigned attempts = 20);
    Json refreshAppVisibility(const std::string &appId, bool visible,
                              const CommandPolicyContext &context,
                              unsigned attempts = 20);
    Json refresh(const Json &expectedVisibility,
                 const CommandPolicyContext &context);
    Json restart(const CommandPolicyContext &context);

  private:
    bool call(const std::string &action, const std::string &uri,
              const Json &payload, const std::string &resource,
              const CommandPolicyContext &context, Json &response,
              std::string *error = nullptr,
              std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));
    bool readBlockedList(const CommandPolicyContext &context, Json &list);
    bool writeBlockedList(const Json &list, const CommandPolicyContext &context);
    bool releaseApp(const std::string &appId, unsigned tries,
                    const CommandPolicyContext &context, bool quiet);
    void delay(unsigned milliseconds) const;
    bool needsForceParameter() const;
    static Json result(bool ok, const std::string &error = std::string());

    CommandPolicyExecutor &policy_;
    ChildProcessRunner &children_;
    std::string systemRoot_;
    Logger *logger_;
    std::mutex operationMutex_;
    std::mutex refreshMutex_;
    std::mutex restartMutex_;
};
} // namespace daemon
} // namespace nativetv
