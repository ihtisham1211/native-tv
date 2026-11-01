#include "daemon/screensaver_control.hpp"
#include "domain/names.hpp"
#include "domain/say.hpp"
#include <algorithm>
#include <cctype>

namespace nativetv {
namespace daemon {
namespace {
const char *const kSaverAppId = "com.webos.app.screensaver";
const char *const kPowerUri = "com.webos.service.tvpower/power/getPowerState";
const char *const kAppInfoUri = "com.webos.applicationManager/getAppInfo";
const char *const kForegroundUri = "com.webos.applicationManager/getForegroundAppInfo";
const char *const kSavers[] = {"stock", "clock", "clock-date", "black", "starfield",
                               "fireworks", "bokeh", "harmonograph", "filament",
                               "currents", "veils", "incense", "murmuration", "vortex",
                               "neo", "tesseract", "vitals"};

Json failure(const std::string &error) {
    Json result = Json::object();
    result.set("ok", Json::flag(false));
    result.set("error", Json::text(error));
    return result;
}

Json okResult() {
    Json result = Json::object();
    result.set("ok", Json::flag(true));
    return result;
}

bool knownSaver(const std::string &mode) {
    for (const char *saver : kSavers)
        if (mode == saver) return true;
    return false;
}

// tvpower's state name as the names table keys it: lower case, no separators.
std::string powerKey(const Json &reply) {
    std::string key = reply["state"].string();
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    key.erase(std::remove_if(key.begin(), key.end(), [](unsigned char c) {
                  return std::isspace(c) || c == '_' || c == '-';
              }), key.end());
    return key;
}

bool isScreenSaver(const Json &power) {
    return power.isObject() && powerKey(power) == "screensaver";
}

bool returnedFalse(const Json &reply) {
    return reply["returnValue"].isBoolean() && !reply["returnValue"].boolean();
}

std::string errorOr(const Json &reply, const std::string &fallback) {
    return reply["errorText"].isString() && !reply["errorText"].string().empty()
        ? reply["errorText"].string() : fallback;
}
} // namespace

ScreensaverControl::ScreensaverControl(ScreensaverControlDeps deps)
    : deps_(std::move(deps)) {
    launched_ = deps_.launchedMarkerExists && deps_.launchedMarkerExists();
}

void ScreensaverControl::setLaunched(bool on) {
    if (deps_.writeLaunchedMarker) deps_.writeLaunchedMarker(on);
    launched_ = on;
}

bool ScreensaverControl::running(bool &known) {
    known = false;
    Json reply;
    if (!deps_.luna("com.webos.applicationManager/running", Json::object(), reply) ||
        !reply["running"].isArray())
        return false;
    known = true;
    for (const Json &app : reply["running"].items())
        if (app["id"].string() == kSaverAppId) return true;
    return false;
}

bool ScreensaverControl::launchedShowing() {
    if (!launched_) return false;
    bool known = false;
    const bool showing = running(known);
    if (known && !showing) setLaunched(false);
    return launched_;
}

void ScreensaverControl::adoptLaunched() {
    if (!deps_.launchedMarkerExists || !deps_.launchedMarkerExists()) return;
    Json power;
    if (!deps_.luna(kPowerUri, Json::object(), power) || returnedFalse(power)) return;
    if (isScreenSaver(power)) {
        setLaunched(false);
        return;
    }
    bool known = false;
    const bool showing = running(known);
    if (known) setLaunched(showing);
}

bool ScreensaverControl::closeLaunched(Json &reply) {
    Json payload = Json::object();
    payload.set("id", Json::text(kSaverAppId));
    if (!deps_.luna("com.webos.applicationManager/closeByAppId", payload, reply))
        reply = Json();
    if (reply["returnValue"].boolean()) setLaunched(false);
    return reply["returnValue"].boolean();
}

void ScreensaverControl::restartApp() {
    Json power;
    const bool answered = deps_.luna(kPowerUri, Json::object(), power);
    Json close = Json::object();
    close.set("id", Json::text(kSaverAppId));
    Json ignored;
    if (!answered || !isScreenSaver(power)) {
        (void)deps_.luna("com.webos.applicationManager/closeByAppId", close, ignored);
        return;
    }
    // Showing now: dismiss it with Back so the new one starts clean, then bring
    // one up again.
    if (!deps_.injectBack) return;
    (void)deps_.injectBack();
    deps_.sleepMs(1500);
    (void)deps_.luna("com.webos.applicationManager/closeByAppId", close, ignored);
    deps_.sleepMs(1500);
    (void)deps_.luna("com.webos.service.tvpower/power/turnOnScreenSaver", Json::object(), ignored);
}

bool ScreensaverControl::settle() {
    const std::string staged = deps_.mountedType();
    if (!staged.empty()) {
        Json id = Json::object();
        id.set("id", Json::text(kSaverAppId));
        const auto samHolds = [&]() {
            Json info;
            return deps_.luna(kAppInfoUri, id, info) && info["appInfo"]["type"].string() == staged;
        };
        const auto samSoon = [&]() {
            for (int attempt = 0; attempt < 10; ++attempt) {
                if (samHolds()) return true;
                deps_.sleepMs(500);
            }
            return false;
        };
        Json info;
        const bool answered = deps_.luna(kAppInfoUri, id, info);
        const std::string cached = answered ? info["appInfo"]["type"].string() : std::string();
        // No answer means the bus is not up yet; leave the service alone. sam is
        // never restarted for a screen saver (that step left the picture and
        // sound off on some TVs), so a switch it will not take is undone instead.
        if (!cached.empty() && cached != staged) {
            const bool reread = deps_.samReread && deps_.samReread() && samSoon();
            if (!reread) {
                const bool nudged = deps_.samNudge && deps_.samNudge() && samSoon();
                if (!nudged) return true;
            }
        }
    }
    restartApp();
    return false;
}

Json ScreensaverControl::setMode(const std::string &mode, const std::string &level) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!knownSaver(mode)) return failure("unknown screen saver: " + mode);
    const service::ScreensaverState before = deps_.state();
    if (mode != "stock" && service::screensaverHeldBack(before))
        return failure(say::msg("srv.saver.held",
            "Custom screen savers are turned off on this TV for now, as they have not been "
            "tried on its webOS version. Before 0.98.1, on webOS 10, they could leave the "
            "picture, the sound and HDMI control off until the TV was unplugged. One already "
            "in use stays until the TV is next fully restarted."));
    const std::string chosenLevel = level == "bright" ? "bright" : "dim";

