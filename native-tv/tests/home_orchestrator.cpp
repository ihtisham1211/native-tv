#include "daemon/home_orchestrator.hpp"
#include "service/home_profiles.hpp"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>

using namespace nativetv;
using namespace nativetv::daemon;

namespace {
void check(bool value, const std::string &message) {
    if (!value) throw std::runtime_error(message);
}

std::string readFile(const std::string &path) {
    std::ifstream stream(path);
    std::stringstream text;
    text << stream.rdbuf();
    return text.str();
}

Json copyOf(const Json &value) { return value.valid() ? Json::parse(value.dump()) : Json(); }

// Keys in order and no escaped slashes, as the fixture's recorder writes it.
std::string canonical(const Json &value) {
    if (!value.valid()) return "null";
    if (value.isObject()) {
        std::vector<std::string> keys = value.keys();
        std::sort(keys.begin(), keys.end());
        std::string out = "{";
        for (size_t i = 0; i < keys.size(); ++i)
            out += (i ? "," : "") + Json::text(keys[i]).dump(false) + ":" + canonical(value[keys[i]]);
        return out + "}";
    }
    if (value.isArray()) {
        std::string out = "[";
        for (size_t i = 0; i < value.size(); ++i) out += (i ? "," : "") + canonical(value[i]);
        return out + "]";
    }
    return value.dump(false);
}

std::string joined(const std::vector<std::string> &events) {
    std::string out;
    for (const std::string &event : events) out += (out.empty() ? "" : " | ") + event;
    return out;
}

// A clock the test moves: timers run, in order, as it passes them.
struct Clock {
    long long now = 0;
    unsigned long long serial = 0;
    std::map<unsigned long long, std::pair<long long, std::function<void()>>> timers;

    unsigned long long after(long long delay, std::function<void()> task) {
        timers[++serial] = {now + delay, std::move(task)};
        return serial;
    }
    void cancel(unsigned long long id) { timers.erase(id); }
    void advance(long long ms) {
        const long long target = now + ms;
        for (;;) {
            auto next = timers.end();
            for (auto it = timers.begin(); it != timers.end(); ++it)
                if (it->second.first <= target &&
                    (next == timers.end() || it->second.first < next->second.first))
                    next = it;
            if (next == timers.end()) break;
            now = next->second.first;
            std::function<void()> task = std::move(next->second.second);
            timers.erase(next);
            task();
        }
        now = target;
    }
};

// The TV and its files, scripted from a scenario.
struct World {
    Clock clock;
    Json raw = Json::object();          // home.json as saved
    Json settings = Json::object();     // the general settings
    std::vector<std::string> events;
    bool recording = true;
    std::string webos = "9.2.2";
    bool experimental = false;
    bool mounted = false;
    bool escaped = false;
    bool blankKept = false;
    bool takeoverOk = true;
    bool noticeOk = true;
    bool writeOk = true;
    bool settingsUnreadable = false;
    std::string homeKey;
    bool homeKeyClearOk = true;
    int legacyInstalled = 0;
    bool legacyRunning = false;
    bool legacyKillFails = false;
    std::string powerOnReason = "remoteKey";
    std::vector<std::string> errors;
    std::vector<std::string> notes;
    std::function<void()> afterSync;
    // The crash watch: wired only where a test asks, so the recorded scenarios run without it.
    bool watch = false;
    int clientListed = 0;          // 1 the app manager lists the client, 0 it does not, -1 it did not say
    bool releaseOk = true;
    Json probeRefusal;             // when set, supported() answers with it

