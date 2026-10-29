#pragma once

#include "daemon/event_loop.hpp"
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace nativetv {
namespace daemon {
class Logger;
class LunaClient;
class LunaSubscription;

// Reapplies native-filter remote-key settings after LG resets them at startup,
// during settings writes, or while the TV enters/leaves standby.
class RemoteKeysReconciler {
  public:
    using Reconcile = std::function<bool(bool &, std::string &)>;

    struct Options {
        Options();
        unsigned startupDelayMilliseconds;
        unsigned settingsDebounceMilliseconds;
        unsigned periodicMilliseconds;
        std::vector<unsigned> powerRechecksMilliseconds;
        unsigned writeLimit;
        unsigned writeWindowMilliseconds;
        unsigned retryMilliseconds;
    };

    RemoteKeysReconciler(EventLoop &loop, LunaClient &luna, Reconcile reconcile,
                         Logger *logger = nullptr, Options options = Options());
    ~RemoteKeysReconciler();
    RemoteKeysReconciler(const RemoteKeysReconciler &) = delete;
    RemoteKeysReconciler &operator=(const RemoteKeysReconciler &) = delete;

    bool start();
    void stop();
    void powerChanged();

  private:
    void settingsChanged();
    void requestReconcile();
    bool rateLimited();
    void recordWrite();

    EventLoop &loop_;
    LunaClient &luna_;
    Reconcile reconcile_;
    Logger *logger_;
    Options options_;
    std::shared_ptr<LunaSubscription> settingsSubscription_;
    EventLoop::TimerId startupTimer_ = 0;
    EventLoop::TimerId settingsTimer_ = 0;
    EventLoop::TimerId periodicTimer_ = 0;
    EventLoop::TimerId retryTimer_ = 0;
    std::vector<EventLoop::TimerId> powerTimers_;
    std::deque<std::chrono::steady_clock::time_point> writes_;
    bool running_ = false;
    bool settingsSnapshotSeen_ = false;
    bool inFlight_ = false;
    bool pending_ = false;
};
} // namespace daemon
} // namespace nativetv
