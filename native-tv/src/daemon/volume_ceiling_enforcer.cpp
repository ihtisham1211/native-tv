#include "daemon/volume_ceiling_enforcer.hpp"
#include "daemon/logger.hpp"
#include "service/control_commands.hpp"
#include "service/volume_ceiling.hpp"
#include <chrono>
#include <fstream>
#include <time.h>

namespace nativetv {
namespace daemon {
namespace {
// A TV clock before this has not been set (screen_time_enforcer.cpp's floor).
const int64_t kClockFloorMs = 1767225600000LL;

int64_t systemWallNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
} // namespace

VolumeCeilingEnforcer::VolumeCeilingEnforcer(
    EventLoop &loop, state::Cache &state, LunaClient &luna, const std::string &procRoot,
    Json settings, bool allowControl, Logger *logger, VolumeCeilingTimings timings,
    CommandPolicyExecutor *commandPolicy)
    : loop_(loop), state_(state), luna_(luna), commandPolicy_(commandPolicy), logger_(logger),
      lifetime_(std::make_shared<int>(0)), procRoot_(procRoot), timings_(timings),
      allowControl_(allowControl) {
    Json document = Json::object();
    document.set("volumeCeiling", settings);
    settings_ = service::volumeCeilingSettings(document);
    const std::weak_ptr<int> lifetime = lifetime_;
    state_.onChange([this, lifetime](const state::Event &event) {
        if (lifetime.expired()) return;
        onStateChange(event);
    });
}

VolumeCeilingEnforcer::~VolumeCeilingEnforcer() {
    stop();
    lifetime_.reset();
}

bool VolumeCeilingEnforcer::start() {
    if (running_)
        return true;
    const Json power = state_.snapshot()["power"];
    wasScreenOn_ = power["systemOn"].boolean() && power["screenOn"].boolean();
    running_ = true;
    checkTimer_ = loop_.scheduleEvery(timings_.check, [this]() { enforce(); });
    if (!checkTimer_) {
        running_ = false;
        if (logger_)
            logger_->error("volumelimit: could not schedule checks");
        return false;
    }
    // On a TV that starts the server as it comes on, no change of power is seen.
    if (wasScreenOn_ && settings_["enabled"].boolean() && bootedJustNow())
        startupSoon(timings_.settle);
    enforce();
    if (settings_["enabled"].boolean() && logger_)
        logger_->info("volumelimit: ceiling on");
    return true;
}

bool VolumeCeilingEnforcer::apply(const Json &settings, std::string *error) {
    if (error)
        error->clear();
    const std::weak_ptr<int> lifetime = lifetime_;
    if (!loop_.post([this, lifetime, settings]() {
            if (!lifetime.expired()) applyOnLoop(settings);
        })) {
        if (error)
            *error = "native volume ceiling loop is stopping or full";
        return false;
    }
    return true;
}

void VolumeCeilingEnforcer::applyOnLoop(const Json &settings) {
    Json document = Json::object();
    document.set("volumeCeiling", settings);
    settings_ = service::volumeCeilingSettings(document);
    if (logger_)
        logger_->info(settings_["enabled"].boolean() ? "volumelimit: ceiling on"
                                                      : "volumelimit: ceiling off");
    refusedUntil_ = 0;
    enforce();
}

void VolumeCeilingEnforcer::stop() {
    running_ = false;
    if (checkTimer_) loop_.cancelTimer(checkTimer_);
    if (startupTimer_) loop_.cancelTimer(startupTimer_);
    checkTimer_ = startupTimer_ = 0;
}

void VolumeCeilingEnforcer::onStateChange(const state::Event &event) {
    if (!running_)
        return;
    if (event.group == "audio" && event.key == "volume")
        enforce();
    else if (event.group == "power")
        powerChanged();
}

void VolumeCeilingEnforcer::powerChanged() {
    const Json power = state_.snapshot()["power"];
    const bool screenOn = power["systemOn"].boolean() && power["screenOn"].boolean();
    const bool cameOn = screenOn && !wasScreenOn_;
    wasScreenOn_ = screenOn;
    if (cameOn && settings_["enabled"].boolean())
        startupSoon(timings_.settle);
}

void VolumeCeilingEnforcer::startupSoon(EventLoop::Milliseconds delay) {
    if (startupTimer_)
        loop_.cancelTimer(startupTimer_);
    const std::weak_ptr<int> lifetime = lifetime_;
    startupTimer_ = loop_.scheduleAfter(delay, [this, lifetime]() {
        if (lifetime.expired()) return;
        startupTimer_ = 0;
        if (setting_) {
            // A correction is on its way; the start-up volume follows it.
            startupSoon(EventLoop::Milliseconds(500));
            return;
        }
        const int level = service::volumeCeilingStartup(settings_, localClockMs());
        if (level >= 0)
            setVolume(level, "start-up volume");
    });
}

void VolumeCeilingEnforcer::enforce() {
    if (!running_ || !settings_["enabled"].boolean())
        return;
    if (setting_) {
        recheck_ = true;
        return;
    }
    if (steadyNow() < refusedUntil_)
        return;
    const Json volume = state_.snapshot()["audio"]["volume"];
    if (!volume.isNumber())
        return;
    const int limit = service::volumeCeilingLimit(settings_, localClockMs());
    if (volume.number() > limit)
        setVolume(limit, "held to the ceiling");
}

void VolumeCeilingEnforcer::setVolume(int level, const std::string &why) {
    if (!allowControl_) {
        if (!toldControlsOff_ && logger_)
            logger_->warning("volumelimit: controls are off in the config, so the volume is not set");
        toldControlsOff_ = true;
        return;
    }
    setting_ = true;
    recheck_ = false;
    Json payload = Json::object();
    payload.set("volume", Json::numeric(level));
    const std::weak_ptr<int> lifetime = lifetime_;
    if (command("volumelimit.set", "com.webos.audio/setVolume", payload, "volume-ceiling",
                [this, lifetime, level, why](const LunaReply &reply) {
                    if (lifetime.expired()) return;
                    setting_ = false;
                    const bool ok = reply.error.empty() && reply.response["returnValue"].boolean();
                    if (logger_)
                        logger_->write(ok ? LogLevel::Info : LogLevel::Warning,
                                       "volumelimit: " + why + ", " + std::to_string(level) +
                                           (ok ? " set" : " not set"));
                    if (!ok && service::volumeScenarioUnsupported(reply.response))
                        // An external sound device takes no set level, and asking again changes nothing.
                        refusedUntil_ = steadyNow() +
                                        static_cast<double>(timings_.refused.count());
                    else if (ok && recheck_)
                        enforce();
                }))
        return;
    setting_ = false;
    if (logger_)
        logger_->warning("volumelimit: " + why + " was not sent");
}

int64_t VolumeCeilingEnforcer::localClockMs() const {
    const int64_t nowMs = timings_.wallNowMs ? timings_.wallNowMs() : systemWallNowMs();
    if (nowMs < kClockFloorMs)
        return -1;
    const time_t seconds = static_cast<time_t>(nowMs / 1000);
    struct tm local;
    if (!localtime_r(&seconds, &local))
        return -1;
    return ((static_cast<int64_t>(local.tm_hour) * 60 + local.tm_min) * 60 + local.tm_sec) * 1000 +
           nowMs % 1000;
}

double VolumeCeilingEnforcer::steadyNow() const {
    if (timings_.steadyNowMs)
        return timings_.steadyNowMs();
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool VolumeCeilingEnforcer::bootedJustNow() const {
    double uptime = -1;
    if (timings_.uptimeSeconds) {
        uptime = timings_.uptimeSeconds();
    } else {
        std::ifstream file(procRoot_ + "/uptime");
        if (!(file >> uptime))
            uptime = -1;
    }
    return uptime >= 0 && uptime < timings_.bootSeconds;
}

bool VolumeCeilingEnforcer::command(const std::string &action, const std::string &uri,
                                    const Json &payload, const std::string &resource,
                                    LunaClient::Callback completion) {
    if (!commandPolicy_)
        return luna_.call(uri, payload, std::move(completion));
    CommandPolicySpec spec;
    spec.action = action;
    spec.uri = uri;
    spec.payload = payload;
    spec.resource = resource;
    CommandPolicyContext context;
    context.allowControl = allowControl_;
    context.allowPower = false;
    return commandPolicy_->submit(spec, context, false, std::move(completion)).kind ==
           CommandDecisionKind::Queued;
}

} // namespace daemon
} // namespace nativetv
