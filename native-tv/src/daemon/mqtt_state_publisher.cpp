#include "daemon/mqtt_state_publisher.hpp"
#include <algorithm>
#include <utility>

namespace nativetv {
namespace daemon {

MqttStatePublisher::MqttStatePublisher(EventLoop &loop, state::Cache &cache,
                                       MqttTopics topics, Publish publish)
    : loop_(loop), cache_(cache), topics_(std::move(topics)),
      publish_(std::move(publish)), gate_(std::make_shared<CallbackGate>()) {
    gate_->owner = this;
}

MqttStatePublisher::~MqttStatePublisher() {
    stop();
    std::lock_guard<std::mutex> lock(gate_->mutex);
    gate_->owner = nullptr;
}

bool MqttStatePublisher::start() {
    std::lock_guard<std::mutex> lock(gate_->mutex);
    if (started_)
        return !stopped_;
    if (stopped_)
        return false;
    started_ = true;
    const std::weak_ptr<CallbackGate> weakGate(gate_);
    cache_.onChange([weakGate](const state::Event &event) {
        const std::shared_ptr<CallbackGate> gate = weakGate.lock();
        if (!gate)
            return;
        std::lock_guard<std::mutex> lock(gate->mutex);
        if (gate->owner)
            gate->owner->onChange(event);
    });
    return true;
}

void MqttStatePublisher::stop() {
    std::lock_guard<std::mutex> lock(gate_->mutex);
    if (stopped_)
        return;
    stopped_ = true;
    if (emptyRemoteTimer_)
        loop_.cancelTimer(emptyRemoteTimer_);
    emptyRemoteTimer_ = 0;
}

void MqttStatePublisher::publishSnapshot() {
    std::lock_guard<std::mutex> lock(gate_->mutex);
    if (!started_ || stopped_ || !publish_)
        return;
    // Removes the retained release state that servers before the native daemon
    // published here (legacy tvweb.js does the same on connect).
    (void)publish_(topics_.prefix() + "/update", "", true);
    const Json snapshot = cache_.snapshot();
    std::vector<std::string> groups = snapshot.keys();
    std::sort(groups.begin(), groups.end());
    for (const std::string &group : groups) {
        if (group == "media") continue;  // the daemon's own player snapshot, not a state topic
        const Json values = snapshot[group];
        std::vector<std::string> keys = values.keys();
        std::sort(keys.begin(), keys.end());
        for (const std::string &key : keys)
            publishValue(group, key, values[key]);
    }
    publishRemote(true, false);
}

void MqttStatePublisher::onChange(const state::Event &event) {
    if (!started_ || stopped_)
        return;
    publishValue(event.group, event.key, event.value);
    const bool changesRemote =
        (event.group == "application" && event.key == "app_id") ||
        event.group == "surface" || event.group == "focus" ||
        (event.group == "power" && event.key == "systemOn");
    if (changesRemote)
        publishRemote(false, false);
}

void MqttStatePublisher::publishValue(const std::string &group,
                                      const std::string &key,
                                      const Json &value) {
    if (!value.valid() || !publish_ || group == "media")
        return;
    (void)publish_(topics_.state(group + "/" + key), scalar(value), true);
    if (group == "power" && key == "screenOn") {
        bool screenOn = truthy(value);
        const Json power = cache_.snapshot()["power"];
        if (power["systemOn"].isBoolean() && !power["systemOn"].boolean())
            screenOn = false;
        (void)publish_(topics_.state("screen"), screenOn ? "ON" : "OFF", true);
    }
}

void MqttStatePublisher::publishRemote(bool force, bool settled) {
    if (!publish_ || !started_ || stopped_)
        return;
    if (force)
        remoteSnapshotPending_ = true;
    const Json snapshot = cache_.snapshot();
    const Json application = snapshot["application"];
    Json payload = Json::object();
    payload.set("app_id", Json::text(application["app_id"].string()));

    bool hasControlApp = false;
    std::string controlApp;
    for (const char *group : {"surface", "focus"}) {
        const Json value = snapshot[group]["control_app_id"];
        if (value.isString()) {
            hasControlApp = true;
            controlApp = value.string();
        }
    }
    const Json systemOn = snapshot["power"]["systemOn"];
    const bool isOff = systemOn.isBoolean() && !systemOn.boolean();
    if (hasControlApp && isOff)
        controlApp.clear();

    if (hasControlApp && controlApp.empty() && !isOff && !settled) {
        if (!emptyRemoteTimer_) {
            const std::weak_ptr<CallbackGate> weakGate(gate_);
            emptyRemoteTimer_ = loop_.scheduleAfter(EventLoop::Milliseconds(200),
                [weakGate] {
                    const std::shared_ptr<CallbackGate> gate = weakGate.lock();
                    if (!gate)
                        return;
                    std::lock_guard<std::mutex> lock(gate->mutex);
                    if (!gate->owner)
                        return;
                    gate->owner->emptyRemoteTimer_ = 0;
                    gate->owner->publishRemote(false, true);
                });
        }
        // Waiting on the timer, whether it was just set or an earlier event did:
        // another window event meanwhile must not publish the empty focus.
        if (emptyRemoteTimer_)
            return;
        settled = true;
    }
    if (emptyRemoteTimer_) {
        (void)loop_.cancelTimer(emptyRemoteTimer_);
        emptyRemoteTimer_ = 0;
    }
    if (hasControlApp)
        payload.set("control_app_id", Json::text(controlApp));
    const std::string body = payload.dump();
    if (!remoteSnapshotPending_ && body == lastRemoteBody_)
        return;
    remoteSnapshotPending_ = false;
    lastRemoteBody_ = body;
    (void)publish_(topics_.state("application/remote"), body, true);
}

std::string MqttStatePublisher::scalar(const Json &value) {
    if (value.isString())
        return value.string();
    if (value.isBoolean())
        return value.boolean() ? "true" : "false";
    return value.dump();
}

bool MqttStatePublisher::truthy(const Json &value) {
    if (value.isBoolean())
        return value.boolean();
    if (value.isNumber())
        return value.number() != 0;
    if (value.isString())
        return !value.string().empty();
    return value.valid();
}

} // namespace daemon
} // namespace nativetv
