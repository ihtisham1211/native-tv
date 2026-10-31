#include "daemon/screen_time_enforcer.hpp"
#include "daemon/luna.hpp"
#include "daemon/logger.hpp"
#include "domain/names.hpp"
#include "service/screen_time.hpp"
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <sstream>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace nativetv {
namespace daemon {
namespace {
const int64_t kScreenTimeClockFloorMs = 1767225600000LL;
const int kWarningSeconds = 300;
const unsigned kKeyBack = 158;
const unsigned kKeyLeftMeta = 125;

int64_t systemWallNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string localDay(const tm &time) {
    char value[16] = {};
    return std::strftime(value, sizeof(value), "%Y-%m-%d", &time) ? value : std::string();
}

std::string minutesText(double seconds) {
    const double rounded = std::max(1.0, std::floor(seconds / 60.0 + 0.5));
    const int minutes = rounded >= static_cast<double>(std::numeric_limits<int>::max())
                            ? std::numeric_limits<int>::max()
                            : static_cast<int>(rounded);
    return std::to_string(minutes) + (minutes == 1 ? " minute" : " minutes");
}

std::string compactState(const std::string &value) {
    std::string result;
    for (unsigned char character : value) {
        if (std::isalnum(character))
            result.push_back(static_cast<char>(std::tolower(character)));
    }
    return result;
}

bool isScreenSaver(const Json &snapshot) {
    return compactState(snapshot["power"]["state"].string()) == "screensaver";
}

std::string rootPath(const std::string &root, const std::string &path) {
    if (root.empty() || root == "/")
        return path;
    return root[root.size() - 1] == '/' ? root + path.substr(1) : root + path;
}

} // namespace

ScreenTimeEnforcer::ScreenTimeEnforcer(
    EventLoop &loop, state::Cache &state, service::HistoryRecorder &history,
    LunaClient &luna, const std::string &dataRoot, const std::string &procRoot,
    const std::string &systemRoot, Json settings, bool allowControl, bool allowPower,
    Logger *logger,
    ScreenTimeTimings timings, CommandPolicyExecutor *commandPolicy)
    : loop_(loop), state_(state), history_(history), luna_(luna),
      commandPolicy_(commandPolicy), logger_(logger),
      lifetime_(std::make_shared<int>(0)),
      procRoot_(procRoot), systemRoot_(systemRoot),
      switchedOffFile_(dataRoot + "/screentime.json", 64 * 1024), timings_(timings),
      allowControl_(allowControl), allowPower_(allowPower), hasHomeApp_(::access(
          rootPath(systemRoot_, "/usr/palm/applications/com.webos.app.home").c_str(), F_OK) == 0) {
    Json document = Json::object();
    document.set("screenTime", settings);
    settings_ = service::screenTimeSettings(document, allowPower_, hasHomeApp_);

    const JsonFileResult saved = switchedOffFile_.load();
    if (!saved.error.empty() && logger_)
        logger_->warning("screentime: could not read switched-off state: " + saved.error);
    const Json switched = saved.value["switchedOff"];
    for (const char *id : {"daily", "bedtime"})
        if (switched[id].isBoolean() && switched[id].boolean())
            switchedOff_[id] = true;

    const std::weak_ptr<int> lifetime = lifetime_;
    state_.onChange([this, lifetime](const state::Event &event) {
        if (lifetime.expired()) return;
        onStateChange(event);
    });
}

ScreenTimeEnforcer::~ScreenTimeEnforcer() {
    stop();
    lifetime_.reset();
}

bool ScreenTimeEnforcer::start() {
    if (running_)
        return true;
    const Json power = state_.snapshot()["power"];
    wasScreenOn_ = power["systemOn"].boolean() && power["screenOn"].boolean();
    return startChecks();
}

bool ScreenTimeEnforcer::apply(const Json &settings, std::string *error) {
    if (error)
        error->clear();
    Json validated;
    std::string validationError;
    if (!service::validateScreenTimePatch(settings, validated, validationError)) {
        if (error)
            *error = validationError;
        return false;
    }
    const std::weak_ptr<int> lifetime = lifetime_;
    if (!loop_.post([this, lifetime, validated]() {
            if (!lifetime.expired()) applyOnLoop(validated);
        })) {
        if (error)
            *error = "native screen-time loop is stopping or full";
        return false;
    }
    return true;
}

void ScreenTimeEnforcer::applyOnLoop(const Json &settings) {
    Json document = Json::object();
    document.set("screenTime", settings);
    settings_ = service::screenTimeSettings(document, allowPower_, hasHomeApp_);
    if (checkTimer_)
        loop_.cancelTimer(checkTimer_);
    checkTimer_ = 0;
    running_ = false;
    cancelPending();
    if (settleTimer_) loop_.cancelTimer(settleTimer_);
    if (verifyTimer_) loop_.cancelTimer(verifyTimer_);
    if (screenSaverTimer_) loop_.cancelTimer(screenSaverTimer_);
    if (powerTimer_) loop_.cancelTimer(powerTimer_);
    settleTimer_ = verifyTimer_ = 0;
    screenSaverTimer_ = powerTimer_ = 0;
    settling_ = false;
    acting_ = false;
    warned_.clear();
    warnedDay_.clear();
    clearSwitchedOff();
    if (idleTimer_) loop_.cancelTimer(idleTimer_);
    idleTimer_ = 0;
    if (!startChecks() && logger_)
        logger_->error("screentime: could not restart policy checks");
}

void ScreenTimeEnforcer::stop() {
    running_ = false;
    if (idleTimer_) loop_.cancelTimer(idleTimer_);
    idleTimer_ = 0;
    if (checkTimer_) loop_.cancelTimer(checkTimer_);
    checkTimer_ = 0;
    cancelPending();
    if (settleTimer_) loop_.cancelTimer(settleTimer_);
    if (verifyTimer_) loop_.cancelTimer(verifyTimer_);
    if (screenSaverTimer_) loop_.cancelTimer(screenSaverTimer_);
    if (powerTimer_) loop_.cancelTimer(powerTimer_);
    settleTimer_ = verifyTimer_ = 0;
    screenSaverTimer_ = powerTimer_ = 0;
    if (keyTimer_) loop_.cancelTimer(keyTimer_);
    keyTimer_ = 0;
    if (keyFd_ >= 0) {
        (void)writeKeyEvent(keyCode_, 0);
        close(keyFd_);
        keyFd_ = -1;
    }
    keyCallback_ = std::function<void(bool)>();
    acting_ = false;
}

bool ScreenTimeEnforcer::startChecks() {
    if (!settings_["enabled"].boolean() && !idleMinutes())
        return true;
    running_ = true;
    lastActivityMs_ = idleNow();
    if (!listening_) {
        listening_ = true;
        const Json power = state_.snapshot()["power"];
        wasScreenOn_ = power["systemOn"].boolean() && power["screenOn"].boolean();
    }
    checkTimer_ = loop_.scheduleEvery(timings_.check, [this]() { check(); });
    if (!checkTimer_) {
        running_ = false;
        if (logger_)
            logger_->error("screentime: could not schedule policy checks");
        return false;
    }
    check();
    if (logger_)
        logger_->info("screentime: limits on");
    return true;
}

void ScreenTimeEnforcer::onStateChange(const state::Event &event) {
    if (!running_)
        return;
    // What a person does shows in the state: another app or input, the volume, the TV waking up.
    if (!event.snapshot &&
        (event.group == "application" || event.group == "audio" ||
         (event.group == "power" && compactState(event.value.string()) == "active")))
        activityOnLoop();
    if (event.group == "power")
        powerChanged();
    else if (event.group == "application" && event.key == "app")
        check();
}

void ScreenTimeEnforcer::powerChanged() {
    const Json power = state_.snapshot()["power"];
    const bool screenOn = power["systemOn"].boolean() && power["screenOn"].boolean();
    const bool cameOn = screenOn && !wasScreenOn_;
    wasScreenOn_ = screenOn;
    if (!cameOn || acting_) {
        check();
        return;
    }
    if (settling_)
        return;
    settling_ = true;
    settleTimer_ = loop_.scheduleAfter(timings_.settle, [this]() {
        settleTimer_ = 0;
        settling_ = false;
        check();
    });
    if (!settleTimer_) {
        settling_ = false;
        if (logger_)
            logger_->error("screentime: could not schedule screen-on settling");
        check();
    }
}

ScreenTimeEnforcer::Assessment ScreenTimeEnforcer::assess() {
    Assessment result;
    if (!running_ || !settings_["enabled"].boolean())
        return result;
    const Json historyStatus = history_.status();
    if (!historyStatus["running"].boolean())
        return result;

    const int64_t nowMs = timings_.wallNowMs ? timings_.wallNowMs() : systemWallNowMs();
    if (nowMs < kScreenTimeClockFloorMs)
        return result;
    const time_t nowSeconds = static_cast<time_t>(nowMs / 1000);
    struct tm local;
    if (!localtime_r(&nowSeconds, &local))
        return result;
    result.day = localDay(local);
    if (result.day.empty())
        return result;
    const int64_t milliseconds = nowMs % 1000;
    result.localClockMs = ((static_cast<int64_t>(local.tm_hour) * 60 + local.tm_min) * 60 +
                           local.tm_sec) * 1000 + milliseconds;
    local.tm_hour = 0;
    local.tm_min = 0;
    local.tm_sec = 0;
    local.tm_isdst = -1;
    const time_t midnight = std::mktime(&local);
    if (midnight == static_cast<time_t>(-1))
        return result;
    const int64_t todayFromMs = static_cast<int64_t>(midnight) * 1000;

    const Json current = history_.overlay(nowMs - 1000, nowMs)["current"];
    result.key = current.string();
    if (result.key.empty())
        return result;
    result.today = history_.totals(todayFromMs, nowMs);
    result.screenOn = result.key != "screen_off";
    result.tv = service::screenTimeTvDue(settings_, result.localClockMs, result.today);
    if (result.screenOn)
        result.app = service::screenTimeAppDue(settings_, result.key,
                                               result.today[result.key].number());
    result.valid = true;

    if (result.day != warnedDay_) {
        warned_.clear();
        warnedDay_ = result.day;
    }
    return result;
}

void ScreenTimeEnforcer::warnSoon(const Assessment &assessment) {
    if (!assessment.screenOn)
        return;
    double tvLeft = std::numeric_limits<double>::infinity();
    std::string tvWhy;
    const Json daily = settings_["dailyMinutes"];
    if (daily.isNumber() && daily.number() > 0) {
        double watched = 0;
        for (const std::string &key : assessment.today.keys()) {
            if (key == "screen_off" || key == "screen_saver")
                continue;
            const Json seconds = assessment.today[key];
            if (seconds.isNumber() && std::isfinite(seconds.number()))
                watched += seconds.number();
        }
        tvLeft = daily.number() * 60.0 - watched;
        tvWhy = "daily";
    }
    const service::ScreenTimeBedtime bed =
        service::screenTimeBedtime(settings_, assessment.localClockMs);
    if (bed.configured && !bed.inside &&
        static_cast<double>(bed.untilStartSeconds) < tvLeft) {
        tvLeft = static_cast<double>(bed.untilStartSeconds);
        tvWhy = "bedtime";
    }
    if (!tvWhy.empty() && tvLeft > 0 && tvLeft <= kWarningSeconds) {
        const std::string message = tvWhy == "bedtime"
                                        ? "Bedtime in " + minutesText(tvLeft) + "."
                                        : minutesText(tvLeft) + " of screen time left today.";
        warnOnce("soon:" + tvWhy, message);
    }

    const Json limit = settings_["apps"][assessment.key];
    if (limit.isNumber() && limit.number() > 0) {
        const double left = limit.number() * 60.0 - assessment.today[assessment.key].number();
        if (left > 0 && left <= kWarningSeconds)
            warnOnce("soon:app:" + assessment.key,
                     minutesText(left) + " left for this today.");
    }
}

void ScreenTimeEnforcer::warnOnce(const std::string &id, const std::string &message) {
    if (warned_[id])
        return;
    warned_[id] = true;
    toast(message);
}

bool ScreenTimeEnforcer::command(const std::string &action, const std::string &uri,
                                 const Json &payload, const std::string &resource,
                                 bool requiresPower, LunaClient::Callback completion,
                                 const std::string &appId, bool urgent) {
    if (!commandPolicy_)
        return luna_.call(uri, payload, std::move(completion), appId, urgent);
    CommandPolicySpec spec;
    spec.action = action;
    spec.uri = uri;
    spec.payload = payload;
    spec.appId = appId;
    spec.resource = resource;
    spec.requiresPower = requiresPower;
    spec.urgent = urgent;
    CommandPolicyContext context;
    context.allowControl = allowControl_;
    context.allowPower = allowPower_;
    return commandPolicy_->submit(spec, context, false, std::move(completion)).kind ==
           CommandDecisionKind::Queued;
}

void ScreenTimeEnforcer::toast(const std::string &message) {
    if (!allowControl_) {
        logControl("toast", false);
        return;
    }
    Json request = Json::object();
    request.set("sourceId", Json::text("com.webos.app.home"));
    request.set("message", Json::text(message.substr(0, 120)));
    const std::weak_ptr<int> lifetime = lifetime_;
    if (!command("screentime.toast", "com.webos.notification/createToast", request,
                    "notifications", false,
                    [this, lifetime](const LunaReply &reply) {
                        if (lifetime.expired()) return;
                        if (!reply.error.empty() || !reply.response["returnValue"].boolean())
                            logControl("toast", false);
                    }, "com.webos.app.home"))
        logControl("toast", false);
}

void ScreenTimeEnforcer::check() {
    if (!running_ || settling_ || acting_)
        return;
    checkIdle();
    Assessment assessment = assess();
    if (!assessment.valid) {
        cancelPending();
        return;
    }

    bool cleared = false;
    for (auto item = switchedOff_.begin(); item != switchedOff_.end();) {
        if (!assessment.tv.due || assessment.tv.id != item->first) {
            item = switchedOff_.erase(item);
            cleared = true;
        } else {
            ++item;
        }
    }
    if (cleared)
        saveSwitchedOff();

    if (assessment.tv.due && assessment.tv.action == "off" &&
        switchedOff_.find(assessment.tv.id) != switchedOff_.end()) {
        afterSwitchedOff(assessment.tv, assessment.screenOn);
        return;
    }

    warnSoon(assessment);
    if (!assessment.screenOn) {
        cancelPending();
        return;
    }
    const service::ScreenTimeDue &due =
        assessment.tv.due ? assessment.tv : assessment.app;
    if (!due.due) {
        cancelPending();
        return;
    }
    if (due.action == "warn") {
        cancelPending();
        warnOnce(due.id, due.text);
        return;
    }
    if (pendingId_ == due.id && !pendingAgain_)
        return;

    cancelPending();
    pendingId_ = due.id;
    const std::string suffix = due.action == "off" ? "The TV switches off in a minute."
        : due.action == "screenOff" ? "The screen goes off in a minute."
                                     : "Going to Home in a minute.";
    toast(due.text + " " + suffix);
    const std::string id = due.id;
    pendingTimer_ = loop_.scheduleAfter(timings_.grace, [this, id]() {
        pendingTimer_ = 0;
        pendingId_.clear();
        Assessment current = assess();
        const service::ScreenTimeDue &still =
            current.tv.due ? current.tv : current.app;
        if (current.valid && current.screenOn && still.due && still.id == id &&
            still.action != "warn")
            run(still.action, id);
    });
    if (!pendingTimer_) {
        if (logger_)
            logger_->error("screentime: could not schedule the grace period");
        cancelPending();
    }
}

void ScreenTimeEnforcer::run(const std::string &action, const std::string &id) {
    if (logger_)
        logger_->info("screentime: " + (id.empty() ? std::string("limit") : id) +
                      ", " + action);
    if (action == "off") {
        if ((id == "daily" || id == "bedtime") && !switchedOff_[id]) {
            switchedOff_[id] = true;
            saveSwitchedOff();
        }
        powerOff();
    } else if (action == "screenOff") {
        turnScreenOff();
    } else if (action == "home") {
        launchHome();
    }
}

double ScreenTimeEnforcer::idleNow() const {
    if (timings_.steadyNowMs)
        return timings_.steadyNowMs();
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

int ScreenTimeEnforcer::idleMinutes() const {
    const Json minutes = settings_["idleMinutes"];
    return minutes.isNumber() ? static_cast<int>(minutes.number()) : 0;
}

void ScreenTimeEnforcer::noteActivity() {
    const std::weak_ptr<int> lifetime = lifetime_;
    (void)loop_.post([this, lifetime]() {
        if (!lifetime.expired()) activityOnLoop();
    });
}

void ScreenTimeEnforcer::activityOnLoop() {
    lastActivityMs_ = idleNow();
    cancelIdle();
}

void ScreenTimeEnforcer::cancelIdle() {
    if (idleTimer_) loop_.cancelTimer(idleTimer_);
    idleTimer_ = 0;
}

// The idle power-off: the TV is on, nobody has pressed a key or changed an app, input or the volume, and
// nothing is playing. An HDMI input is never counted idle (the TV cannot see what a console or box is
// doing there). When the time is up a toast says so, and a minute later, if nothing has happened, the TV
// is switched off.
void ScreenTimeEnforcer::checkIdle() {
    const int minutes = idleMinutes();
    const Json snapshot = state_.snapshot();
    const double now = idleNow();
    const bool on = snapshot["power"]["systemOn"].boolean();
    const std::string app = snapshot["application"]["app"].string();
    // The media state is kept as one scalar of JSON text (see state/mediaValue).
    const std::string mediaText = snapshot["media"]["snapshot"].string();
    const bool playing = !mediaText.empty() && Json::parse(mediaText)["state"].string() == "playing";
    const bool busy = app.compare(0, 4, "hdmi") == 0 || playing;
    if (!minutes || !on || !allowControl_ || !allowPower_ || busy) {
        lastActivityMs_ = now;
        cancelIdle();
        return;
    }
    if (idleTimer_ || now - lastActivityMs_ < minutes * 60000.0)
        return;
    const double since = lastActivityMs_;
    toast("The TV has been idle for " + minutesText(minutes * 60.0) +
          ". It switches off in a minute; press any key to keep it on.");
    idleTimer_ = loop_.scheduleAfter(timings_.grace, [this, since, minutes]() {
        idleTimer_ = 0;
        if (!running_ || lastActivityMs_ != since || idleMinutes() != minutes)
            return;
        if (logger_)
            logger_->info("screentime: idle for " + minutesText(minutes * 60.0) + ", switching off");
        powerOff();
        lastActivityMs_ = idleNow();
    });
    if (!idleTimer_ && logger_)
        logger_->error("screentime: could not schedule the idle grace period");
}

void ScreenTimeEnforcer::powerOff() {
    if (!allowControl_ || !allowPower_) {
        logControl("power off", false);
        return;
    }
    Json request = Json::object();
    request.set("reason", Json::text("remoteKey"));
    const std::weak_ptr<int> lifetime = lifetime_;
    if (!command("screentime.power-off", "com.webos.service.tvpower/power/powerOff",
                    request, "tv-power", true,
                    [this, lifetime](const LunaReply &reply) {
                        if (lifetime.expired()) return;
                        if (reply.response["returnValue"].boolean()) {
                            logControl("power off", true);
                            return;
                        }
                        const LunaReply firstReply = reply;
                        powerTimer_ = loop_.scheduleAfter(timings_.powerSettle,
                            [this, lifetime, firstReply]() {
                                if (lifetime.expired()) return;
                                powerTimer_ = 0;
                                (void)luna_.call(
                                    "com.webos.service.tvpower/power/getPowerState", Json::object(),
                                    [this, lifetime, firstReply](const LunaReply &stateReply) {
                                        if (lifetime.expired()) return;
                                        const std::string raw =
                                            stateReply.response["state"].string();
                                        const Json mapped = names::powerState(Json::text(raw));
                                        if (mapped["display"].string() != "Unknown" &&
                                            !mapped["systemOn"].boolean()) {
                                            logControl("power off", true);
                                            return;
                                        }
                                        Json retry = Json::object();
                                        retry.set("reason", Json::text("localKey"));
                                        (void)command(
                                            "screentime.power-off-retry",
                                            "com.webos.service.tvpower/power/powerOff", retry,
                                            "tv-power", true,
                                            [this, lifetime, firstReply](const LunaReply &retryReply) {
                                                if (lifetime.expired()) return;
                                                const std::string error =
                                                    retryReply.response["errorText"].string();
                                                std::string lower = error;
                                                std::transform(lower.begin(), lower.end(),
                                                               lower.begin(), [](unsigned char c) {
                                                                   return static_cast<char>(std::tolower(c));
                                                               });
                                                logControl("power off",
                                                    retryReply.response["returnValue"].boolean() ||
                                                    lower.find("power is busy") != std::string::npos);
                                                (void)firstReply;
                                            }, "", true);
                                    });
                            });
                        if (!powerTimer_)
                            logControl("power off", false);
                    }, "", true))
        logControl("power off", false);
}

void ScreenTimeEnforcer::turnScreenOff() {
    if (!allowControl_) {
        logControl("screen off", false);
        return;
    }
    acting_ = true;
    const std::weak_ptr<int> lifetime = lifetime_;
    const auto sendOff = [this, lifetime](bool verify) {
        if (lifetime.expired()) return;
        if (!command("screentime.screen-off",
                        "com.webos.service.tvpower/power/turnOffScreen", Json::object(),
                        "tv-screen", false,
                        [this, lifetime, verify](const LunaReply &reply) {
                            if (lifetime.expired()) return;
                            logControl("screen off", reply.response["returnValue"].boolean());
                            if (verifyTimer_)
                                loop_.cancelTimer(verifyTimer_);
                            verifyTimer_ = loop_.scheduleAfter(timings_.verify,
                                [this, lifetime, verify]() {
                                    if (lifetime.expired()) return;
                                    verifyTimer_ = 0;
                                    if (verify && screenIsOn()) {
                                        if (logger_)
                                            logger_->info("screentime: the screen came back on, turning it off again");
                                        turnScreenOff();
                                    } else {
                                        acting_ = false;
                                    }
                                });
                            if (!verifyTimer_) {
                                acting_ = false;
                                logControl("screen off verification", false);
                            }
                        })) {
            acting_ = false;
            logControl("screen off", false);
        }
    };

    if (!isScreenSaver(state_.snapshot())) {
        sendOff(true);
        return;
    }
    injectKey(kKeyBack, timings_.keyPress, [this, lifetime, sendOff](bool) {
        if (lifetime.expired()) return;
        screenSaverTimer_ = loop_.scheduleAfter(timings_.screenSaver,
            [this, lifetime, sendOff]() {
                if (lifetime.expired()) return;
                screenSaverTimer_ = 0;
                sendOff(true);
            });
        if (!screenSaverTimer_) {
            acting_ = false;
            logControl("screen saver dismissal", false);
        }
    });
}

void ScreenTimeEnforcer::launchHome() {
    if (!allowControl_) {
        logControl("Home", false);
        return;
    }
    Json request = Json::object();
    request.set("id", Json::text("com.webos.app.home"));
    const std::weak_ptr<int> lifetime = lifetime_;
    if (!command("screentime.home", "com.webos.applicationManager/launch", request,
                    "application-manager", false,
                    [this, lifetime](const LunaReply &reply) {
                        if (lifetime.expired()) return;
                        if (reply.response["returnValue"].boolean()) {
                            logControl("Home", true);
                            return;
                        }
                        injectKey(kKeyLeftMeta, EventLoop::Milliseconds(100),
                                  [this, lifetime](bool ok) {
                                      if (!lifetime.expired()) logControl("Home", ok);
                                  });
                    }, "", true))
        injectKey(kKeyLeftMeta, EventLoop::Milliseconds(100),
                  [this, lifetime](bool ok) {
                      if (!lifetime.expired()) logControl("Home", ok);
                  });
}

void ScreenTimeEnforcer::injectKey(unsigned code, EventLoop::Milliseconds press,
                                   std::function<void(bool)> callback) {
    if (keyFd_ >= 0) {
        if (callback) callback(false);
        return;
    }
    const std::string path = inputDevicePath();
    keyFd_ = open(path.c_str(), O_WRONLY | O_CLOEXEC);
    const std::string fallback = rootPath(systemRoot_, "/dev/input/event1");
    if (keyFd_ < 0 && path != fallback)
        keyFd_ = open(fallback.c_str(), O_WRONLY | O_CLOEXEC);
    if (keyFd_ < 0) {
        if (callback) callback(false);
        return;
    }
    keyCode_ = code;
    keyCallback_ = std::move(callback);
    if (!writeKeyEvent(code, 1)) {
        close(keyFd_);
        keyFd_ = -1;
        std::function<void(bool)> failed = std::move(keyCallback_);
        if (failed) failed(false);
        return;
    }
    const std::weak_ptr<int> lifetime = lifetime_;
    keyTimer_ = loop_.scheduleAfter(press, [this, lifetime]() {
        if (lifetime.expired()) return;
        keyTimer_ = 0;
        const bool released = writeKeyEvent(keyCode_, 0);
        if (keyFd_ >= 0) close(keyFd_);
        keyFd_ = -1;
        std::function<void(bool)> callback = std::move(keyCallback_);
        keyCallback_ = std::function<void(bool)>();
        if (callback) callback(released);
    });
    if (!keyTimer_) {
        (void)writeKeyEvent(keyCode_, 0);
        close(keyFd_);
        keyFd_ = -1;
        std::function<void(bool)> failed = std::move(keyCallback_);
        keyCallback_ = std::function<void(bool)>();
        if (failed) failed(false);
    }
}

bool ScreenTimeEnforcer::writeKeyEvent(unsigned code, int value) {
    if (keyFd_ < 0)
        return false;
    unsigned char event[16] = {};
    const uint16_t type = value == -1 ? 0 : 1;
    const uint16_t eventCode = value == -1 ? 0 : static_cast<uint16_t>(code);
    const int32_t eventValue = value == -1 ? 0 : value;
    std::memcpy(event + 8, &type, sizeof(type));
    std::memcpy(event + 10, &eventCode, sizeof(eventCode));
    std::memcpy(event + 12, &eventValue, sizeof(eventValue));
    size_t offset = 0;
    while (offset < sizeof(event)) {
        const ssize_t written = write(keyFd_, event + offset, sizeof(event) - offset);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return false;
        offset += static_cast<size_t>(written);
    }
    if (value != -1) {
        const uint16_t syncType = 0;
        const uint16_t syncCode = 0;
        const int32_t syncValue = 0;
        std::memcpy(event + 8, &syncType, sizeof(syncType));
        std::memcpy(event + 10, &syncCode, sizeof(syncCode));
        std::memcpy(event + 12, &syncValue, sizeof(syncValue));
        offset = 0;
        while (offset < sizeof(event)) {
            const ssize_t written = write(keyFd_, event + offset, sizeof(event) - offset);
            if (written < 0 && errno == EINTR)
                continue;
            if (written <= 0)
                return false;
            offset += static_cast<size_t>(written);
        }
    }
    return true;
}

std::string ScreenTimeEnforcer::inputDevicePath() const {
    std::ifstream input(rootPath(procRoot_, "/bus/input/devices").c_str());
    bool rcu = false;
    std::string line;
    while (std::getline(input, line)) {
        if (line.find("Name=\"LGE RCU\"") != std::string::npos ||
            line.find("Name=\"Smart Remote RCU Input\"") != std::string::npos) {
            rcu = true;
            continue;
        }
        if (!rcu || line.find("Handlers=") == std::string::npos)
            continue;
        const size_t marker = line.find("event");
        if (marker == std::string::npos) {
            rcu = false;
            continue;
        }
        size_t end = marker + 5;
        while (end < line.size() && line[end] >= '0' && line[end] <= '9')
            ++end;
        if (end > marker + 5)
            return rootPath(systemRoot_, "/dev/input/") +
                   line.substr(marker, end - marker);
        rcu = false;
    }
    return rootPath(systemRoot_, "/dev/input/event1");
}

bool ScreenTimeEnforcer::screenIsOn() const {
    const Json power = state_.snapshot()["power"];
    return power["systemOn"].boolean() && power["screenOn"].boolean();
}

void ScreenTimeEnforcer::afterSwitchedOff(const service::ScreenTimeDue &due,
                                          bool screenOn) {
    if (!pendingAgain_ || pendingId_ != "again:" + due.id) {
        cancelPending();
        pendingAgain_ = true;
        pendingId_ = "again:" + due.id;
        const std::string id = due.id;
        pendingTimer_ = loop_.scheduleAfter(timings_.again, [this, id]() {
            pendingTimer_ = 0;
            pendingAgain_ = false;
            pendingId_.clear();
            const Assessment current = assess();
            if (current.valid && current.tv.due && current.tv.id == id &&
                switchedOff_.find(id) != switchedOff_.end())
                run("off", id);
        });
        if (!pendingTimer_) {
            if (logger_)
                logger_->error("screentime: could not schedule switched-on retry");
            cancelPending();
            return;
        }
    }
    if (!screenOn || pendingBlanking_)
        return;
    if (pendingTold_) {
        blankSoon(EventLoop::Milliseconds(0));
        return;
    }
    pendingTold_ = true;
    toast(due.text + " The screen goes off now, and the TV switches off in 5 minutes unless the limit is changed in Native Tv.");
    blankSoon(timings_.blank);
}

void ScreenTimeEnforcer::blankSoon(EventLoop::Milliseconds delay) {
    if (pendingId_.empty())
        return;
    pendingBlanking_ = true;
    if (blankTimer_)
        loop_.cancelTimer(blankTimer_);
    blankTimer_ = loop_.scheduleAfter(delay, [this]() {
        blankTimer_ = 0;
        pendingBlanking_ = false;
        if (pendingAgain_)
            run("screenOff", "");
    });
    if (!blankTimer_) {
        pendingBlanking_ = false;
        if (logger_)
            logger_->error("screentime: could not schedule screen blanking");
    }
}

void ScreenTimeEnforcer::cancelPending() {
    if (pendingTimer_)
        loop_.cancelTimer(pendingTimer_);
    if (blankTimer_)
        loop_.cancelTimer(blankTimer_);
    pendingTimer_ = blankTimer_ = 0;
    pendingId_.clear();
    pendingAgain_ = false;
    pendingTold_ = false;
    pendingBlanking_ = false;
}

void ScreenTimeEnforcer::saveSwitchedOff() {
    if (switchedOff_.empty()) {
        clearSwitchedOff();
        return;
    }
    Json state = Json::object();
    Json switched = Json::object();
    for (const auto &item : switchedOff_)
        if (item.second) switched.set(item.first, Json::flag(true));
    state.set("switchedOff", switched);
    std::string error;
    if (!switchedOffFile_.save(state, &error) && logger_)
        logger_->error("screentime: could not write switched-off state: " + error);
}

void ScreenTimeEnforcer::clearSwitchedOff() {
    switchedOff_.clear();
    for (const std::string &path : {switchedOffFile_.path(),
                                    switchedOffFile_.path() + ".last-good"}) {
        if (unlink(path.c_str()) != 0 && errno != ENOENT && logger_)
            logger_->error("screentime: could not remove switched-off state: " + path +
                           ": " + std::strerror(errno));
    }
}

void ScreenTimeEnforcer::logControl(const std::string &message, bool ok) {
    if (!logger_)
        return;
    logger_->write(ok ? LogLevel::Info : LogLevel::Warning,
                   "screentime: " + message + (ok ? " applied" : " failed"));
}

} // namespace daemon
} // namespace nativetv
