#pragma once

#include "core/json.hpp"
#include "daemon/command_policy.hpp"
#include "daemon/event_loop.hpp"
#include "daemon/luna.hpp"
#include "domain/live_state.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace nativetv {
namespace daemon {
class Logger;

struct VolumeCeilingTimings {
    EventLoop::Milliseconds check{30000};    // a night window starting under a volume already set
    EventLoop::Milliseconds settle{3000};    // after the TV comes on, for the audio service to be up
    EventLoop::Milliseconds refused{300000}; // before trying an output that takes no set level again
    int bootSeconds = 180;                   // a server this young at start-up started with the TV
    std::function<int64_t()> wallNowMs;
    std::function<double()> steadyNowMs;
    std::function<double()> uptimeSeconds;   // /proc/uptime when unset
};

// Keeps the TV's volume at or under the ceiling, whoever sets it (the remote, an
// app, the dashboard, Home Assistant), and sets the start-up volume when the TV
// comes on. The settings are saved by the config bridge.
class VolumeCeilingEnforcer {
  public:
    VolumeCeilingEnforcer(EventLoop &loop, state::Cache &state, LunaClient &luna,
                          const std::string &procRoot, Json settings, bool allowControl,
                          Logger *logger = nullptr,
                          VolumeCeilingTimings timings = VolumeCeilingTimings(),
                          CommandPolicyExecutor *commandPolicy = nullptr);
    ~VolumeCeilingEnforcer();
    VolumeCeilingEnforcer(const VolumeCeilingEnforcer &) = delete;
    VolumeCeilingEnforcer &operator=(const VolumeCeilingEnforcer &) = delete;

    bool start();
    // The whole settings section, as service::volumeCeilingSettings returns it.
    bool apply(const Json &settings, std::string *error = nullptr);
    void stop();

  private:
    void applyOnLoop(const Json &settings);
    void onStateChange(const state::Event &event);
    void powerChanged();
    void startupSoon(EventLoop::Milliseconds delay);
    void enforce();
    void setVolume(int level, const std::string &why);
    int64_t localClockMs() const;
    double steadyNow() const;
    bool bootedJustNow() const;
    bool command(const std::string &action, const std::string &uri, const Json &payload,
                 const std::string &resource, LunaClient::Callback completion);

    EventLoop &loop_;
    state::Cache &state_;
    LunaClient &luna_;
    CommandPolicyExecutor *commandPolicy_;
    Logger *logger_;
    std::shared_ptr<int> lifetime_;
    std::string procRoot_;
    VolumeCeilingTimings timings_;
    Json settings_;
    bool allowControl_;
    bool running_ = false;
    bool wasScreenOn_ = false;
    bool setting_ = false;
    bool recheck_ = false;
    bool toldControlsOff_ = false;
    double refusedUntil_ = 0;
    EventLoop::TimerId checkTimer_ = 0;
    EventLoop::TimerId startupTimer_ = 0;
};
} // namespace daemon
} // namespace nativetv
