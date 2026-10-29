#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace nativetv {
namespace daemon {
struct ChildOptions {
    std::vector<std::string> arguments;
    std::vector<std::string> environment;
    std::chrono::milliseconds timeout{3500};
    size_t maximumOutputBytes = 4 * 1024 * 1024;
    bool urgent = false;
    bool captureOutput = true;
    std::shared_ptr<std::atomic<bool>> cancellation;
    std::function<void(const std::string &)> onStandardOutput;
    std::function<void(const std::string &)> onStandardError;
};

struct ChildResult {
    int exitCode = -1;
    int signal = 0;
    bool timedOut = false;
    bool cancelled = false;
    bool outputLimitExceeded = false;
    std::string standardOutput;
    std::string standardError;
    std::string error;
    std::chrono::steady_clock::time_point startedAt;
    std::chrono::steady_clock::time_point endedAt;

    bool succeeded() const { return error.empty() && !timedOut && !outputLimitExceeded && exitCode == 0; }
};

struct ChildRunnerOptions {
    std::chrono::milliseconds startupGap{150};
    std::chrono::milliseconds regularGap{50};
    std::chrono::milliseconds startupWindow{30000};
    std::chrono::milliseconds terminateGrace{100};
};

// Executes argv directly with posix_spawn, bounded output and a deadline.
// Spawn calls share a single urgent-aware launch queue and startup spacing.
class ChildProcessRunner {
  public:
    explicit ChildProcessRunner(ChildRunnerOptions options = ChildRunnerOptions());
    ChildProcessRunner(const ChildProcessRunner &) = delete;
    ChildProcessRunner &operator=(const ChildProcessRunner &) = delete;

    ChildResult run(const ChildOptions &options);

  private:
    struct Ticket {
        explicit Ticket(bool priority) : urgent(priority) {}
        bool urgent;
    };

    ChildRunnerOptions options_;
    std::chrono::steady_clock::time_point bornAt_;
    std::chrono::steady_clock::time_point nextLaunchAt_;
    std::mutex launchMutex_;
    std::condition_variable launchCondition_;
    std::deque<Ticket *> launches_;
};
} // namespace daemon
} // namespace nativetv
