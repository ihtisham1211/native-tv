#pragma once

#include "daemon/event_loop.hpp"
#include <functional>
#include <string>

namespace nativetv {
namespace daemon {
class Logger;

struct DailyBackupTimings {
    EventLoop::Milliseconds initialDelay = EventLoop::Milliseconds(300000);
    EventLoop::Milliseconds interval = EventLoop::Milliseconds(3600000);
};

// Runs the managed daily-backup check away from the event-loop thread. The
// callback may perform blocking Luna and loopback HTTP work.
class DailyBackupScheduler {
  public:
    using Check = std::function<bool(std::string &)>;

    DailyBackupScheduler(EventLoop &loop, Check check, Logger *logger = nullptr,
                         DailyBackupTimings timings = DailyBackupTimings());
    ~DailyBackupScheduler();
    DailyBackupScheduler(const DailyBackupScheduler &) = delete;
    DailyBackupScheduler &operator=(const DailyBackupScheduler &) = delete;

    bool start();
    void stop();

  private:
    void dispatch();

    EventLoop &loop_;
    Check check_;
    Logger *logger_;
    DailyBackupTimings timings_;
    EventLoop::TimerId initialTimer_ = 0;
    EventLoop::TimerId intervalTimer_ = 0;
    bool started_ = false;
    bool stopped_ = false;
    bool inFlight_ = false;
};
} // namespace daemon
} // namespace nativetv
