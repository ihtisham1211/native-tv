#pragma once

#include "core/json.hpp"
#include "daemon/command_policy.hpp"

#include <string>

namespace nativetv {
class JsonFile;
namespace daemon {
class HomeTakeoverLifecycleManager;

// Completes the standalone tvwebctl Home shutdown/restore operation.
class HomeRestoreManager {
  public:
    HomeRestoreManager(HomeTakeoverLifecycleManager &takeover,
                       CommandPolicyExecutor &policy,
                       std::string dataRoot);

    Json restore(const CommandPolicyContext &context);

  private:
    bool lunaCall(const CommandPolicyContext &context, const std::string &action,
                  const std::string &uri, const Json &payload,
                  const std::string &resource, Json &response,
                  std::string &error);
    bool readSettings(const CommandPolicyContext &context, Json &settings,
                      std::string &error);
    bool writeSettings(const CommandPolicyContext &context,
                       const Json &settings, std::string &error);
    bool restoreStartingNotice(const CommandPolicyContext &context,
                               std::string &error);
    bool restoreLegacyAppSettings(const CommandPolicyContext &context,
                                  const Json &current,
                                  const Json &snapshot,
                                  std::string &error);

    HomeTakeoverLifecycleManager &takeover_;
    CommandPolicyExecutor &policy_;
    std::string dataRoot_;
};
} // namespace daemon
} // namespace nativetv
