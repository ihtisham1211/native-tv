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

// Runs a check five minutes after start and then hourly, away from the
// event-loop thread: the daily backup's, and the opt-in release check's (the
// label names it in the log). The callback may perform blocking Luna, loopback
// HTTP and network work.
class DailyBackupScheduler {
  public:
    using Check = std::function<bool(std::string &)>;

    DailyBackupScheduler(EventLoop &loop, Check check, Logger *logger = nullptr,
                         DailyBackupTimings timings = DailyBackupTimings(),
                         std::string label = "daily backup");
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
    std::string label_;
    EventLoop::TimerId initialTimer_ = 0;
    EventLoop::TimerId intervalTimer_ = 0;
    bool started_ = false;
    bool stopped_ = false;
    bool inFlight_ = false;
};
} // namespace daemon
} // namespace nativetv
