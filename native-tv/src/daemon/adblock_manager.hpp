#pragma once

#include "daemon/event_loop.hpp"
#include "service/adblock_store.hpp"
#include <atomic>
#include <string>

namespace nativetv {
namespace daemon {

class Logger;
class LunaClient;

class AdBlockManager {
  public:
    // mountOperations replaces the bind mount of the hosts table, for a test.
    AdBlockManager(EventLoop &loop, LunaClient &luna, std::string dataRoot,
                   std::string systemRoot, Logger *logger = nullptr,
                   service::AdBlockMountOperations mountOperations =
                       service::AdBlockMountOperations());
    AdBlockManager(const AdBlockManager &) = delete;
    AdBlockManager &operator=(const AdBlockManager &) = delete;

    bool start(std::string *error = nullptr);
    void stop();

    service::AdBlockStore &store();
    bool sdpGraceActive() const;
    Json setMode(const std::string &mode);
    Json setTvUpdatesBlocked(bool blocked);
    Json setCustomHosts(const Json &options);

  private:
    void learnCountry();
    void reconcileSdpGrace();
    void reconcileSdpGraceOnLoop();
    void checkSdpClock(bool firstCheck);
    void beginSdpPolling();
    void acceptAnyTimeSource();
    void finalizeSdpGrace(const std::string &message);
    void cancelSdpTimers();

    EventLoop &loop_;
    LunaClient &luna_;
    Logger *logger_;
    service::AdBlockStore store_;
    std::atomic<bool> sdpGraceActive_{true};
    std::atomic<bool> stopped_{false};
    bool sdpMonitoring_ = false;
    bool sdpAnySourceAccepted_ = false;
    bool sdpCheckPending_ = false;
    EventLoop::TimerId sdpPollTimer_ = 0;
    EventLoop::TimerId sdpCapTimer_ = 0;
};

} // namespace daemon
} // namespace nativetv
