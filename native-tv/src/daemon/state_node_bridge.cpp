#include "daemon/state_node_bridge.hpp"
#include "daemon/logger.hpp"
#include <algorithm>
#include <memory>
#include <utility>

namespace nativetv {
namespace daemon {
StateNodeBridge::StateNodeBridge(EventLoop &loop, HttpProxy proxy, Logger *logger)
    : loop_(loop), proxy_(std::move(proxy)), logger_(logger) {}

void StateNodeBridge::publish(const state::Event &event) {
    Json message = Json::object();
    message.set("type", Json::text("state"));
    message.set("group", Json::text(event.group));
    message.set("key", Json::text(event.key));
    message.set("value", event.value);
    message.set("sourceTime", Json::numeric(static_cast<double>(event.sourceTime)));
    message.set("snapshot", Json::flag(event.snapshot));
    message.set("tvReported", Json::flag(event.tvReported));
    enqueue(std::move(message));
}

void StateNodeBridge::publishStatus(const std::string &group, bool live) {
    Json message = Json::object();
    message.set("type", Json::text("status"));
    message.set("group", Json::text(group));
    message.set("live", Json::flag(live));
    enqueue(std::move(message));
}

void StateNodeBridge::publishSyslogStatus(const Json &status) {
    Json message = Json::object();
    message.set("type", Json::text("syslog"));
    message.set("group", Json::text("forwarder"));
    message.set("status", status);
    enqueue(std::move(message));
}

void StateNodeBridge::publishNtpStatus(const Json &status) {
    Json message = Json::object();
    message.set("type", Json::text("ntp"));
    message.set("group", Json::text("client"));
    message.set("status", status);
    enqueue(std::move(message));
}

void StateNodeBridge::publishMqttStatus(const Json &status) {
    Json message = Json::object();
    message.set("type", Json::text("mqtt-status"));
    message.set("status", status);
    enqueue(std::move(message));
}

void StateNodeBridge::publishRemoteInput(const std::string &event,
                                         unsigned code, bool pressed,
                                         const std::string &button) {
    Json message = Json::object();
    message.set("type", Json::text("remote-input"));
    message.set("event", Json::text(event));
    message.set("code", Json::numeric(code));
    message.set("pressed", Json::flag(pressed));
    if (!button.empty())
        message.set("button", Json::text(button));
    enqueue(std::move(message));
}

void StateNodeBridge::enqueue(Json message) {
    if (loop_.isLoopThread()) {
        enqueueOnLoop(std::move(message));
        return;
    }
    (void)loop_.post([this, message]() { enqueueOnLoop(message); });
}

void StateNodeBridge::enqueueOnLoop(Json message) {
    const std::string type = message["type"].string();
    const std::string group = message["group"].string();
    auto samePending = [&](const Json &item) {
        if (item["type"].string() != type || item["group"].string() != group)
            return false;
        if (type == "state")
            return item["key"].string() == message["key"].string();
        return type == "status" || type == "syslog" || type == "ntp" ||
               type == "mqtt-status";
    };

    const auto begin = pending_.begin() + (inFlight_ && !pending_.empty() ? 1 : 0);
    for (auto item = pending_.end(); item != begin;) {
        --item;
        if (samePending(*item)) {
            *item = std::move(message);
            return;
        }
    }
    if (pending_.size() >= 1024) {
        const size_t removeAt = inFlight_ ? 1 : 0;
        if (removeAt >= pending_.size())
            return;
        pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(removeAt));
        if (logger_)
            logger_->warning("state bridge: dropped the oldest queued update at capacity");
    }
    pending_.push_back(std::move(message));
    pump();
}

void StateNodeBridge::pump() {
    if (inFlight_ || retryTimer_ || pending_.empty())
        return;
    const Json message = pending_.front();
    const std::shared_ptr<std::string> error = std::make_shared<std::string>();
    inFlight_ = true;
    if (!loop_.submitBlocking(
            [this, message, error]() { (void)proxy_.requestNodeState(message, *error); },
            [this, error](std::exception_ptr failure) {
                bool succeeded = !failure && error->empty();
                if (inFlight_)
                    inFlight_ = false;
                if (succeeded) {
                    if (!pending_.empty())
                        pending_.pop_front();
                    retryDelayMs_ = 250;
                    warningLogged_ = false;
                    pump();
                    return;
                }
                if (logger_ && !warningLogged_) {
                    logger_->warning("state bridge: Node did not accept a state update" +
                                     (error->empty() ? std::string() : ": " + *error));
                    warningLogged_ = true;
                }
                retry();
            })) {
        inFlight_ = false;
        retry();
    }
}

void StateNodeBridge::retry() {
    if (retryTimer_)
        return;
    const unsigned delay = retryDelayMs_;
    retryDelayMs_ = std::min(retryDelayMs_ * 2, 5000u);
    retryTimer_ = loop_.scheduleAfter(EventLoop::Milliseconds(delay), [this]() {
        retryTimer_ = 0;
        pump();
    });
}
} // namespace daemon
} // namespace nativetv
