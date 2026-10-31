#include "daemon/daily_backup_scheduler.hpp"
#include "daemon/logger.hpp"
#include <exception>
#include <memory>
#include <stdexcept>
#include <utility>

namespace nativetv {
namespace daemon {
namespace {
struct CheckResult {
    bool succeeded = false;
    std::string error;
};

std::string exceptionText(std::exception_ptr failure) {
    try {
        if (failure)
            std::rethrow_exception(failure);
    } catch (const std::exception &error) {
        return error.what();
    } catch (...) {
        return "unknown error";
    }
    return std::string();
}
} // namespace

DailyBackupScheduler::DailyBackupScheduler(EventLoop &loop, Check check,
                                           Logger *logger,
                                           DailyBackupTimings timings, std::string label)
    : loop_(loop), check_(std::move(check)), logger_(logger), timings_(timings),
      label_(std::move(label)) {
    if (!check_)
        throw std::invalid_argument("daily backup scheduler needs a check callback");
}

DailyBackupScheduler::~DailyBackupScheduler() {
    stop();
}

bool DailyBackupScheduler::start() {
    if (started_)
        return true;
    if (stopped_ || timings_.initialDelay.count() <= 0 || timings_.interval.count() <= 0)
        return false;
    initialTimer_ = loop_.scheduleAfter(timings_.initialDelay, [this]() {
        initialTimer_ = 0;
        if (stopped_)
            return;
        intervalTimer_ = loop_.scheduleEvery(timings_.interval, [this]() { dispatch(); });
        if (!intervalTimer_ && logger_)
            logger_->warning(label_ + ": could not schedule the hourly check");
        dispatch();
    });
    if (!initialTimer_)
        return false;
    started_ = true;
    return true;
}

void DailyBackupScheduler::stop() {
    if (stopped_)
        return;
    stopped_ = true;
    if (initialTimer_)
        (void)loop_.cancelTimer(initialTimer_);
    if (intervalTimer_)
        (void)loop_.cancelTimer(intervalTimer_);
    initialTimer_ = 0;
    intervalTimer_ = 0;
}

void DailyBackupScheduler::dispatch() {
    if (stopped_ || inFlight_)
        return;
    const std::shared_ptr<CheckResult> result(new CheckResult());
    if (!loop_.submitBlocking(
            [check = check_, result]() { result->succeeded = check(result->error); },
            [this, result](std::exception_ptr failure) {
                inFlight_ = false;
                if (!logger_)
                    return;
                const std::string error = failure ? exceptionText(failure) : result->error;
                if (failure || !result->succeeded)
                    logger_->warning(label_ + ": " +
                                     (error.empty() ? "check failed" : error));
            })) {
        if (logger_)
            logger_->warning(label_ + ": worker queue is full; will retry later");
        return;
    }
    inFlight_ = true;
}
} // namespace daemon
} // namespace nativetv
