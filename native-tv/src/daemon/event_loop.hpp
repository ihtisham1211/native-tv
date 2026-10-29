#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <map>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <vector>

namespace nativetv {
namespace daemon {
class EventLoop {
  public:
    using Task = std::function<void()>;
    using Completion = std::function<void(std::exception_ptr)>;
    using ErrorHandler = std::function<void(std::exception_ptr)>;
    using TimerId = unsigned long long;
    using Milliseconds = std::chrono::milliseconds;
    using Now = std::function<std::chrono::steady_clock::time_point()>;

    explicit EventLoop(size_t workerCount = 2, size_t workQueueLimit = 32,
                       size_t loopQueueLimit = 128, ErrorHandler errors = ErrorHandler(),
                       Now now = Now());
    ~EventLoop();
    EventLoop(const EventLoop &) = delete;
    EventLoop &operator=(const EventLoop &) = delete;

    // Posts short, nonblocking work to the loop thread. False means the bounded
    // queue is full or the loop has begun stopping.
    bool post(Task task);

    TimerId scheduleAfter(Milliseconds delay, Task task);
    TimerId scheduleEvery(Milliseconds interval, Task task);
    bool cancelTimer(TimerId id);

    // Runs blocking I/O on the bounded worker pool. Completion always returns
    // to the loop thread and receives any exception thrown by work.
    bool submitBlocking(Task work, Completion completion = Completion());

    // run() owns the calling thread until stop() and all accepted work finish.
    void run();
    void stop();
    // Wake a timer wait after advancing an injected test clock.
    void notifyClockChanged();
    bool isLoopThread() const;

  private:
    using Clock = std::chrono::steady_clock;
    struct Timer {
        TimerId id;
        Clock::time_point due;
        Clock::duration interval;
        Task task;
    };
    struct WorkItem {
        Task work;
        Completion completion;
    };

    void workerLoop();
    bool takeNext(Task &task, std::unique_lock<std::mutex> &lock);
    void execute(Task task);

    const size_t workQueueLimit_;
    const size_t loopQueueLimit_;
    ErrorHandler errors_;
    Now now_;
    mutable std::mutex mutex_;
    std::condition_variable loopCondition_;
    std::condition_variable workerCondition_;
    std::queue<Task> ready_;
    std::queue<WorkItem> work_;
    std::multimap<Clock::time_point, Timer> timers_;
    std::unordered_map<TimerId, std::multimap<Clock::time_point, Timer>::iterator> timerIndex_;
    std::vector<std::thread> workers_;
    TimerId nextTimerId_ = 1;
    size_t activeWorkers_ = 0;
    size_t reservedCompletions_ = 0;
    bool stopping_ = false;
    bool running_ = false;
    bool finished_ = false;
    std::thread::id loopThread_;
};
} // namespace daemon
} // namespace nativetv
