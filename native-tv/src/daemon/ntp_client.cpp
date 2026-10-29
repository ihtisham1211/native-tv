#include "daemon/ntp_client.hpp"
#include "daemon/logger.hpp"
#include "daemon/luna.hpp"
#include "domain/names.hpp"
#include <cmath>
#include <exception>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <utility>

namespace nativetv {
namespace daemon {
namespace {
double monotonicMilliseconds() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
} // namespace

NtpClient::NtpClient(EventLoop &loop, LunaClient &luna, Logger *logger)
    : loop_(loop), luna_(luna), logger_(logger) {}

bool NtpClient::start(const Json &config) {
    stop();
    return startConfigured(config);
}

bool NtpClient::watchConfig(const std::string &path, const Json &config) {
    stop();
    if (path.empty()) {
        if (logger_)
            logger_->warning("ntp: config watcher needs the active config path");
        return false;
    }
    configPath_ = path;
    const Json configuredNtp = config["ntp"];
    ntpConfigSnapshot_ = configuredNtp.valid() ? configuredNtp.dump() : "null";
    (void)startConfigured(config);
    configTimer_ = loop_.scheduleEvery(std::chrono::seconds(1), [this] { checkConfigFile(); });
    if (!configTimer_ && logger_)
        logger_->warning("ntp: could not schedule config updates");
    return configTimer_ != 0;
}

void NtpClient::setStatusChanged(std::function<void(const Json &)> callback) {
    statusChanged_ = std::move(callback);
}

bool NtpClient::startConfigured(const Json &config) {
    stopClient();
    service::NtpSettings settings;
    if (!service::readNtpSettings(config, settings)) {
        publishStatus();
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        settings_ = settings;
        running_ = true;
        powerStateKnown_ = false;
        powerWasOn_ = false;
        generation_++;
    }
    if (logger_) {
        logger_->info("ntp: setting the clock from " + settings.server +
                      (settings.port != 123 ? ":" + std::to_string(settings.port) : ""));
    }
    Json powerRequest = Json::object();
    powerRequest.set("subscribe", Json::flag(true));
    LunaSubscriptionHandlers handlers;
    handlers.message = [this](const Json &response) {
        Json rawState = response["state"];
        if (!rawState.isString() || rawState.string().empty())
            rawState = response["processing"];
        if (!rawState.isString() || rawState.string().empty())
            return;
        const bool isOn = names::powerState(rawState)["systemOn"].boolean();
        bool transitionedOn = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_)
                return;
            transitionedOn = isOn && powerStateKnown_ && !powerWasOn_;
            powerStateKnown_ = true;
            powerWasOn_ = isOn;
        }
        if (transitionedOn)
            sync();
    };
    std::shared_ptr<LunaSubscription> subscription = luna_.subscribe(
        "com.webos.service.tvpower/power/getPowerState", powerRequest, "", handlers);
    if (subscription && subscription->start()) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_)
            powerSubscription_ = subscription;
        else
            subscription->stop();
    }
    syncOnLoop();
    publishStatus();
    return true;
}

void NtpClient::sync() {
    const bool posted = loop_.post([this] { syncOnLoop(); });
    if (!posted && logger_)
        logger_->warning("ntp: sync request was dropped because the event loop is stopping");
}

void NtpClient::stop() {
    EventLoop::TimerId configTimer = configTimer_;
    configTimer_ = 0;
    if (configTimer)
        loop_.cancelTimer(configTimer);
    configPath_.clear();
    ntpConfigSnapshot_.clear();
    stopClient();
    publishStatus();
}

void NtpClient::stopClient() {
    EventLoop::TimerId timer = 0;
    std::shared_ptr<LunaSubscription> subscription;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++generation_;
        running_ = false;
        busy_ = false;
        appliedOnce_ = false;
        hasLastAnswer_ = false;
        hasLastSet_ = false;
        powerStateKnown_ = false;
        powerWasOn_ = false;
        lastSetMonoMs_ = 0;
        sets_ = 0;
        errors_ = 0;
        error_.clear();
        settings_ = service::NtpSettings();
        lastAnswer_ = service::NtpAnswer();
        timer = timer_;
        timer_ = 0;
        subscription.swap(powerSubscription_);
    }
    if (timer)
        loop_.cancelTimer(timer);
    if (subscription)
        subscription->stop();
}

void NtpClient::checkConfigFile() {
    if (configPath_.empty())
        return;
    std::ifstream input(configPath_.c_str(), std::ios::in | std::ios::binary);
    if (!input)
        return;
    input.seekg(0, std::ios::end);
    const std::streamoff length = input.tellg();
    if (length <= 0 || length > 1024 * 1024)
        return;
    input.seekg(0, std::ios::beg);
    std::string bytes(static_cast<size_t>(length), '\0');
    input.read(&bytes[0], static_cast<std::streamsize>(length));
    if (!input)
        return;
    const Json config = Json::parse(bytes);
    if (!config.isObject())
        return;
    const Json configuredNtp = config["ntp"];
    const std::string snapshot = configuredNtp.valid() ? configuredNtp.dump() : "null";
    if (snapshot == ntpConfigSnapshot_)
        return;
    ntpConfigSnapshot_ = snapshot;
    if (logger_)
        logger_->info("ntp: settings changed; applying saved config");
    (void)startConfigured(config);
}

void NtpClient::publishStatus() const {
    if (statusChanged_)
        statusChanged_(status());
}