    const Json staged = deps_.stage(mode, chosenLevel);
    if (!staged["ok"].boolean()) {
        return failure("could not stage the screen saver: " +
            (staged["error"].isString() ? staged["error"].string()
                                        : std::string("native staging failed")));
    }
    const auto answer = [&](bool ok, const std::string &error) {
        const service::ScreensaverState now = deps_.state();
        Json result = Json::object();
        result.set("ok", Json::flag(ok));
        result.set("current", Json::text(service::screensaverMode(now)));
        result.set("level", Json::text(service::screensaverLevel(now)));
        result.set("switching", Json::flag(false));
        if (!ok && !error.empty()) result.set("error", Json::text(error));
        return result;
    };

    const bool notTaken = settle();
    if (mode == "stock") {
        const bool stock = service::screensaverMode(deps_.state()) == "stock";
        return answer(!notTaken && stock, notTaken ? say::msg("srv.saver.stockLater",
            "LG's screen saver is set, and the TV uses it from its next restart: it did not "
            "take the change without restarting its app manager, which has left the picture "
            "or sound off on some TVs.") : std::string());
    }
    if (notTaken) {
        // sam still runs LG's: put LG's files back under it, so they agree.
        deps_.restoreStock();
        return answer(false, say::msg("srv.saver.notTaken",
            "The TV did not take the new screen saver without restarting its app manager, "
            "which has left the picture or sound off on some TVs, so LG's screen saver is kept."));
    }
    const bool mounted = staged["mounted"].boolean();
    const bool took = mounted && service::screensaverMode(deps_.state()) == mode;
    return answer(took, took ? std::string()
        : staged["error"].isString() ? staged["error"].string()
        : std::string("the mount did not take"));
}