    void ev(const std::string &text) {
        if (recording) events.push_back(text);
    }
    bool startsWith(const std::string &prefix) const { return webos.compare(0, prefix.size(), prefix) == 0; }
    Json document() const { return service::normalizeHomeDocument(raw); }
};

HomeOrchestratorDeps depsFor(World &w) {
    HomeOrchestratorDeps d;
    d.config = [&w]() { return w.document(); };
    d.update = [&w](const std::function<void(Json &)> &mutate, std::string &) {
        Json document = copyOf(w.document());
        mutate(document);
        w.raw = service::normalizeHomeDocument(document);
        return true;
    };
    d.supported = [&w]() {
        if (w.probeRefusal.valid()) return copyOf(w.probeRefusal);
        Json result = Json::object();
        const bool ok = w.startsWith("9.") || w.startsWith("10.") || (w.startsWith("11.") && w.experimental);
        result.set("ok", Json::flag(ok));
        if (!ok) result.set("reason", Json::text("not tried on webOS " + w.webos));
        return result;
    };
    d.mounted = [&w]() { return w.mounted; };
    d.escaped = [&w]() { return w.escaped; };
    d.startLayoutAvailable = [&w]() { return w.startsWith("9."); };
    d.blankKept = [&w]() { return w.blankKept; };
    d.takeover = [&w](bool blank) {
        w.ev(blank ? "take:blank" : "take");
        Json result = Json::object();
        result.set("ok", Json::flag(w.takeoverOk));
        if (!w.takeoverOk) result.set("error", Json::text("refused"));
        else w.mounted = true;
        return result;
    };
    d.disable = [&w]() {
        w.ev("release");
        w.recording = false;
        w.mounted = false;
        Json result = Json::object();
        result.set("ok", Json::flag(true));
        return result;
    };
    d.startingNotice = [&w](bool on) {
        w.ev(on ? "notice:on" : "notice:off");
        return w.noticeOk;
    };
    d.syncAutoLaunch = [&w](bool escaped) {
        w.ev(std::string("sync:") + (escaped ? "true" : "false"));
        if (w.afterSync) w.afterSync();
        Json result = Json::object();
        result.set("ok", Json::flag(true));
        return result;
    };
    d.launch = [&w](const std::string &id) {
        w.ev("launch:" + id);
        return true;
    };
    d.powerOnReason = [&w]() {
        w.ev("reason");
        return w.powerOnReason;
    };
    d.readSettings = [&w](const Json &keys) {
        std::string list;
        for (const Json &key : keys.items()) list += (list.empty() ? "" : ",") + key.string();
        w.ev("read:" + list);
        if (w.settingsUnreadable) return Json();
        Json reply = Json::object();
        for (const Json &key : keys.items())
            if (w.settings.has(key.string()) && w.settings[key.string()].valid())
                reply.set(key.string(), w.settings[key.string()]);
        return reply;
    };
    d.writeSettings = [&w](const Json &settings) {
        w.ev("write:" + canonical(settings));
        if (!w.writeOk) return false;
        for (const std::string &key : settings.keys()) w.settings.set(key, settings[key]);
        return true;
    };
    d.homeKeyApp = [&w]() { return w.homeKey; };
    d.clearHomeKey = [&w]() {
        w.ev("homekey:clear");
        if (!w.homeKeyClearOk) return false;
        w.homeKey.clear();
        return true;
    };
    d.legacyAppInstalled = [&w]() {
        w.ev("legacy:installed?");
        return w.legacyInstalled;
    };
    d.legacyAppRunning = [&w]() {
        w.ev("legacy:running?");
        return w.legacyRunning ? 1 : 0;
    };
    d.killLegacyApp = [&w]() {
        w.ev("legacy:kill");
        if (!w.legacyKillFails) w.legacyRunning = false;
    };
    d.removeLegacyApp = [&w]() {
        w.ev("legacy:remove");
        w.legacyInstalled = 0;
    };
    if (w.watch) {
        d.nowMs = [&w]() { return w.clock.now; };
        d.clientRunning = [&w]() {
            w.ev("running?");
            return w.clientListed;
        };
        d.release = [&w]() {
            w.ev("unmount");
            Json result = Json::object();
            result.set("ok", Json::flag(w.releaseOk));
            if (w.releaseOk) w.mounted = false;
            else result.set("error", Json::text("sam would not"));
            return result;
        };
    }
    d.after = [&w](long long delay, std::function<void()> task) { return w.clock.after(delay, std::move(task)); };
    d.cancel = [&w](unsigned long long id) { w.clock.cancel(id); };
    d.blocking = [](std::function<void()> work) { work(); };
    d.log = [&w](const std::string &line) { w.notes.push_back(line); };
    d.logError = [&w](const std::string &line) { w.errors.push_back(line); };
    return d;
}

void load(World &w, const Json &scenario) {
    if (scenario["config"].isObject()) w.raw = copyOf(scenario["config"]);
    if (scenario["settings"].isObject()) w.settings = copyOf(scenario["settings"]);
    if (scenario["webos"].isString()) w.webos = scenario["webos"].string();
    w.experimental = scenario["experimental"].boolean();
    const Json tv = scenario["tv"];
    w.mounted = tv["mounted"].boolean();
    w.blankKept = tv["blankKept"].boolean();
    if (tv["takeoverOk"].isBoolean()) w.takeoverOk = tv["takeoverOk"].boolean();
    if (tv["noticeOk"].isBoolean()) w.noticeOk = tv["noticeOk"].boolean();
    if (tv["writeOk"].isBoolean()) w.writeOk = tv["writeOk"].boolean();
    w.settingsUnreadable = tv["settingsUnreadable"].boolean();
    w.homeKey = tv["homeKey"].string();
    if (tv["homeKeyClearOk"].isBoolean()) w.homeKeyClearOk = tv["homeKeyClearOk"].boolean();
    if (tv["powerOnReason"].isString()) w.powerOnReason = tv["powerOnReason"].string();
    const Json legacy = tv["legacy"];
    if (legacy.isObject()) {
        w.legacyInstalled = !legacy["installed"].isBoolean() ? -1 : legacy["installed"].boolean() ? 1 : 0;
        w.legacyRunning = legacy["running"].boolean();
        w.legacyKillFails = legacy["killFails"].boolean();
    }
}

void scenarios(const std::string &path) {
    const Json fixture = Json::parse(readFile(path));
    check(fixture["scenarios"].isArray() && fixture["scenarios"].size() > 30, "the fixture lists its scenarios");
    for (const Json &scenario : fixture["scenarios"].items()) {
        const std::string name = scenario["name"].string();
        World w;
        load(w, scenario);
        HomeOrchestrator orchestrator(depsFor(w));
        for (const Json &step : scenario["steps"].items()) {
            if (step.has("wait")) w.clock.advance(static_cast<long long>(step["wait"].number()));
            else if (step.has("start")) orchestrator.start();
            else if (step.has("foreground")) orchestrator.onForeground(step["foreground"].string());
            else if (step.has("power")) orchestrator.onPowerState(step["power"].string());
            else if (step.has("screen")) orchestrator.onScreen(step["screen"].boolean());
            else if (step.has("escaped")) w.escaped = step["escaped"].boolean();
            else if (step.has("mounted")) w.mounted = step["mounted"].boolean();
        }
        std::vector<std::string> expected;
        for (const Json &event : scenario["expect"]["events"].items()) expected.push_back(event.string());
        check(w.events == expected,
              name + ": the calls differ\n  node:   " + joined(expected) + "\n  native: " + joined(w.events));
        const Json config = scenario["expect"]["config"];
        if (config.isObject()) {
            const Json saved = w.document();
            const bool hideSet = saved["hideLgHomeAtStart"].isBoolean();
            check(config["hideLgHomeAtStart"].isBoolean() == hideSet &&
                      (!hideSet || config["hideLgHomeAtStart"].boolean() == saved["hideLgHomeAtStart"].boolean()),
                  name + ": hideLgHomeAtStart differs");
            check(config["snapshot"].boolean() == saved["snapshot"].isObject(), name + ": the snapshot differs");
        }
    }
}

void failingSwitchOffIsReported() {
    World w;
    w.raw.set("enabled", Json::flag(false));
    w.raw.set("restorePending", Json::flag(true));
    HomeOrchestratorDeps d = depsFor(w);
    d.disable = [&w]() {
        w.ev("release");
        Json result = Json::object();
        result.set("ok", Json::flag(false));
        result.set("error", Json::text("the TV would not take its earlier Home settings back"));
        return result;
    };
    HomeOrchestrator orchestrator(d);
    orchestrator.start();
    check(w.errors.size() == 1 && w.errors[0].find("earlier Home settings") != std::string::npos,
          "a switch-off that fails is logged with its reason");
    check(w.events.back() == "release", "and nothing follows it");
}

void aLostInferredSaveStillStarts() {
    World w;
    w.raw.set("enabled", Json::flag(true));
    w.blankKept = true;
    HomeOrchestratorDeps d = depsFor(w);
    d.update = [](const std::function<void(Json &)> &, std::string &error) {
        error = "disk full";
        return false;
    };
    HomeOrchestrator orchestrator(d);
    orchestrator.start();
    check(w.errors.size() == 1 && w.errors[0].find("inferred") != std::string::npos,
          "the save failure is logged");
    check(std::find(w.events.begin(), w.events.end(), "take:blank") != w.events.end(),
          "and the start goes on with the layout it inferred");
}

// Asked for while one runs, a second sync runs once more afterwards, not beside it.
void syncsAreCoalesced() {
    World w;
    w.raw.set("enabled", Json::flag(true));
    HomeOrchestrator orchestrator(depsFor(w));
    bool first = true;
    w.afterSync = [&]() {
        if (!first) return;
        first = false;
        orchestrator.onScreen(false);
        orchestrator.onScreen(true);
        orchestrator.onScreen(false);
    };
    orchestrator.start();
    size_t syncs = 0;
    for (const std::string &event : w.events)
        if (event.compare(0, 5, "sync:") == 0) ++syncs;
    check(syncs == 2, "three requests during a sync cost one more sync, not three: " + joined(w.events));
}

void stopCancelsTheWake() {
    World w;
    w.raw.set("enabled", Json::flag(true));
    w.raw.set("openOnPowerOn", Json::flag(true));
    w.mounted = true;
    HomeOrchestrator orchestrator(depsFor(w));
    orchestrator.onForeground("com.webos.app.hdmi1");
    orchestrator.onPowerState("Active");
    orchestrator.onPowerState("Active Standby");
    orchestrator.onPowerState("Active");
    check(w.clock.timers.size() == 1, "the wake waits for its timer");
    orchestrator.stop();
    w.clock.advance(5000);
    check(w.events.empty(), "a stopped orchestrator opens nothing");
}

// A wake whose timer is already in flight when the TV goes back to standby opens nothing.
void aLateTimerIsIgnored() {
    World w;
    w.raw.set("enabled", Json::flag(true));
    w.raw.set("openOnPowerOn", Json::flag(true));
    w.mounted = true;
    HomeOrchestrator orchestrator(depsFor(w));
    orchestrator.onForeground("com.webos.app.hdmi1");
    orchestrator.onPowerState("Active");
    orchestrator.onPowerState("Active Standby");
    orchestrator.onPowerState("Active");
    std::function<void()> due = w.clock.timers.begin()->second.second;
    orchestrator.onPowerState("Active Standby");
    due();
    check(w.events.empty(), "a timer that fires after the wake was cancelled does nothing");
}

// ---------------------------------------------------------------- the native client

const char *const kClient = "com.tvweb.native";
const char *const kLgHome = "com.webos.app.home";

bool happened(const World &w, const std::string &event) {
    return std::find(w.events.begin(), w.events.end(), event) != w.events.end();
}

size_t times(const World &w, const std::string &event) {
    return static_cast<size_t>(std::count(w.events.begin(), w.events.end(), event));
}

World watched() {
    World w;
    w.watch = true;
    w.raw.set("enabled", Json::flag(true));
    w.mounted = true;
    return w;
}

// The client in front, then dying: LG's Home (the stub) comes to the front and
// the app manager no longer lists the client.
void crash(World &w, HomeOrchestrator &orchestrator) {
    w.clientListed = 1;
    orchestrator.onForeground(kClient);
    w.clientListed = 0;
    orchestrator.onForeground(kLgHome);
    // The stub starts it again.
    orchestrator.onForeground(kClient);
    w.clock.advance(3000);
}

void threeCrashesGiveLgHomeBack() {
    World w = watched();
    HomeOrchestrator orchestrator(depsFor(w));
    crash(w, orchestrator);
    crash(w, orchestrator);
    check(!orchestrator.halted() && !happened(w, "unmount"), "two crashes in a minute change nothing");
    check(orchestrator.nativeStatus()["crashes"]["recentExits"].number() == 2, "but are counted");
    crash(w, orchestrator);
    check(orchestrator.halted(), "the third gives LG's Home back");
    check(times(w, "unmount") == 1, "once: " + joined(w.events));
    check(happened(w, "notice:off"), "the Home key's starting notice is cleared");
    check(happened(w, "sync:false"), "and LG's auto launch is put back in line");
    check(w.errors.size() == 1 && w.errors[0].find("stopped 3 times within 120 seconds") != std::string::npos &&
              w.errors[0].find("until nativeTv restarts or Home is switched on again") != std::string::npos,
          "the log says why: " + joined(w.errors));
    check(!w.mounted && w.document()["enabled"].boolean(),
          "LG's Home is back and Home is still switched on, for the next start");
    const Json status = orchestrator.nativeStatus();
    check(status["halted"].boolean() && status["cause"].string() == "crashLoop" &&
              status["reason"].string().find("stopped 3 times") != std::string::npos,
          "the status says so: " + status.dump());

    // No retake: more crashes, a stub left in front, and a wake do nothing.
    const size_t before = w.events.size();
    crash(w, orchestrator);
    orchestrator.onForeground(kLgHome);
    w.clock.advance(60000);
    check(w.events.size() == before && w.errors.size() == 1, "nothing follows: " + joined(w.events));
    w.raw.set("openOnPowerOn", Json::flag(true));
    orchestrator.onForeground("com.webos.app.hdmi1");
    orchestrator.onPowerState("Active");
    orchestrator.onPowerState("Active Standby");
    orchestrator.onPowerState("Active");
    w.clock.advance(5000);
    check(!happened(w, "launch:com.webos.app.home") && !happened(w, "take"),
          "a wake does not open it or take it again");
}

void crashesBeyondTheWindowDoNotAdd() {
    World w = watched();
    HomeOrchestrator orchestrator(depsFor(w));
    crash(w, orchestrator);              // at 0 s
    w.clock.advance(60000);
    crash(w, orchestrator);              // at about 63 s
    w.clock.advance(60000);
    crash(w, orchestrator);              // at about 126 s: the first is out of the window
    check(!orchestrator.halted() && !happened(w, "unmount"), "three in four minutes do not trip it");
    check(orchestrator.nativeStatus()["crashes"]["recentExits"].number() == 2, "two are in the window");
    w.clock.advance(10000);
    crash(w, orchestrator);
    check(orchestrator.halted(), "the next one, three within two minutes, does");
}

void leavingOnPurposeIsNotACrash() {
    World w = watched();
    HomeOrchestrator orchestrator(depsFor(w));
    for (int press = 0; press < 5; ++press) {
        // The Home key: LG's Home (the stub) over the client, which stays listed.
        w.clientListed = 1;
        orchestrator.onForeground(kClient);
        orchestrator.onForeground(kLgHome);
        w.clock.advance(1500);
        orchestrator.onForeground(kClient);
        w.clock.advance(3000);
    }
    // Another app opened from Home: the client is closed after it.
    for (int open = 0; open < 5; ++open) {
        w.clientListed = 0;
        orchestrator.onForeground(kClient);
        orchestrator.onForeground("com.webos.app.netflix");
        w.clock.advance(5000);
        orchestrator.onForeground("com.webos.app.hdmi1");
    }
    check(!orchestrator.halted() && orchestrator.nativeStatus()["crashes"]["recentExits"].number() == 0,
          "neither is counted: " + joined(w.events));
    w.clientListed = -1;
    orchestrator.onForeground(kClient);
    orchestrator.onForeground(kLgHome);
    w.clock.advance(5000);
    check(orchestrator.nativeStatus()["crashes"]["recentExits"].number() == 0,
          "a running list that cannot be read is not a crash");
}

void aStubLeftInFrontIsACrash() {
    World w = watched();
    HomeOrchestrator orchestrator(depsFor(w));
    // The launch is refused, or the client dies before it reaches the front.
    orchestrator.onForeground(kLgHome);
    w.clock.advance(14999);
    check(orchestrator.nativeStatus()["crashes"]["recentExits"].number() == 0, "not before the grace");
    w.clock.advance(1);
    check(orchestrator.nativeStatus()["crashes"]["recentExits"].number() == 1, "one a grace later");
    w.clock.advance(15000);
    check(!orchestrator.halted() && orchestrator.nativeStatus()["crashes"]["recentExits"].number() == 2,
          "and one more");
    w.clock.advance(15000);
    check(orchestrator.halted() && happened(w, "unmount"), "the third, 45 s in, gives LG's Home back");
    check(w.clock.timers.empty(), "and nothing is left waiting");
}

void aClientThatComesUpIsNotACrash() {
    World w = watched();
    HomeOrchestrator orchestrator(depsFor(w));
    orchestrator.onForeground(kLgHome);
    w.clock.advance(8000);
    orchestrator.onForeground(kClient);
    w.clock.advance(120000);
    check(orchestrator.nativeStatus()["crashes"]["recentExits"].number() == 0 && w.clock.timers.empty(),
          "the stub's grace ends when the client is in front");
    // A cold start slower than most is still inside the grace.
    orchestrator.onForeground(kLgHome);
    w.clock.advance(14000);
    orchestrator.onForeground(kClient);
    w.clock.advance(120000);
    check(!orchestrator.halted(), "14 s is not a crash");
}

void standbyAndEscapesAreNotWatched() {
    {
        World w = watched();
        HomeOrchestrator orchestrator(depsFor(w));
        orchestrator.onPowerState("Active");
        w.clientListed = 1;
        orchestrator.onForeground(kClient);
        orchestrator.onPowerState("Active Standby");
        w.clientListed = 0;
        orchestrator.onForeground("");           // the standby closes the client
        orchestrator.onForeground(kLgHome);
        w.clock.advance(60000);
        check(orchestrator.nativeStatus()["crashes"]["recentExits"].number() == 0 && !orchestrator.halted(),
              "a client closed by a standby, and a stub in front through it, are not a crash");
        orchestrator.onPowerState("Active");
        orchestrator.onForeground(kClient);
        orchestrator.onForeground(kLgHome);
        check(orchestrator.nativeStatus()["crashes"]["recentExits"].number() == 1, "and the watch is back with the TV");
    }
    {
        World w = watched();
        HomeOrchestrator orchestrator(depsFor(w));
        orchestrator.onScreen(false);
        orchestrator.onForeground(kLgHome);
        w.clock.advance(60000);
        check(!orchestrator.halted() && orchestrator.nativeStatus()["crashes"]["recentExits"].number() == 0,
              "or one with the screen off");
        orchestrator.onScreen(true);
        orchestrator.onForeground(kClient);
        orchestrator.onForeground(kLgHome);
        w.clock.advance(15000);
        check(orchestrator.nativeStatus()["crashes"]["recentExits"].number() >= 1, "and the watch is back with the screen");
    }
    {
        World w = watched();
        w.escaped = true;
        HomeOrchestrator orchestrator(depsFor(w));
        orchestrator.onForeground(kLgHome);
        w.clock.advance(60000);
        check(orchestrator.nativeStatus()["crashes"]["recentExits"].number() == 0,
              "LG's Home opened on purpose is LG's Home");
    }
    {
        World w = watched();
        w.mounted = false;
        HomeOrchestrator orchestrator(depsFor(w));
        orchestrator.onForeground(kLgHome);
        w.clock.advance(60000);
        check(orchestrator.nativeStatus()["crashes"]["recentExits"].number() == 0,
              "as is LG's Home without the takeover in place");
    }
    {
        World w = watched();
        w.raw.set("enabled", Json::flag(false));
        HomeOrchestrator orchestrator(depsFor(w));
        orchestrator.onForeground(kLgHome);
        w.clock.advance(60000);
        check(orchestrator.nativeStatus()["crashes"]["recentExits"].number() == 0, "or with Home off");
    }
}

void rearmingStartsOver() {
    World w = watched();
    HomeOrchestrator orchestrator(depsFor(w));
    crash(w, orchestrator);
    crash(w, orchestrator);
    crash(w, orchestrator);
    check(orchestrator.halted(), "tripped");
    // Home is switched on again: the takeover is made again and the watch starts over.
    w.mounted = true;
    orchestrator.rearm();
    check(!orchestrator.halted() && orchestrator.nativeStatus()["crashes"]["recentExits"].number() == 0,
          "rearmed");
    crash(w, orchestrator);
    crash(w, orchestrator);
    check(!orchestrator.halted(), "two crashes after it are two");
    crash(w, orchestrator);
    check(orchestrator.halted() && times(w, "unmount") == 2, "and the third trips it again");
}

void aFailedReleaseIsReported() {
    World w = watched();
    w.releaseOk = false;
    HomeOrchestrator orchestrator(depsFor(w));
    crash(w, orchestrator);
    crash(w, orchestrator);
    crash(w, orchestrator);
    check(orchestrator.halted() && w.errors.size() == 2 &&
              w.errors[1].find("could not be put back: sam would not") != std::string::npos,
          "the refusal is logged: " + joined(w.errors));
}

void aStoppedWatchWaitsForNothing() {
    World w = watched();
    HomeOrchestrator orchestrator(depsFor(w));
    orchestrator.onForeground(kLgHome);
    w.clientListed = 1;
    orchestrator.onForeground(kClient);
    w.clientListed = 0;
    orchestrator.onForeground(kLgHome);
    check(!w.clock.timers.empty(), "timers wait");
    orchestrator.stop();
    check(w.clock.timers.empty(), "stop() cancels them, so none fires into a destroyed orchestrator");
}

void theProbeKeepsLgHomeAtStart() {
    {
        World w = watched();
        w.probeRefusal = Json::parse(
            R"({"ok":false,"webos":"10.3.2","reason":"This TV's graphics driver cannot run the nativeTv app (check exit 12).","probe":{"ok":false,"exit":12,"class":"gl"}})");
        HomeOrchestrator orchestrator(depsFor(w));
        orchestrator.start();
        check(!happened(w, "release") && !happened(w, "take") && !happened(w, "take:blank"),
              "the probe's refusal neither switches Home off nor takes the place: " + joined(w.events));
        check(happened(w, "unmount") && happened(w, "notice:off") && happened(w, "sync:false"),
              "it gives LG's Home the place, clears the notice and puts auto launch back: " + joined(w.events));
        check(w.document()["enabled"].boolean(), "Home stays switched on, for the next start");
        const Json status = orchestrator.nativeStatus();
        check(status["halted"].boolean() && status["cause"].string() == "probe" &&
                  status["reason"].string().find("graphics driver") != std::string::npos,
              "the status names the probe: " + status.dump());
        check(w.errors.size() == 1 && w.errors[0].find("check exit 12") != std::string::npos,
              "and so does the log");
        orchestrator.rearm();
        check(!orchestrator.halted(), "switching Home on again clears it");
    }
    {
        World w = watched();
        w.mounted = false;
        w.probeRefusal = Json::parse(R"({"ok":false,"reason":"not installed","probe":{"ok":false,"class":"missing"}})");
        HomeOrchestrator orchestrator(depsFor(w));
        orchestrator.start();
        check(!happened(w, "unmount") && !happened(w, "take") && happened(w, "notice:off") &&
                  orchestrator.halted(),
              "with LG's Home already in place there is nothing to give back: " + joined(w.events));
    }
    {
        World w = watched();
        w.raw.set("enabled", Json::flag(false));
        w.mounted = false;
        w.probeRefusal = Json::parse(R"({"ok":false,"reason":"x","probe":{"ok":false,"class":"gl"}})");
        HomeOrchestrator orchestrator(depsFor(w));
        orchestrator.start();
        check(!orchestrator.halted() && !happened(w, "unmount"), "a Home that is off is not held back by a probe");
    }
    {
        World w = watched();
        w.webos = "4.4.0";
        HomeOrchestrator orchestrator(depsFor(w));
        orchestrator.start();
        check(happened(w, "release") && !orchestrator.halted(), "an unsupported version still switches Home off");
    }
}
} // namespace

int main(int argc, char **argv) {
    try {
        check(argc > 1, "the fixture path is expected");
        scenarios(argv[1]);
        failingSwitchOffIsReported();
        aLostInferredSaveStillStarts();
        syncsAreCoalesced();
        stopCancelsTheWake();
        aLateTimerIsIgnored();
        threeCrashesGiveLgHomeBack();
        crashesBeyondTheWindowDoNotAdd();
        leavingOnPurposeIsNotACrash();
        aStubLeftInFrontIsACrash();
        aClientThatComesUpIsNotACrash();
        standbyAndEscapesAreNotWatched();
        rearmingStartsOver();
        aFailedReleaseIsReported();
        aStoppedWatchWaitsForNothing();
        theProbeKeepsLgHomeAtStart();
    } catch (const std::exception &error) {
        std::fprintf(stderr, "home orchestrator test failed: %s\n", error.what());
        return 1;
    }
    std::puts("home orchestrator tests passed");
    return 0;
}
