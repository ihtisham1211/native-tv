#pragma once

#include "daemon/command_policy.hpp"
#include "core/json.hpp"

#include <mutex>
#include <string>

namespace nativetv {
namespace daemon {
class HomeScreenManager;
class HomeTakeoverManager;
class LunaClient;
class SamRescanManager;

// Sequences the Home mount with SAM's firmware-specific manifest reread.
class HomeTakeoverLifecycleManager {
  public:
    HomeTakeoverLifecycleManager(HomeScreenManager &homeScreen,
                                 HomeTakeoverManager &takeover,
                                 SamRescanManager &samRescan,
                                 LunaClient &luna,
                                 std::string systemRoot);

    // The takeover and the release also tell the preload manager (below).
    Json takeover(bool blankStartLayout,
                  const CommandPolicyContext &context);
    Json release(const CommandPolicyContext &context);

    // On webOS 10 and later the preload manager launches LG's Home now and
    // then, which with Native Tv Home under its id brings Native Tv Home in
    // front of the app being watched. hold tells it not to keep LG's Home
    // loaded; !hold lets it again. The policy is not saved, so the preload
    // manager loses it when it restarts, and the caller sends it again.
    // False where this TV is not on webOS 10 or later, or the TV refused.
    bool holdLgHomePreload(bool hold);
    // Whether this TV needs holdLgHomePreload at all.
    bool needsLgHomePreloadHold() const { return preloadHold_; }

  private:
    bool appInfo(Json &info);
    bool manifestAccepted(bool nativeTvManifest);
    bool reread(bool nativeTvManifest,
                const CommandPolicyContext &context);
    bool waitForManifest(bool nativeTvManifest);
    void rollbackTakeover(const CommandPolicyContext &context);

    HomeScreenManager &homeScreen_;
    HomeTakeoverManager &takeover_;
    SamRescanManager &samRescan_;
    LunaClient &luna_;
    bool blockFirst_ = false;
    bool preloadHold_ = false;
    std::mutex mutex_;
};
} // namespace daemon
} // namespace nativetv