bool ScreensaverControl::readMedia(Json &media) {
    const auto idle = []() {
        Json value = Json::object();
        value.set("state", Json::text("idle"));
        value.set("fullScreen", Json::flag(false));
        return value;
    };
    const auto fromMediaService = [&]() {
        Json reply;
        if (!deps_.luna("com.webos.media/getForegroundAppInfo", Json::object(), reply) ||
            returnedFalse(reply) || !reply["foregroundAppInfo"].isArray())
            return false;
        for (const Json &app : reply["foregroundAppInfo"].items()) {
            if (!app["playState"].isString() || app["playState"].string().empty()) continue;
            media = Json::object();
            media.set("state", app["playState"]);
            media.set("fullScreen", Json::flag(true));
            return true;
        }
        media = idle();
        return true;
    };
    if (!acbAbsent_) {
        Json acb;
        const bool answered = deps_.luna("com.webos.service.acb/getForegroundAppInfo",
                                         Json::object(), acb);
        const Json pipe = answered ? acb["acbs"][0] : Json();
        if (pipe["playStateNow"].isString() && !pipe["playStateNow"].string().empty()) {
            media = Json::object();
            media.set("state", pipe["playStateNow"]);
            media.set("fullScreen", Json::flag(
                !(pipe["isFullScreen"].isBoolean() && !pipe["isFullScreen"].boolean())));
            return true;
        }
        if (answered && !returnedFalse(acb) && acb["acbs"].isArray()) {
            media = idle();
            return true;
        }
        if (answered && acb["errorText"].string().find("Service does not exist") !=
                            std::string::npos)
            acbAbsent_ = true;
    }
    return fromMediaService();
}

bool ScreensaverControl::launchApp(std::string &error) {
    error.clear();
    // tvpower's refusal also protects playback. An unavailable media service is
    // not evidence that nothing is playing.
    Json media;
    if (!readMedia(media)) {
        error = say::msg("srv.saver.unknownPlayback",
                         "the TV could not check whether anything is playing");
        return false;
    }
    if (media["state"].string() == "playing" && media["fullScreen"].boolean()) {
        error = say::msg("srv.saver.playing",
                         "the TV does not start a screen saver while something is playing");
        return false;
    }
    // The checks can take seconds: do not cover an input or undo Screen Off if
    // the source or power changed in the meantime.
    Json power;
    const bool answered = deps_.luna(kPowerUri, Json::object(), power);
    if (answered && isScreenSaver(power)) return true;
    const Json current = names::powerState(Json::text(power["state"].string()));
    const std::string processing = power["processing"].string();
    if (!answered || returnedFalse(power) || !current["screenOn"].boolean() ||
        (!processing.empty() && processing != "Screen On"))
        return false;
    Json foreground;
    const bool haveForeground = deps_.luna(kForegroundUri, Json::object(), foreground);
    const std::string appId = foreground["appId"].string();
    const std::string input = haveForeground && !returnedFalse(foreground) && !appId.empty()
        ? names::shortAppId(appId) : std::string();
    if (input.empty() || names::isHdmiKey(input) || input == "livetv") return false;
    Json launch = Json::object();
    launch.set("id", Json::text(kSaverAppId));
    Json reply;
    const bool ok = deps_.luna("com.webos.applicationManager/launch", launch, reply) &&
                    reply["returnValue"].boolean();
    if (ok) setLaunched(true);
    return ok;
}

// True where the screen saver tvpower agreed to start was then refused: on a
// G3 (webOS 9.2.2) turnOnScreenSaver returned true and tvpower went back to
// Active with nothing started. False once it shows, or while tvpower is still
// working on it after the last look, so a slow start is not given a second one.
bool ScreensaverControl::refusedAfterRequest() {
    for (int look = 0; look < 6; ++look) {
        deps_.sleepMs(500);
        Json power;
        if (!deps_.luna(kPowerUri, Json::object(), power) || isScreenSaver(power))
            return false;
        const std::string processing = power["processing"].string();
        const bool settled = names::powerState(Json::text(power["state"].string()))
                                 ["screenOn"].boolean() &&
                             (processing.empty() || processing == "Screen On");
        if (settled) return true;
    }
    return false;
}

