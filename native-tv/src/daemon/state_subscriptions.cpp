#include "daemon/state_subscriptions.hpp"
#include "daemon/logger.hpp"
#include "daemon/luna.hpp"
#include "daemon/media_seen.hpp"
#include "daemon/paths.hpp"
#include <algorithm>
#include <chrono>
#include <cctype>
#include <utility>

namespace nativetv {
namespace daemon {
namespace {
const char *MASTER_VOLUME = "com.webos.service.audio/master/getVolume";
const char *LEGACY_VOLUME = "com.webos.audio/getVolume";
const char *ACB_MEDIA = "com.webos.service.acb/getForegroundAppInfo";
const char *LEGACY_MEDIA = "com.webos.media/getForegroundAppInfo";

Json subscribedPayload() {
    Json payload = Json::object();
    payload.set("subscribe", Json::flag(true));
    return payload;
}

Json picturePayload() {
    Json payload = subscribedPayload();
    payload.set("category", Json::text("picture"));
    Json keys = Json::array();
    for (const char *key : {"backlight", "pictureMode", "energySaving", "screenShift",
                            "logoLuminanceAdjust"})
        keys.add(Json::text(key));
    payload.set("keys", keys);
    return payload;
}

bool containsInsensitive(const std::string &text, const std::string &part) {
    std::string lowered = text;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return lowered.find(part) != std::string::npos;
}

std::int64_t wallTimeMilliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
} // namespace

StateSubscriptions::StateSubscriptions(EventLoop &loop, LunaClient &luna,
                                       state::MappingOptions mapping, Logger *logger,
                                       Invalidate invalidate, StatusChange statusChange)
    : loop_(loop), luna_(luna), mapping_(std::move(mapping)), logger_(logger),
      invalidate_(std::move(invalidate)), statusChange_(std::move(statusChange)),
      mediaSeenPath_(dataDirectory() + "/media_seen") {
    defineGroups();
}

StateSubscriptions::~StateSubscriptions() {
    for (auto &entry : groups_) {
        if (entry.second->subscription)
            entry.second->subscription->stop();
    }
}

void StateSubscriptions::defineGroups() {
    const auto add = [this](const std::string &name, const std::string &uri,
                            const Json &payload) {
        std::unique_ptr<Group> group(new Group());
        group->name = name;
        group->uri = uri;
        group->payload = payload;
        group->subscription = makeSubscription(*group, ++group->generation);
        groups_.emplace(name, std::move(group));
    };
    add("picture", "com.webos.service.settings/getSystemSettings", picturePayload());
    add("power", "com.webos.service.tvpower/power/getPowerState", subscribedPayload());
    add("application", "com.webos.applicationManager/getForegroundAppInfo",
        subscribedPayload());
    add("surface", "com.webos.surfacemanager/getForegroundAppInfo", subscribedPayload());
    add("focus", "com.webos.surfacemanager/getFocusedWindowInfo", subscribedPayload());
    add("audio", MASTER_VOLUME, subscribedPayload());
    add("media", ACB_MEDIA, subscribedPayload());
}

std::shared_ptr<LunaSubscription> StateSubscriptions::makeSubscription(
    Group &group, unsigned long long generation) {
    LunaSubscriptionHandlers handlers;
    handlers.message = [this, &group, generation](const Json &response) {
        receive(group, generation, response);
    };
    handlers.error = [this, &group, generation](const std::string &error) {
        if (group.generation == generation && logger_)
            logger_->warning("state: " + group.name + " subscription: " + error);
    };
    handlers.close = [this, &group, generation](int code, int signal) {
        if (group.generation != generation)
            return;
        group.snapshotPending.store(true, std::memory_order_release);
        setLive(group, false);
        if (logger_ && (code != 0 || signal != 0))
            logger_->warning("state: " + group.name + " subscription closed (" +
                             std::to_string(code) + ", signal " + std::to_string(signal) + ")");
    };
    return luna_.subscribe(group.uri, group.payload, "", std::move(handlers));
}

bool StateSubscriptions::start() {
    if (!loop_.isLoopThread())
        return false;
    for (const char *name : {"picture", "power", "application", "audio", "media"})
        acquire(*groups_.at(name));
    return true;
}

bool StateSubscriptions::stop() {
    if (!loop_.isLoopThread())
        return false;
    for (auto &entry : groups_)
        release(*entry.second);
    return true;
}

bool StateSubscriptions::startGroup(const std::string &name) {
    if (!loop_.isLoopThread())
        return false;
    auto found = groups_.find(name);
    if (found == groups_.end())
        return false;
    acquire(*found->second);
    return true;
}

bool StateSubscriptions::stopGroup(const std::string &name) {
    if (!loop_.isLoopThread())
        return false;
    auto found = groups_.find(name);
    if (found == groups_.end())
        return false;
    release(*found->second);
    return true;
}

bool StateSubscriptions::isLive(const std::string &name) const {
    auto found = groups_.find(name);
    if (found == groups_.end())
        return false;
    return found->second->users.load(std::memory_order_acquire) > 0 &&
           !found->second->snapshotPending.load(std::memory_order_acquire);
}

Json StateSubscriptions::snapshot() const { return cache_.snapshot(); }

state::Cache &StateSubscriptions::cache() { return cache_; }

const state::Cache &StateSubscriptions::cache() const { return cache_; }

void StateSubscriptions::receive(Group &group, unsigned long long generation,
                                 const Json &response) {
    if (group.generation != generation)
        return;
    if (response["returnValue"].isBoolean() && !response["returnValue"].boolean()) {
        refused(group, response);
        return;
    }
    if (invalidate_ && (group.name == "picture" || group.name == "power" ||
                        group.name == "application" || group.name == "audio")) {
        try {
            invalidate_(group.name);
        } catch (...) {
            if (logger_)
                logger_->warning("state: cache invalidation callback failed for " + group.name);
        }
    }

    const Json values = state::mapSubscription(group.name, response, mapping_);
    if (!values.isObject())
        return;
    if (group.name == "media") {
        const Json encoded = values["snapshot"];
        const Json media = encoded.isString() ? Json::parse(encoded.string()) : Json();
        const Json mediaState = media["state"];
        if (mediaState.isString() && mediaState.string() != "idle") {
            std::string error;
            if (!markMediaSeen(mediaSeenPath_, &error) && logger_)
                logger_->warning("state: persist media_seen: " + error);
        }
    }
    const bool snapshotValue = group.snapshotPending.load(std::memory_order_acquire);
    const std::int64_t sourceTime = wallTimeMilliseconds();
    for (const std::string &key : values.keys()) {
        const Json value = values[key];
        if (!value.valid())
            continue;
        state::Event event{group.name, key, value, sourceTime, snapshotValue, true};
        cache_.update(event);
    }
    group.snapshotPending.store(false, std::memory_order_release);
    setLive(group, true);
}

void StateSubscriptions::refused(Group &group, const Json &response) {
    const std::string error = response["errorText"].string();
    const bool unknownMethod = containsInsensitive(error, "unknown method");
    if (group.name == "media" && group.uri == ACB_MEDIA &&
        containsInsensitive(error, "service does not exist")) {
        group.snapshotPending.store(true, std::memory_order_release);
        setLive(group, false);
        if (group.subscription)
            group.subscription->stop();
        group.uri = LEGACY_MEDIA;
        const unsigned long long generation = ++group.generation;
        group.subscription = makeSubscription(group, generation);
        if (group.users.load(std::memory_order_acquire) > 0 && group.subscription)
            group.subscription->start();
        return;
    }
    if ((group.name == "surface" || group.name == "focus") && unknownMethod) {
        group.snapshotPending.store(true, std::memory_order_release);
        setLive(group, false);
        if (group.subscription)
            group.subscription->stop();
        return;
    }
    if (group.name != "audio" || group.uri != MASTER_VOLUME ||
        !(unknownMethod || containsInsensitive(error, "unknown service")))
        return;

    group.snapshotPending.store(true, std::memory_order_release);
    setLive(group, false);
    if (group.subscription)
        group.subscription->stop();
    group.uri = LEGACY_VOLUME;
    const unsigned long long generation = ++group.generation;
    group.subscription = makeSubscription(group, generation);
    if (group.users.load(std::memory_order_acquire) > 0 && group.subscription)
        group.subscription->start();
}

void StateSubscriptions::acquire(Group &group) {
    const unsigned previous = group.users.fetch_add(1, std::memory_order_acq_rel);
    if (previous != 0)
        return;
    group.snapshotPending.store(true, std::memory_order_release);
    if (group.subscription)
        group.subscription->start();
}

void StateSubscriptions::release(Group &group) {
    unsigned users = group.users.load(std::memory_order_acquire);
    while (users > 0) {
        if (group.users.compare_exchange_weak(users, users - 1,
                                              std::memory_order_acq_rel)) {
            if (users == 1) {
                group.snapshotPending.store(true, std::memory_order_release);
                setLive(group, false);
                if (group.subscription)
                    group.subscription->stop();
            }
            return;
        }
    }
}

void StateSubscriptions::setLive(Group &group, bool live) {
    const bool previous = group.live.exchange(live, std::memory_order_acq_rel);
    if (previous == live || !statusChange_)
        return;
    statusChange_(group.name, live);
}

} // namespace daemon
} // namespace nativetv
