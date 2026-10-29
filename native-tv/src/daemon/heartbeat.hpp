#pragma once

#include "daemon/event_loop.hpp"
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace nativetv {
namespace daemon {
class Heartbeat {
  public:
    Heartbeat(EventLoop &loop, std::string path,
              EventLoop::Milliseconds interval = EventLoop::Milliseconds(20000));
    ~Heartbeat();
    Heartbeat(const Heartbeat &) = delete;
    Heartbeat &operator=(const Heartbeat &) = delete;

    bool start();
    void stop();
    bool running() const;
    bool failed() const;
    unsigned long writes() const;

  private:
    struct State {
        explicit State(std::string file) : path(std::move(file)) {}
        std::string path;
        std::atomic<bool> active{false};
        std::atomic<bool> failure{false};
        std::atomic<unsigned long> writeCount{0};
    };

    EventLoop &loop_;
    std::shared_ptr<State> state_;
    EventLoop::Milliseconds interval_;
    mutable std::mutex mutex_;
    EventLoop::TimerId timer_ = 0;
};
} // namespace daemon
} // namespace nativetv
