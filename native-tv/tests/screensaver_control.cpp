#include "daemon/screensaver_control.hpp"
#include <cstdio>
#include <deque>
#include <map>
#include <stdexcept>
#include <vector>

using namespace nativetv;
using namespace nativetv::daemon;

namespace {
void check(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}

// A scripted bus: each URI answers from its queue, then repeats its last reply.
struct Bus {
    std::map<std::string, std::deque<Json>> replies;
    std::vector<std::string> calls;
    bool backInjected = false;
    bool markerPresent = false;
    int rereads = 0;
    int nudges = 0;
    int restores = 0;
    bool samAccepts = false;
    std::string mounted = "qml";
    service::ScreensaverState state;
    Json staged = Json::parse("{\"ok\":true,\"mounted\":true}");

    void say(const std::string &uri, const std::string &json) {
        replies[uri].push_back(Json::parse(json));
    }
    ScreensaverControlDeps deps() {
        ScreensaverControlDeps d;
        d.luna = [this](const std::string &uri, const Json &, Json &reply) {
            calls.push_back(uri);
            auto found = replies.find(uri);
            if (found == replies.end() || found->second.empty()) return false;
            reply = found->second.front();
            if (found->second.size() > 1) found->second.pop_front();
            return true;
        };
        d.injectBack = [this]() { backInjected = true; return true; };
        d.state = [this]() { return state; };
        d.stage = [this](const std::string &, const std::string &) { return staged; };
        d.restoreStock = [this]() { ++restores; };
        d.mountedType = [this]() { return mounted; };
        d.samReread = [this]() { ++rereads; return samAccepts; };
        d.samNudge = [this]() { ++nudges; return samAccepts; };
        d.sleepMs = [](unsigned) {};
        d.writeLaunchedMarker = [this](bool on) { markerPresent = on; };
        d.launchedMarkerExists = [this]() { return markerPresent; };
        return d;
    }
    size_t count(const std::string &uri) const {
        size_t n = 0;
        for (const std::string &call : calls) n += call == uri;
        return n;
    }
};

const char *const kPower = "com.webos.service.tvpower/power/getPowerState";
const char *const kForeground = "com.webos.applicationManager/getForegroundAppInfo";
const char *const kAppInfo = "com.webos.applicationManager/getAppInfo";
const char *const kTurnOn = "com.webos.service.tvpower/power/turnOnScreenSaver";
const char *const kLaunch = "com.webos.applicationManager/launch";
const char *const kClose = "com.webos.applicationManager/closeByAppId";
const char *const kRunning = "com.webos.applicationManager/running";
const char *const kAcb = "com.webos.service.acb/getForegroundAppInfo";

void appBehindNetflix(Bus &bus) {
    bus.say(kPower, "{\"returnValue\":true,\"state\":\"Active\"}");
    bus.say(kForeground, "{\"returnValue\":true,\"appId\":\"netflix\"}");
    bus.say(kAppInfo, "{\"returnValue\":true,\"appInfo\":{\"type\":\"flutter\"}}");
}
} // namespace