bool ScreensaverControl::ensureKnown() {
    Json id = Json::object();
    id.set("id", Json::text(kSaverAppId));
    const auto known = [&](Json &reply) {
        const bool answered = deps_.luna(kAppInfoUri, id, reply);
        return answered && reply["appInfo"].valid() && !reply["appInfo"].isBoolean();
    };
    Json first;
    const bool answered = deps_.luna(kAppInfoUri, id, first);
    // No answer, or an answer that is not a refusal, leaves a start that may work.
    if (!answered || (first["appInfo"].valid()) || !returnedFalse(first)) return true;
    const auto knownSoon = [&]() {
        for (int attempt = 0; attempt < 8; ++attempt) {
            Json reply;
            if (known(reply)) return true;
            deps_.sleepMs(500);
        }
        return false;
    };
    if (deps_.samNudge && deps_.samNudge() && knownSoon()) return true;
    return deps_.samReread && deps_.samReread() && knownSoon();
}

Json ScreensaverControl::trigger() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (int again = 0; again < 2; ++again) {
        Json power;
        const bool answered = deps_.luna(kPowerUri, Json::object(), power);
        if (answered && isScreenSaver(power)) {
            if (!deps_.injectBack) return failure("key injection not available");
            return deps_.injectBack() ? okResult()
                : failure(say::msg("srv.saver.noRemote",
                                   "could not reach the remote input device"));
        }
        if (launched_) {
            bool known = false;
            const bool showing = running(known);
            // Closed already: start one. No answer is not closed.
            if (known && !showing) {
                setLaunched(false);
                continue;
            }
            Json reply;
            if (closeLaunched(reply)) return okResult();
            return failure(say::msg("srv.saver.noClose",
                "the TV would not close the screen saver: {error}",
                [&]() {
                    Json variables = Json::object();
                    variables.set("error", Json::text(errorOr(reply, "no answer")));
                    return variables;
                }()));
        }
        break;
    }

    Json foreground;
    (void)deps_.luna(kForegroundUri, Json::object(), foreground);
    const std::string appId = foreground["appId"].string();
    const std::string source = appId.empty() ? std::string() : names::shortAppId(appId);
    if (names::isHdmiKey(source) || source == "livetv") {
        Json variables = Json::object();
        variables.set("source", Json::text(source));
        return failure(say::msg("srv.saver.onlyInApps",
            "the screen saver is only available from an app, not from {source}", variables));
    }
    if (!ensureKnown())
        return failure(say::msg("srv.saver.lost",
            "The TV has lost track of its screen saver app and did not find it again. "
            "Restarting the TV brings it back."));

    const auto sourceRefusal = [&]() {
        if (source.empty())
            return say::msg("srv.saver.refusedHere",
                            "the TV would not start a screen saver from this source");
        Json variables = Json::object();
        variables.set("source", Json::text(source));
        return say::msg("srv.saver.refusedFrom",
                        "the TV would not start a screen saver from {source}", variables);
    };
    Json reply;
    (void)deps_.luna("com.webos.service.tvpower/power/turnOnScreenSaver", Json::object(), reply);
    std::string launchError;
    if (reply["returnValue"].boolean()) {
        if (!refusedAfterRequest()) return okResult();
        return launchApp(launchError) ? okResult()
            : failure(!launchError.empty() ? launchError : sourceRefusal());
    }
    const auto refusedHere = [&]() {
        Json variables = Json::object();
        variables.set("error", Json::text(reply["errorText"].string()));
        return say::msg("srv.saver.refused",
                        "the TV would not start a screen saver here: {error}", variables);
    };
    if (reply["errorText"].string() == "Invalid State change Request") {
        if (launchApp(launchError)) return okResult();
        return failure(!launchError.empty() ? launchError : refusedHere());
    }
    return failure(!reply["errorText"].string().empty() ? refusedHere() : sourceRefusal());
}

} // namespace daemon
} // namespace nativetv