Json NtpClient::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_)
        return Json();
    Json result = Json::object();
    result.set("sets", Json::numeric(sets_));
    result.set("errors", Json::numeric(errors_));
    result.set("server", Json::text(settings_.server));
    result.set("port", Json::numeric(settings_.port));
    result.set("error", error_.empty() ? Json() : Json::text(error_));
    if (hasLastAnswer_) {
        Json last = Json::object();
        last.set("at", Json::numeric(lastAnswer_.atMs));
        last.set("atMono", Json::numeric(lastAnswer_.atMonoMs));
        last.set("offsetMs", Json::numeric(lastAnswer_.offsetMs));
        last.set("roundTripMs", Json::numeric(lastAnswer_.roundTripMs));
        last.set("stratum", Json::numeric(lastAnswer_.stratum));
        result.set("last", last);
        result.set("answeredAgoS", Json::numeric(
            std::round((monotonicMilliseconds() - lastAnswer_.atMonoMs) / 1000.0)));
    } else {
        result.set("last", Json());
        result.set("answeredAgoS", Json());
    }
    result.set("setAgoS", hasLastSet_
                               ? Json::numeric(std::round(
                                     (monotonicMilliseconds() - lastSetMonoMs_) / 1000.0))
                               : Json());
    result.set("setAtMonoMs", hasLastSet_ ? Json::numeric(lastSetMonoMs_) : Json());
    return result;
}

void NtpClient::syncOnLoop() {
    service::NtpSettings settings;
    unsigned long long generation = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ || busy_)
            return;
        busy_ = true;
        settings = settings_;
        generation = generation_;
    }
    const std::shared_ptr<service::NtpResult> result =
        std::make_shared<service::NtpResult>();
    const bool queued = loop_.submitBlocking(
        [settings, result] { *result = service::queryNtp(settings); },
        [this, generation, result](std::exception_ptr failure) {
            if (failure) {
                std::string message = "NTP query failed";
                try {
                    std::rethrow_exception(failure);
                } catch (const std::exception &error) {
                    message += ": ";
                    message += error.what();
                } catch (...) {
                }
                syncFailed(generation, message);
                return;
            }
            completeQuery(generation, *result);
        });
    if (!queued)
        syncFailed(generation, "could not queue an NTP query");
}

void NtpClient::completeQuery(unsigned long long generation,
                              const service::NtpResult &result) {
    if (!result.ok) {
        syncFailed(generation, result.error.empty() ? "NTP query failed" : result.error);
        return;
    }
    bool shouldSet = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ || generation != generation_)
            return;
        lastAnswer_ = result.answer;
        hasLastAnswer_ = true;
        shouldSet = !appliedOnce_ || std::abs(result.answer.offsetMs) >= 1000.0;
        if (!shouldSet)
            busy_ = false;
    }
    publishStatus();
    if (shouldSet) {
        setClock(generation, result.answer);
        return;
    }
    setError(generation, "");
    schedule(std::chrono::hours(1));
    publishStatus();
}

void NtpClient::setClock(unsigned long long generation,
                         const service::NtpAnswer &answer) {
    const Json payload = service::ntpSetTimePayload(answer);
    const bool queued = luna_.call(
        "com.webos.service.systemservice/clock/setTime", payload,
        [this, generation, answer](const LunaReply &reply) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!running_ || generation != generation_)
                    return;
            }
            if (!reply.error.empty() || !reply.response.isObject()) {
                syncFailed(generation, "the time service did not answer");
                return;
            }
            const Json returned = reply.response["returnValue"];
            if (!returned.isBoolean() || !returned.boolean()) {
                syncFailed(generation, "the time service refused the time: " +
                                            reply.response.dump());
                return;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!running_ || generation != generation_)
                    return;
                busy_ = false;
                appliedOnce_ = true;
                ++sets_;
                lastSetMonoMs_ = monotonicMilliseconds();
                hasLastSet_ = true;
            }
            setError(generation, "");
            if (logger_) {
                std::ostringstream offset;
                offset << (answer.offsetMs >= 0 ? "+" : "") << std::fixed
                       << std::setprecision(3) << answer.offsetMs / 1000.0;
                service::NtpSettings settings;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    settings = settings_;
                }
                logger_->info("ntp: set the clock from " + settings.server +
                              " (stratum " + std::to_string(answer.stratum) +
                              "), " + offset.str() + " s");
            }
            schedule(std::chrono::hours(1));
            publishStatus();
        });
    if (!queued)
        syncFailed(generation, "could not queue a clock update");
}

void NtpClient::syncFailed(unsigned long long generation, const std::string &message) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ || generation != generation_)
            return;
        busy_ = false;
        ++errors_;
    }
    setError(generation, message);
    schedule(std::chrono::seconds(30));
    publishStatus();
}

void NtpClient::setError(unsigned long long generation, const std::string &message) {
    bool changed = false;
    bool cleared = false;
    std::string server;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ || generation != generation_ || error_ == message)
            return;
        changed = true;
        cleared = !message.empty() ? false : !error_.empty();
        error_ = message;
        server = settings_.server;
    }
    if (!changed || !logger_)
        return;
    if (!message.empty())
        logger_->error("ntp: " + message);
    else if (cleared)
        logger_->info("ntp: syncing with " + server + " again");
}

void NtpClient::schedule(std::chrono::milliseconds delay) {
    EventLoop::TimerId previous = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_)
            return;
        previous = timer_;
        timer_ = 0;
    }
    if (previous)
        loop_.cancelTimer(previous);
    const EventLoop::TimerId timer = loop_.scheduleAfter(delay, [this] {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            timer_ = 0;
        }
        syncOnLoop();
    });
    if (timer) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_)
            timer_ = timer;
        else
            loop_.cancelTimer(timer);
    }
}

} // namespace daemon
} // namespace nativetv