int main() {
    try {
        {   // Already showing: Back dismisses it.
            Bus bus;
            bus.say(kPower, "{\"returnValue\":true,\"state\":\"Screen Saver\"}");
            ScreensaverControl control(bus.deps());
            check(control.trigger()["ok"].boolean() && bus.backInjected, "back dismisses");
        }
        {   // From an HDMI input there is nothing to cover.
            Bus bus;
            bus.say(kPower, "{\"returnValue\":true,\"state\":\"Active\"}");
            bus.say(kForeground, "{\"returnValue\":true,\"appId\":\"com.webos.app.hdmi2\"}");
            const Json result = ScreensaverControl(bus.deps()).trigger();
            check(!result["ok"].boolean(), "hdmi refused");
            check(result["error"].string().find("only available from an app") !=
                      std::string::npos, "hdmi wording");
            check(bus.count(kTurnOn) == 0, "no request from hdmi");
        }
        {   // tvpower agrees and the saver shows.
            Bus bus;
            appBehindNetflix(bus);
            bus.say(kPower, "{\"returnValue\":true,\"state\":\"Screen Saver\"}");
            bus.say(kTurnOn, "{\"returnValue\":true}");
            check(ScreensaverControl(bus.deps()).trigger()["ok"].boolean(), "saver starts");
            check(bus.count(kLaunch) == 0 && !bus.markerPresent, "no launch needed");
        }
        {   // tvpower agrees but drops back to Active: the app is launched instead,
            // and a second trigger closes it.
            Bus bus;
            appBehindNetflix(bus);
            bus.say(kPower, "{\"returnValue\":true,\"state\":\"Active\"}");
            bus.say(kTurnOn, "{\"returnValue\":true}");
            bus.say(kAcb, "{\"returnValue\":true,\"acbs\":[]}");
            bus.say(kLaunch, "{\"returnValue\":true}");
            ScreensaverControl control(bus.deps());
            check(control.trigger()["ok"].boolean(), "launched after refusal");
            check(bus.count(kLaunch) == 1 && bus.markerPresent, "launch recorded");
            bus.say(kRunning, "{\"returnValue\":true,\"running\":[{\"id\":\"com.webos.app.screensaver\"}]}");
            bus.say(kClose, "{\"returnValue\":true}");
            check(control.trigger()["ok"].boolean() && !bus.markerPresent, "second trigger closes");
            check(bus.count(kClose) == 1, "closed once");
        }
        {   // A refused request while something plays says why.
            Bus bus;
            appBehindNetflix(bus);
            bus.say(kTurnOn, "{\"returnValue\":false,\"errorText\":\"Invalid State change Request\"}");
            bus.say(kAcb, "{\"returnValue\":true,\"acbs\":[{\"playStateNow\":\"playing\",\"isFullScreen\":true}]}");
            const Json result = ScreensaverControl(bus.deps()).trigger();
            check(!result["ok"].boolean() &&
                      result["error"].string().find("something is playing") != std::string::npos,
                  "playing refusal");
            check(bus.count(kLaunch) == 0, "nothing launched over playback");
        }
        {   // No media answer is not evidence of silence.
            Bus bus;
            appBehindNetflix(bus);
            bus.say(kTurnOn, "{\"returnValue\":false,\"errorText\":\"Invalid State change Request\"}");
            bus.say(kAcb, "{\"returnValue\":false,\"errorText\":\"Service does not exist\"}");
            const Json result = ScreensaverControl(bus.deps()).trigger();
            check(result["error"].string().find("could not check") != std::string::npos,
                  "unknown playback refusal");
        }
        {   // A launched saver left by an earlier run is adopted or forgotten.
            Bus bus;
            bus.markerPresent = true;
            bus.say(kPower, "{\"returnValue\":true,\"state\":\"Active\"}");
            bus.say(kRunning, "{\"returnValue\":true,\"running\":[]}");
            ScreensaverControl control(bus.deps());
            check(control.launchedShowing() == false && !bus.markerPresent,
                  "stale marker is cleared");
        }
        {   // Switching: unknown, held back, then the stock and custom paths.
            Bus bus;
            bus.state.stockType = "flutter";
            bus.state.webosMajor = 12;
            ScreensaverControl control(bus.deps());
            check(control.setMode("nope", "dim")["error"].string() == "unknown screen saver: nope",
                  "unknown mode");
            check(control.setMode("clock", "dim")["error"].string().find("turned off") !=
                      std::string::npos, "held back");
        }
        {   // sam holds LG's type and will not reread: the switch is undone.
            Bus bus;
            bus.state.stockType = "qml";
            bus.state.markerMode = "clock";
            bus.say(kAppInfo, "{\"returnValue\":true,\"appInfo\":{\"type\":\"flutter\"}}");
            const Json result = ScreensaverControl(bus.deps()).setMode("clock", "bright");
            check(!result["ok"].boolean() && bus.restores == 1, "not taken is restored");
            check(result["error"].string().find("LG's screen saver is kept") != std::string::npos,
                  "not taken wording");
            check(bus.rereads == 1 && bus.nudges == 1, "tried reread then nudge");
        }
        {   // sam rereads on the first try: the app is closed to restart it.
            Bus bus;
            bus.state.stockType = "flutter";
            bus.state.webosMajor = 10;
            bus.state.markerMode = "clock";
            bus.state.markerLevel = "bright";
            bus.samAccepts = true;
            bus.say(kAppInfo, "{\"returnValue\":true,\"appInfo\":{\"type\":\"flutter\"}}");
            bus.say(kAppInfo, "{\"returnValue\":true,\"appInfo\":{\"type\":\"qml\"}}");
            bus.say(kPower, "{\"returnValue\":true,\"state\":\"Active\"}");
            const Json result = ScreensaverControl(bus.deps()).setMode("clock", "bright");
            check(result["ok"].boolean(), "taken after reread");
            check(result["current"].string() == "clock" && result["level"].string() == "bright",
                  "reports the mounted saver");
            check(bus.count(kClose) == 1 && bus.restores == 0, "app restarted, not restored");
        }
        {   // Back to LG's: the mount failing to stage is reported.
            Bus bus;
            bus.state.stockType = "qml";
            bus.staged = Json::parse("{\"ok\":false,\"error\":\"disk full\"}");
            const Json result = ScreensaverControl(bus.deps()).setMode("stock", "dim");
            check(result["error"].string() == "could not stage the screen saver: disk full",
                  "staging error");
        }
        {
            Bus bus;
            bus.state.stockType = "qml";
            bus.say(kPower, "{\"returnValue\":true,\"state\":\"Active\"}");
            const Json result = ScreensaverControl(bus.deps()).setMode("stock", "bright");
            check(result["ok"].boolean() && result["current"].string() == "stock",
                  "stock restored");
            check(result["switching"].isBoolean() && !result["switching"].boolean(),
                  "never switching");
        }
        // test-screensaver-lost.js: where sam has lost the screen saver app it
        // rereads its apps first, and says so if that fails.
        const char *const kLostApp =
            "{\"returnValue\":false,\"errorCode\":1,\"errorText\":\"Invalid appId specified OR Unsupported Application Type: "
            "com.webos.app.screensaver\"}";
        {   // A nudge makes sam find it again, and then the saver is started.
            Bus bus;
            bus.say(kPower, "{\"returnValue\":true,\"state\":\"Active\"}");
            bus.say(kPower, "{\"returnValue\":true,\"state\":\"Screen Saver\"}");
            bus.say(kForeground, "{\"returnValue\":true,\"appId\":\"netflix\"}");
            bus.say(kAppInfo, kLostApp);
            bus.say(kAppInfo, "{\"returnValue\":true,\"appInfo\":{\"type\":\"qml\"}}");
            bus.say(kTurnOn, "{\"returnValue\":true}");
            bus.samAccepts = true;
            const Json result = ScreensaverControl(bus.deps()).trigger();
            check(result["ok"].boolean(), "a lost saver app is found again by a reread");
            check(bus.nudges >= 1 && bus.rereads == 0, "sam was nudged to read its apps again");
            check(bus.count(kTurnOn) == 1, "then the screen saver was started");
        }
        {   // The nudge refused, a reread of the one app takes.
            Bus bus;
            bus.say(kPower, "{\"returnValue\":true,\"state\":\"Active\"}");
            bus.say(kPower, "{\"returnValue\":true,\"state\":\"Screen Saver\"}");
            bus.say(kForeground, "{\"returnValue\":true,\"appId\":\"netflix\"}");
            bus.say(kAppInfo, kLostApp);
            bus.say(kTurnOn, "{\"returnValue\":true}");
            bool nudgeAnswered = false;
            ScreensaverControlDeps deps = bus.deps();
            deps.samNudge = [&]() { ++bus.nudges; return false; };
            deps.samReread = [&]() {
                ++bus.rereads;
                bus.replies[kAppInfo].clear();
                bus.say(kAppInfo, "{\"returnValue\":true,\"appInfo\":{\"type\":\"qml\"}}");
                nudgeAnswered = true;
                return true;
            };
            check(ScreensaverControl(deps).trigger()["ok"].boolean() && nudgeAnswered,
                  "a reread of the app is the second try");
            check(bus.nudges == 1 && bus.rereads == 1 && bus.count(kTurnOn) == 1, "nudge, then reread, then start");
        }
        {   // One still lost after the reread is reported, not started silently.
            for (bool accepts : {true, false}) {
                Bus bus;
                bus.say(kPower, "{\"returnValue\":true,\"state\":\"Active\"}");
                bus.say(kForeground, "{\"returnValue\":true,\"appId\":\"netflix\"}");
                bus.say(kAppInfo, kLostApp);
                bus.samAccepts = accepts;
                const Json result = ScreensaverControl(bus.deps()).trigger();
                check(!result["ok"].boolean() &&
                          result["error"].string().find("lost track of its screen saver app") != std::string::npos,
                      "reported as lost");
                check(bus.count(kTurnOn) == 0, "nothing is asked of tvpower");
                check(bus.nudges == 1 && bus.rereads == 1, "both tries were made");
            }
        }

        // test-screensaver-dropped.js: launch, dismissal, restart recovery and refusals.
        {   // A launched saver survives a server restart and closes on the next press.
            Bus bus;
            appBehindNetflix(bus);
            bus.say(kTurnOn, "{\"returnValue\":true}");
            bus.say(kAcb, "{\"returnValue\":true,\"acbs\":[]}");
            bus.say(kLaunch, "{\"returnValue\":true}");
            check(ScreensaverControl(bus.deps()).trigger()["ok"].boolean() && bus.markerPresent, "launched");
            bus.say(kRunning, "{\"returnValue\":true,\"running\":[{\"id\":\"com.webos.app.screensaver\"}]}");
            ScreensaverControl restarted(bus.deps());
            restarted.adoptLaunched();
            check(restarted.launchedShowing() && bus.markerPresent, "recovered across a server restart");
            bus.say(kClose, "{\"returnValue\":true}");
            check(restarted.trigger()["ok"].boolean() && !bus.markerPresent && !restarted.launchedShowing(),
                  "closed on the next press");
        }
        {   // A saver nobody here launched is left to tvpower.
            Bus bus;
            bus.say(kPower, "{\"returnValue\":true,\"state\":\"Active\"}");
            bus.say(kRunning, "{\"returnValue\":true,\"running\":[{\"id\":\"com.webos.app.screensaver\"}]}");
            ScreensaverControl control(bus.deps());
            control.adoptLaunched();
            check(!control.launchedShowing() && bus.count(kClose) == 0,
                  "cached native screen savers are not adopted");
            Bus own;
            own.markerPresent = true;
            own.say(kPower, "{\"returnValue\":true,\"state\":\"Screen Saver\"}");
            own.say(kRunning, "{\"returnValue\":true,\"running\":[{\"id\":\"com.webos.app.screensaver\"}]}");
            ScreensaverControl handedOver(own.deps());
            handedOver.adoptLaunched();
            check(!handedOver.launchedShowing() && !own.markerPresent, "tvpower's own saver is not ours");
        }
        {   // Failed and unanswered closes keep tracking, and the press stays retryable.
            Bus bus;
            appBehindNetflix(bus);
            bus.say(kTurnOn, "{\"returnValue\":true}");
            bus.say(kAcb, "{\"returnValue\":true,\"acbs\":[]}");
            bus.say(kLaunch, "{\"returnValue\":true}");
            ScreensaverControl control(bus.deps());
            check(control.trigger()["ok"].boolean(), "launched");
            bus.say(kRunning, "{\"returnValue\":true,\"running\":[{\"id\":\"com.webos.app.screensaver\"}]}");
            bus.say(kClose, "{\"returnValue\":false,\"errorText\":\"busy\"}");
            const Json refused = control.trigger();
            check(!refused["ok"].boolean() && refused["error"].string().find("busy") != std::string::npos &&
                      control.launchedShowing() && bus.markerPresent,
                  "a refused close keeps tracking");
            bus.replies.erase(kClose);
            const Json unanswered = control.trigger();
            check(!unanswered["ok"].boolean() && unanswered["error"].string().find("no answer") != std::string::npos &&
                      control.launchedShowing(),
                  "an unanswered close keeps tracking");
            bus.say(kClose, "{\"returnValue\":true}");
            check(control.trigger()["ok"].boolean() && !control.launchedShowing() && bus.count(kClose) == 3,
                  "the third press closes it");
        }
        {   // A saver closed elsewhere is cleared, and the next press starts another.
            Bus bus;
            appBehindNetflix(bus);
            bus.say(kTurnOn, "{\"returnValue\":true}");
            bus.say(kAcb, "{\"returnValue\":true,\"acbs\":[]}");
            bus.say(kLaunch, "{\"returnValue\":true}");
            ScreensaverControl control(bus.deps());
            check(control.trigger()["ok"].boolean() && bus.count(kLaunch) == 1, "launched");
            bus.say(kRunning, "{\"returnValue\":true,\"running\":[]}");
            check(!control.launchedShowing() && !bus.markerPresent, "cleared");
            check(control.trigger()["ok"].boolean() && bus.count(kLaunch) == 2, "the next press starts another");
        }
        {   // A dropped request, like an immediate refusal, cannot cover playback it
            // cannot see or full-screen playback.
            for (bool refusal : {false, true}) {
                for (bool playing : {true, false}) {
                    Bus bus;
                    appBehindNetflix(bus);
                    bus.say(kTurnOn, refusal ? "{\"returnValue\":false,\"errorText\":\"Invalid State change Request\"}"
                                             : "{\"returnValue\":true}");
                    bus.say(kAcb, playing
                        ? "{\"returnValue\":true,\"acbs\":[{\"playStateNow\":\"playing\",\"isFullScreen\":true}]}"
                        : "{\"returnValue\":false,\"errorText\":\"Service does not exist\"}");
                    const Json result = ScreensaverControl(bus.deps()).trigger();
                    check(!result["ok"].boolean() && bus.count(kLaunch) == 0, "nothing launched");
                    check(result["error"].string().find(playing ? "something is playing" : "could not check") !=
                              std::string::npos,
                          "the refusal says why");
                }
            }
        }
        {   // A delayed fallback respects a change in power or source meanwhile.
            const char *const changes[] = {
                "{\"returnValue\":true,\"state\":\"Screen Off\"}",
                "{\"returnValue\":true,\"state\":\"Active Standby\"}",
                "app:com.webos.app.hdmi2",
                "app:com.webos.app.livetv"};
            for (const char *change : changes) {
                Bus bus;
                appBehindNetflix(bus);
                bus.say(kTurnOn, "{\"returnValue\":false,\"errorText\":\"Invalid State change Request\"}");
                bus.say(kAcb, "{\"returnValue\":true,\"acbs\":[]}");
                const std::string text = change;
                if (text.compare(0, 4, "app:") == 0)
                    bus.say(kForeground, "{\"returnValue\":true,\"appId\":\"" + text.substr(4) + "\"}");
                else
                    bus.say(kPower, text);
                const Json result = ScreensaverControl(bus.deps()).trigger();
                check(!result["ok"].boolean() && bus.count(kLaunch) == 0,
                      "a delayed fallback respects a change in power or source");
            }
        }
    } catch (const std::exception &error) {
        fprintf(stderr, "screensaver control test failed: %s\n", error.what());
        return 1;
    }
    return 0;
}
