#pragma once

#include "core/json.hpp"
#include "daemon/command_policy.hpp"
#include "service/screensaver_policy.hpp"
#include <atomic>
#include <memory>
#include <mutex>
#include <string>

namespace nativetv {
namespace daemon {
class ChildProcessRunner;
class LunaClient;
class Logger;
class SamRescanManager;

// Owns the privileged filesystem staging and bind mount for custom QML savers,
// plus dismissal of a Native Tv-launched saver on native remote input. The
// boot hook delegates firmware policy and SAM rereads to this manager.
class ScreensaverManager {
  public:
    ScreensaverManager(ChildProcessRunner &children, LunaClient &luna,
                       std::string systemRoot, std::string assetsRoot,
                       Logger *logger = nullptr);

    void reconcileStartup();
    // The policy inputs as the files on the TV give them, for /api/screensaver
    // and the Server tab's flags. Reads only; takes no lock.
    service::ScreensaverState state(bool allowControl, bool allowedAnyway) const;
    Json reconcileBoot(SamRescanManager &samRescan,
                       const CommandPolicyContext &context,
                       bool allowExperimental);
    void remoteKeyPressed();
    Json prepare(const std::string &mode, const std::string &level,
                 unsigned short publicPort, const std::string &token);
    Json restoreStock();
    Json unmount();
    Json restage(const std::string &mode, const std::string &level,
                 unsigned short publicPort, const std::string &token);

  private:
    ChildProcessRunner &children_;
    LunaClient &luna_;
    std::string systemRoot_;
    std::string assetsRoot_;
    Logger *logger_;
    std::shared_ptr<std::atomic<bool>> remoteClosePending_;
    std::mutex mutex_;
};
} // namespace daemon
} // namespace nativetv
