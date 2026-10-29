#include "daemon/event_loop.hpp"
#include <stdexcept>
#include <utility>

namespace nativetv {
namespace daemon {
EventLoop::EventLoop(size_t workerCount, size_t workQueueLimit, size_t loopQueueLimit,
                     ErrorHandler errors, Now now)
    : workQueueLimit_(workQueueLimit), loopQueueLimit_(loopQueueLimit),
      errors_(std::move(errors)),
      now_(now ? std::move(now) : Now([]() { return Clock::now(); })) {
    if (!workerCount || !workQueueLimit_ || !loopQueueLimit_)
        throw std::invalid_argument("event loop and worker queue limits must be positive");
    try {
        for (size_t i = 0; i < workerCount; ++i)
            workers_.emplace_back(&EventLoop::workerLoop, this);
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        workerCondition_.notify_all();
        for (auto &worker : workers_)
            if (worker.joinable())
                worker.join();
        throw;
    }
}

EventLoop::~EventLoop() {
    stop();
    for (auto &worker : workers_)
        if (worker.joinable())
            worker.join();
}

bool EventLoop::post(Task task) {
    if (!task)
        return false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || ready_.size() + reservedCompletions_ >= loopQueueLimit_)
            return false;
        ready_.push(std::move(task));
    }
    loopCondition_.notify_one();
    return true;
}

EventLoop::TimerId EventLoop::scheduleAfter(Milliseconds delay, Task task) {
    if (!task)
        return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || timerIndex_.size() >= loopQueueLimit_)
        return 0;
    Timer timer{nextTimerId_++, now_() + delay, Clock::duration::zero(), std::move(task)};
    auto inserted = timers_.emplace(timer.due, std::move(timer));
    timerIndex_[inserted->second.id] = inserted;
    loopCondition_.notify_one();
    return inserted->second.id;
}

EventLoop::TimerId EventLoop::scheduleEvery(Milliseconds interval, Task task) {
    if (!task || interval.count() <= 0)
        return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || timerIndex_.size() >= loopQueueLimit_)
        return 0;
    Timer timer{nextTimerId_++, now_() + interval, interval, std::move(task)};
    auto inserted = timers_.emplace(timer.due, std::move(timer));
    timerIndex_[inserted->second.id] = inserted;
    loopCondition_.notify_one();
    return inserted->second.id;
}

bool EventLoop::cancelTimer(TimerId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = timerIndex_.find(id);
    if (found == timerIndex_.end())
        return false;
    timers_.erase(found->second);
    timerIndex_.erase(found);
    loopCondition_.notify_one();
    return true;
}

bool EventLoop::submitBlocking(Task work, Completion completion) {
    if (!work)
        return false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || work_.size() >= workQueueLimit_ ||
            (completion && ready_.size() + reservedCompletions_ >= loopQueueLimit_))
            return false;
        if (completion)
            ++reservedCompletions_;
        work_.push({std::move(work), std::move(completion)});
    }
    workerCondition_.notify_one();
    return true;
}

void EventLoop::run() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_ || finished_)
            throw std::logic_error("event loop can only run once");
        running_ = true;
        loopThread_ = std::this_thread::get_id();
    }
    while (true) {
        Task task;
        std::unique_lock<std::mutex> lock(mutex_);
        if (!takeNext(task, lock)) {
            loopThread_ = std::thread::id();
            running_ = false;
            finished_ = true;
            return;
        }
        lock.unlock();
        execute(std::move(task));
    }
}

void EventLoop::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        timers_.clear();
        timerIndex_.clear();
    }
    loopCondition_.notify_all();
    workerCondition_.notify_all();
}

void EventLoop::notifyClockChanged() {
    loopCondition_.notify_all();
}

bool EventLoop::isLoopThread() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_ && loopThread_ == std::this_thread::get_id();
}

bool EventLoop::takeNext(Task &task, std::unique_lock<std::mutex> &lock) {
    while (true) {
        auto now = now_();
        if (!timers_.empty() && timers_.begin()->first <= now) {
            auto first = timers_.begin();
            Timer timer = first->second;
            timers_.erase(first);
            if (timer.interval != Clock::duration::zero() && !stopping_) {
                timer.due += timer.interval;
                if (timer.due <= now)
                    timer.due = now + timer.interval;
                auto inserted = timers_.emplace(timer.due, timer);
                timerIndex_[timer.id] = inserted;
            } else {
                timerIndex_.erase(timer.id);
            }
            task = std::move(timer.task);
            return true;
        }
        if (!ready_.empty()) {
            task = std::move(ready_.front());
            ready_.pop();
            return true;
        }
        if (stopping_ && work_.empty() && activeWorkers_ == 0 && reservedCompletions_ == 0)
            return false;
        if (!timers_.empty()) {
            Clock::duration delay = timers_.begin()->first - now;
            if (delay > Clock::duration::zero())
                loopCondition_.wait_for(lock, delay);
        } else {
            loopCondition_.wait(lock);
        }
    }
}

void EventLoop::workerLoop() {
    while (true) {
        WorkItem item;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            workerCondition_.wait(lock, [this]() { return stopping_ || !work_.empty(); });
            if (work_.empty())
                return;
            item = std::move(work_.front());
            work_.pop();
            ++activeWorkers_;
        }
        std::exception_ptr failure;
        try {
            item.work();
        } catch (...) {
            failure = std::current_exception();
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            --activeWorkers_;
            if (item.completion) {
                --reservedCompletions_;
                ready_.push([completion = std::move(item.completion), failure]() {
                    completion(failure);
                });
            }
        }
        loopCondition_.notify_one();
    }
}

void EventLoop::execute(Task task) {
    try {
        task();
    } catch (...) {
        if (!errors_) {
            stop();
            return;
        }
        try {
            errors_(std::current_exception());
        } catch (...) {
            stop();
        }
    }
}
} // namespace daemon
} // namespace nativetv
