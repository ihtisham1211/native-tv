#include "daemon/adblock_manager.hpp"
#include "daemon/logger.hpp"
#include "daemon/luna.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <utility>

namespace nativetv {
namespace daemon {
namespace {
const EventLoop::Milliseconds kSdpPollInterval(3000);
const EventLoop::Milliseconds kSdpSourceWait(120000);
const EventLoop::Milliseconds kSdpSlowPollInterval(30000);

bool timeIsValid(const Json &response) {
    if (!response.isObject() ||
        (response["returnValue"].isBoolean() && !response["returnValue"].boolean()))
        return false;
    if (response.has("timeValid") || response.has("timevalid")) {
        const Json valid = response.has("timeValid") ? response["timeValid"]
                                                       : response["timevalid"];
        return (valid.isBoolean() && valid.boolean()) ||
               (valid.isString() && valid.string() == "true");
    }
    const Json utc = response["utc"];
    if (!utc.isNumber() && !utc.isString())
        return false;
    char *end = nullptr;
    const std::string value = utc.isString() ? utc.string() : utc.dump();
    const double seconds = std::strtod(value.c_str(), &end);
    return end && end != value.c_str() && *end == '\0' && seconds > 1577836800.0;
}

bool javascriptTruthy(const Json &value) {
    if (!value.valid())
        return false;
    if (value.isBoolean())
        return value.boolean();
    if (value.isNumber())
        return value.number() != 0.0;
    if (value.isString())
        return !value.string().empty();
    return true;
}

bool timeIsSdpSource(const Json &response) {
    const char *keys[] = {"systemTimeSource", "source", "timeSource"};
    for (const char *key : keys) {
        const Json value = response[key];
        if (!javascriptTruthy(value))
            continue;
        if (!value.isString())
            return false;
        std::string source = value.string();
        std::transform(source.begin(), source.end(), source.begin(),
                       [](unsigned char c) {
                           return static_cast<char>(std::tolower(c));
                       });
        return source == "sdp";
    }
    return false;
}
} // namespace

AdBlockManager::AdBlockManager(EventLoop &loop, LunaClient &luna,
                               std::string dataRoot, std::string systemRoot,
                               Logger *logger,
                               service::AdBlockMountOperations mountOperations)
    : loop_(loop), luna_(luna), logger_(logger),
      store_(std::move(dataRoot), std::move(systemRoot), std::move(mountOperations)) {}

bool AdBlockManager::start(std::string *error) {
    stopped_.store(false);
    sdpGraceActive_.store(true);
    if (!store_.restoreAtBoot(true, error))
        return false;
    learnCountry();
    if (store_.waitsForSdp()) {
        if (loop_.isLoopThread()) {
            reconcileSdpGraceOnLoop();
        } else if (!loop_.post([this]() { reconcileSdpGraceOnLoop(); })) {
            if (error)
                *error = "could not schedule the ad-block clock check";
            return false;
        }
    }
    if (loop_.isLoopThread()) {
        watchNetworkOnLoop();
    } else {
        (void)loop_.post([this]() { watchNetworkOnLoop(); });
    }
    return true;
}

// The TV can move to another network, or be given another address on its own,
// and the table's local lookups name the addresses of the network it was
// written on: checked every 10 minutes, and the table rewritten on a change.
void AdBlockManager::watchNetworkOnLoop() {
    if (stopped_.load() || networkTimer_)
        return;
    networkTimer_ = loop_.scheduleEvery(std::chrono::minutes(10), [this]() {
        if (stopped_.load())
            return;
        std::string error;
        if (!store_.refreshForNetworkChange(sdpGraceActive_.load(), &error) && logger_)
            logger_->warning("adblock: could not refresh the hosts table for the new network" +
                             (error.empty() ? std::string() : ": " + error));
    });
}

void AdBlockManager::stop() {
    stopped_.store(true);
    if (!loop_.isLoopThread()) {
        (void)loop_.post([this]() { stop(); });
        return;
    }
    cancelSdpTimers();
    if (networkTimer_)
        (void)loop_.cancelTimer(networkTimer_);
    networkTimer_ = 0;
    sdpMonitoring_ = false;
    sdpCheckPending_ = false;
}

service::AdBlockStore &AdBlockManager::store() { return store_; }

bool AdBlockManager::sdpGraceActive() const { return sdpGraceActive_.load(); }

Json AdBlockManager::setMode(const std::string &mode) {
    Json result = store_.setMode(mode, sdpGraceActive_.load());
    reconcileSdpGrace();
    return result;
}

Json AdBlockManager::setTvUpdatesBlocked(bool blocked) {
    Json result = store_.setTvUpdatesBlocked(blocked, sdpGraceActive_.load());
    reconcileSdpGrace();
    return result;
}

Json AdBlockManager::setLocalLookups(bool on) {
    Json result = store_.setLocalLookups(on, sdpGraceActive_.load());
    reconcileSdpGrace();
    return result;
}

Json AdBlockManager::setCustomHosts(const Json &options) {
    Json result = store_.setCustomHosts(options, sdpGraceActive_.load());
    reconcileSdpGrace();
    return result;
}

void AdBlockManager::learnCountry() {
    Json request = Json::object();
    request.set("category", Json::text("option"));
    Json keys = Json::array();
    keys.add(Json::text("smartServiceCountryCode2"));
    request.set("keys", keys);
    (void)luna_.call("com.webos.settingsservice/getSystemSettings", request,
                     [this](const LunaReply &reply) {
        if (stopped_.load())
            return;
        const std::string country =
            reply.response["settings"]["smartServiceCountryCode2"].string();
        std::string error;
        if (!store_.updateCountry(country, sdpGraceActive_.load(), &error) && logger_)
            logger_->warning("adblock: could not save the TV country" +
                             (error.empty() ? std::string() : ": " + error));
    });
}

void AdBlockManager::reconcileSdpGrace() {
    if (stopped_.load())
        return;
    if (loop_.isLoopThread()) {
        reconcileSdpGraceOnLoop();
        return;
    }
    (void)loop_.post([this]() { reconcileSdpGraceOnLoop(); });
}

void AdBlockManager::reconcileSdpGraceOnLoop() {
    if (stopped_.load())
        return;
    if (!store_.waitsForSdp()) {
        cancelSdpTimers();
        sdpMonitoring_ = false;
        sdpCheckPending_ = false;
        return;
    }
    if (!sdpGraceActive_.load() || sdpMonitoring_)
        return;
    sdpMonitoring_ = true;
    sdpAnySourceAccepted_ = false;
    checkSdpClock(true);
}

void AdBlockManager::checkSdpClock(bool firstCheck) {
    if (stopped_.load() || !sdpGraceActive_.load() || sdpCheckPending_)
        return;
    sdpCheckPending_ = true;
    const bool queued = luna_.call(
        "com.webos.service.systemservice/time/getSystemTime", Json::object(),
        [this, firstCheck](const LunaReply &reply) {
        sdpCheckPending_ = false;
        if (stopped_.load() || !sdpGraceActive_.load())
            return;
        const bool valid = timeIsValid(reply.response);
        const bool fromSdp = timeIsSdpSource(reply.response);
        if (firstCheck && valid) {
            finalizeSdpGrace("system time already valid; skipping SDP grace period");
            return;
        }
        if (valid && fromSdp) {
            finalizeSdpGrace("SDP clock sync complete; nextlgsdp.com blocked");
            return;
        }
        if (valid && sdpAnySourceAccepted_) {
            finalizeSdpGrace("system time valid from another source; nextlgsdp.com blocked");
            return;
        }
        if (firstCheck && !sdpPollTimer_)
            beginSdpPolling();
        });
    if (!queued) {
        sdpCheckPending_ = false;
        if (firstCheck && !sdpPollTimer_)
            beginSdpPolling();
    }
}

void AdBlockManager::beginSdpPolling() {
    sdpCapTimer_ = loop_.scheduleAfter(kSdpSourceWait, [this]() {
        sdpCapTimer_ = 0;
        acceptAnyTimeSource();
    });
    sdpPollTimer_ = loop_.scheduleEvery(kSdpPollInterval, [this]() {
        checkSdpClock(false);
    });
}

void AdBlockManager::acceptAnyTimeSource() {
    if (stopped_.load() || !sdpGraceActive_.load())
        return;
    sdpAnySourceAccepted_ = true;
    if (sdpPollTimer_)
        (void)loop_.cancelTimer(sdpPollTimer_);
    sdpPollTimer_ = loop_.scheduleEvery(kSdpSlowPollInterval, [this]() {
        checkSdpClock(false);
    });
    if (logger_)
        logger_->info("adblock: no SDP clock sync after 120s; waiting for any valid time source");
}

void AdBlockManager::finalizeSdpGrace(const std::string &message) {
    if (!sdpGraceActive_.exchange(false))
        return;
    cancelSdpTimers();
    sdpMonitoring_ = false;
    sdpCheckPending_ = false;
    if (logger_)
        logger_->info("adblock: " + message);
    if (store_.waitsForSdp() && store_.tableMounted()) {
        std::string error;
        if (!store_.refreshMountedTable(false, &error) && logger_)
            logger_->warning("adblock: could not refresh the mounted hosts table" +
                             (error.empty() ? std::string() : ": " + error));
    }
}

void AdBlockManager::cancelSdpTimers() {
    if (sdpPollTimer_)
        (void)loop_.cancelTimer(sdpPollTimer_);
    if (sdpCapTimer_)
        (void)loop_.cancelTimer(sdpCapTimer_);
    sdpPollTimer_ = 0;
    sdpCapTimer_ = 0;
    sdpAnySourceAccepted_ = false;
}

} // namespace daemon
} // namespace nativetv
