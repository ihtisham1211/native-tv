#pragma once

#include "core/json.hpp"
#include "daemon/command_policy.hpp"
#include <mutex>
#include <string>
#include <vector>

namespace nativetv {
namespace daemon {

class ChildProcessRunner;
class SamRescanManager;

// Owns the reversible appinfo overrides and bind mounts used by app tile hiding.
class AppTileManager {
  public:
    AppTileManager(ChildProcessRunner &children, std::string systemRoot,
                   std::string dataRoot);

    Json apply(const std::string &action, const std::string &appId);
    Json restoreHiddenAppsAtBoot(const Json &config,
                                 SamRescanManager &samRescan,
                                 const CommandPolicyContext &context);

  private:
    struct Target {
        std::string appInfo;
        std::string appDirectory;
    };

    std::vector<Target> targetsFor(const std::string &appId) const;
    std::string overridePath(const std::string &appId) const;
    bool unmountTargets(const std::vector<Target> &targets);
    bool mountTargets(const std::string &overrideFile,
                      const std::vector<Target> &targets,
                      std::vector<std::string> *errors);
    bool mountPersistedOverride(const std::string &appId);
    Json hide(const std::string &appId);
    Json unhide(const std::string &appId);
    Json setMounted(const std::string &appId, bool mounted);

    ChildProcessRunner &children_;
    std::string systemRoot_;
    std::string dataRoot_;
    std::mutex mutex_;
};

bool appTileIdIsProtected(const std::string &appId);
bool appTileIdIsValid(const std::string &appId);

} // namespace daemon
} // namespace nativetv
