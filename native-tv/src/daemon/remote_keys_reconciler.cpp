#include "daemon/remote_keys_reconciler.hpp"
#include "daemon/logger.hpp"
#include "daemon/luna.hpp"
#include <algorithm>
#include <chrono>
#include <utility>

namespace nativetv {
namespace daemon {
namespace {
EventLoop::Milliseconds positiveDelay(unsigned value) {
    return EventLoop::Milliseconds(value ? value : 1);
}
} // namespace

RemoteKeysReconciler::Options::Options()
    : startupDelayMilliseconds(60000), settingsDebounceMilliseconds(1000),
      periodicMilliseconds(30 * 60 * 1000),
      powerRechecksMilliseconds{3000, 15000, 60000, 180000, 600000},
      writeLimit(12), writeWindowMilliseconds(60000), retryMilliseconds(1000) {}

RemoteKeysReconciler::RemoteKeysReconciler(EventLoop &loop, LunaClient &luna,
                                           Reconcile reconcile, Logger *logger,
                                           Options options)
    : loop_(loop), luna_(luna), reconcile_(std::move(reconcile)),
      logger_(logger), options_(std::move(options)) {}

RemoteKeysReconciler::~RemoteKeysReconciler() { stop(); }

bool RemoteKeysReconciler::start() {
    if (!loop_.isLoopThread() || running_ || !reconcile_)
        return false;
    running_ = true;
    Json payload = Json::object();
    payload.set("category", Json::text("other"));
    Json keys = Json::array();
    keys.add(Json::text("mapping_info"));
    payload.set("keys", keys);
    payload.set("subscribe", Json::flag(true));
    LunaSubscriptionHandlers handlers;
    handlers.message = [this](const Json &) {
        if (!settingsSnapshotSeen_) {
            settingsSnapshotSeen_ = true;
            return;
        }
        settingsChanged();
    };
    handlers.error = [this](const std::string &error) {
        if (running_ && logger_)
            logger_->warning("remotekeys: settings subscription: " + error);
    };
    handlers.close = [this](int code, int signal) {
        if (running_ && logger_ && (code != 0 || signal != 0))
            logger_->warning("remotekeys: settings subscription closed (" +
                             std::to_string(code) + ", " +
                             std::to_string(signal) + ")");
    };
    settingsSubscription_ = luna_.subscribe(
        "com.webos.settingsservice/getSystemSettings", payload, "",
        std::move(handlers));
    if (!settingsSubscription_ || !settingsSubscription_->start()) {
        running_ = false;
        settingsSubscription_.reset();
        return false;
    }
    startupTimer_ = loop_.scheduleAfter(
        positiveDelay(options_.startupDelayMilliseconds),
        [this]() { startupTimer_ = 0; requestReconcile(); });
    periodicTimer_ = loop_.scheduleEvery(
        positiveDelay(options_.periodicMilliseconds),
        [this]() { requestReconcile(); });
    if (!startupTimer_ || !periodicTimer_) {
        stop();
        return false;
    }
    return true;
}

void RemoteKeysReconciler::stop() {
    if (!running_ && !settingsSubscription_)
        return;
    running_ = false;
    if (startupTimer_) loop_.cancelTimer(startupTimer_);
    if (settingsTimer_) loop_.cancelTimer(settingsTimer_);
    if (periodicTimer_) loop_.cancelTimer(periodicTimer_);
    if (retryTimer_) loop_.cancelTimer(retryTimer_);
    startupTimer_ = settingsTimer_ = periodicTimer_ = retryTimer_ = 0;
    for (const EventLoop::TimerId timer : powerTimers_)
        if (timer) loop_.cancelTimer(timer);
    powerTimers_.clear();
    if (settingsSubscription_) settingsSubscription_->stop();
    settingsSubscription_.reset();
    pending_ = false;
}

void RemoteKeysReconciler::powerChanged() {
    if (!running_ || !loop_.isLoopThread())
        return;
    for (const EventLoop::TimerId timer : powerTimers_)
        if (timer) loop_.cancelTimer(timer);
    powerTimers_.clear();
    for (const unsigned delay : options_.powerRechecksMilliseconds) {
        const EventLoop::TimerId timer = loop_.scheduleAfter(
            positiveDelay(delay), [this]() { requestReconcile(); });
        if (timer) powerTimers_.push_back(timer);
    }
}

void RemoteKeysReconciler::settingsChanged() {
    if (!running_ || rateLimited())
        return;
    if (settingsTimer_) loop_.cancelTimer(settingsTimer_);
    settingsTimer_ = loop_.scheduleAfter(
        positiveDelay(options_.settingsDebounceMilliseconds),
        [this]() {
            settingsTimer_ = 0;
            requestReconcile();
        });
}

void RemoteKeysReconciler::requestReconcile() {
    if (!running_ || !loop_.isLoopThread())
        return;
    if (rateLimited())
        return;
    if (inFlight_) {
        pending_ = true;
        return;
    }
    inFlight_ = true;
    const std::shared_ptr<bool> wrote = std::make_shared<bool>(false);
    const std::shared_ptr<std::string> error =
        std::make_shared<std::string>();
    const Reconcile reconcile = reconcile_;
    if (!loop_.submitBlocking(
            [reconcile, wrote, error]() {
                if (!reconcile(*wrote, *error) && error->empty())
                    *error = "native remote-key reconciliation failed";
            },
            [this, wrote, error](std::exception_ptr failure) {
                inFlight_ = false;
                if (failure && error->empty())
                    *error = "native remote-key reconciliation raised an exception";
                if (running_ && !error->empty() && logger_)
                    logger_->warning("remotekeys: " + *error);
                if (*wrote) {
                    recordWrite();
                    if (logger_)
                        logger_->info("remotekeys: LG settings reapplied");
                }
                const bool runPending = pending_;
                pending_ = false;
                if (running_ && runPending && !rateLimited()) {
                    retryTimer_ = loop_.scheduleAfter(
                        positiveDelay(options_.retryMilliseconds),
                        [this]() {
                            retryTimer_ = 0;
                            requestReconcile();
                        });
                }
            })) {
        inFlight_ = false;
        if (logger_)
            logger_->warning("remotekeys: native reconciliation queue is full");
        retryTimer_ = loop_.scheduleAfter(
            positiveDelay(options_.retryMilliseconds),
            [this]() {
                retryTimer_ = 0;
                requestReconcile();
            });
    }
}

bool RemoteKeysReconciler::rateLimited() {
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    const std::chrono::steady_clock::duration window =
        positiveDelay(options_.writeWindowMilliseconds);
    while (!writes_.empty() && now - writes_.front() >= window)
        writes_.pop_front();
    return options_.writeLimit && writes_.size() >= options_.writeLimit;
}

void RemoteKeysReconciler::recordWrite() {
    writes_.push_back(std::chrono::steady_clock::now());
    (void)rateLimited();
}

} // namespace daemon
} // namespace nativetv
