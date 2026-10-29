#pragma once

#include "core/json.hpp"
#include "daemon/command_policy.hpp"
#include "daemon/event_loop.hpp"
#include "daemon/json_file.hpp"
#include "domain/live_state.hpp"
#include "service/history_recorder.hpp"
#include "service/screen_time.hpp"
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>

namespace nativetv {
namespace daemon {
class Logger;
class LunaClient;

struct ScreenTimeTimings {
    EventLoop::Milliseconds check{30000};
    EventLoop::Milliseconds grace{60000};
    EventLoop::Milliseconds blank{10000};
    EventLoop::Milliseconds again{300000};
    EventLoop::Milliseconds settle{5000};
    EventLoop::Milliseconds screenSaver{1500};
    EventLoop::Milliseconds verify{3000};
    EventLoop::Milliseconds keyPress{50};
    EventLoop::Milliseconds powerSettle{1500};
    std::function<int64_t()> wallNowMs;
};

// Runs the screen-time policy on the daemon loop using the native state and
// history recorder. Settings persistence remains owned by the config bridge.
class ScreenTimeEnforcer {
  public:
    ScreenTimeEnforcer(EventLoop &loop, state::Cache &state,
                       service::HistoryRecorder &history, LunaClient &luna,
                       const std::string &dataRoot, const std::string &procRoot,
                       const std::string &systemRoot, Json settings, bool allowControl,
                       bool allowPower,
                       Logger *logger = nullptr,
                       ScreenTimeTimings timings = ScreenTimeTimings(),
                       CommandPolicyExecutor *commandPolicy = nullptr);
    ~ScreenTimeEnforcer();
    ScreenTimeEnforcer(const ScreenTimeEnforcer &) = delete;
    ScreenTimeEnforcer &operator=(const ScreenTimeEnforcer &) = delete;

    bool start();
    bool apply(const Json &settings, std::string *error = nullptr);
    void stop();

  private:
    struct Assessment {
        bool valid = false;
        bool screenOn = false;
        int64_t localClockMs = 0;
        std::string day;
        std::string key;
        Json today;
        service::ScreenTimeDue tv;
        service::ScreenTimeDue app;
    };

    void applyOnLoop(const Json &settings);
    bool startChecks();
    void onStateChange(const state::Event &event);
    void powerChanged();
    void check();
    Assessment assess();
    void warnSoon(const Assessment &assessment);
    void warnOnce(const std::string &id, const std::string &message);
    void toast(const std::string &message);
    void run(const std::string &action, const std::string &id);
    void powerOff();
    void turnScreenOff();
    void launchHome();
    bool command(const std::string &action, const std::string &uri,
                 const Json &payload, const std::string &resource,
                 bool requiresPower, LunaClient::Callback completion,
                 const std::string &appId = std::string(), bool urgent = false);
    void injectKey(unsigned code, EventLoop::Milliseconds press,
                   std::function<void(bool)> callback);
    bool writeKeyEvent(unsigned code, int value);
    std::string inputDevicePath() const;
    bool screenIsOn() const;
    void afterSwitchedOff(const service::ScreenTimeDue &due, bool screenOn);
    void blankSoon(EventLoop::Milliseconds delay);
    void cancelPending();
    void saveSwitchedOff();
    void clearSwitchedOff();
    void logControl(const std::string &message, bool ok);

    EventLoop &loop_;
    state::Cache &state_;
    service::HistoryRecorder &history_;
    LunaClient &luna_;
    CommandPolicyExecutor *commandPolicy_;
    Logger *logger_;
    std::shared_ptr<int> lifetime_;
    std::string procRoot_;
    std::string systemRoot_;
    JsonFile switchedOffFile_;
    ScreenTimeTimings timings_;
    Json settings_;
    bool allowControl_;
    bool allowPower_;
    bool hasHomeApp_;
    bool running_ = false;
    bool listening_ = false;
    bool settling_ = false;
    bool wasScreenOn_ = false;
    bool acting_ = false;
    bool pendingAgain_ = false;
    bool pendingTold_ = false;
    bool pendingBlanking_ = false;
    std::string pendingId_;
    std::string warnedDay_;
    std::map<std::string, bool> switchedOff_;
    std::map<std::string, bool> warned_;
    EventLoop::TimerId checkTimer_ = 0;
    EventLoop::TimerId pendingTimer_ = 0;
    EventLoop::TimerId blankTimer_ = 0;
    EventLoop::TimerId settleTimer_ = 0;
    EventLoop::TimerId verifyTimer_ = 0;
    EventLoop::TimerId screenSaverTimer_ = 0;
    EventLoop::TimerId powerTimer_ = 0;
    EventLoop::TimerId keyTimer_ = 0;
    int keyFd_ = -1;
    unsigned keyCode_ = 0;
    std::function<void(bool)> keyCallback_;
};
} // namespace daemon
} // namespace nativetv
