#pragma once

#include "daemon/event_loop.hpp"
#include "daemon/http_proxy.hpp"
#include "domain/live_state.hpp"
#include <deque>
#include <string>

namespace nativetv {
namespace daemon {
class Logger;

// Serializes native state events to the existing Node consumers. Failed posts
// stay queued and retry with backoff until the private Node listener is ready.
class StateNodeBridge {
  public:
    StateNodeBridge(EventLoop &loop, HttpProxy proxy, Logger *logger = nullptr);
    StateNodeBridge(const StateNodeBridge &) = delete;
    StateNodeBridge &operator=(const StateNodeBridge &) = delete;

    void publish(const state::Event &event);
    void publishStatus(const std::string &group, bool live);
    void publishSyslogStatus(const Json &status);
    void publishNtpStatus(const Json &status);
    void publishMqttStatus(const Json &status);
    void publishRemoteInput(const std::string &event, unsigned code,
                            bool pressed, const std::string &button = "");

  private:
    void enqueue(Json message);
    void enqueueOnLoop(Json message);
    void pump();
    void retry();

    EventLoop &loop_;
    HttpProxy proxy_;
    Logger *logger_;
    std::deque<Json> pending_;
    EventLoop::TimerId retryTimer_ = 0;
    unsigned retryDelayMs_ = 250;
    bool inFlight_ = false;
    bool warningLogged_ = false;
};
} // namespace daemon
} // namespace nativetv
