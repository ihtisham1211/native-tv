#include "daemon/home_orchestrator.hpp"
#include "daemon/home_probe_gate.hpp"
#include <algorithm>
#include "domain/names.hpp"
#include "domain/say.hpp"
#include <cctype>
#include <cstring>

namespace nativetv {
namespace daemon {
namespace {
const char *const kLegacyApp = "com.tvweb.home";
const char *const kLgHome = "com.webos.app.home";
const char *const kLiveTv = "com.webos.app.livetv";
// After a wake, how long bootd is given to put the last input in front.
const long long kWakeSettleMs = 1500;
// The power-on reasons bootd opens Home for while auto launch is on: LG's
// com.webos.service.tvpower.fullHomeBootingPowerOnReasons on a G5 (webOS 10.3.1).
const char *const kFullHomeReasons[] = {"aiKey", "lastPowerOn", "localKey", "remoteKey", "seniorHelp"};
// The settings earlier builds changed, as their snapshot kept them.
const char *const kLegacyInputKeys[] = {"lastInputApp", "physicalLastInputApp"};

bool isInputApp(const std::string &id) {
    static const std::string prefix = "com.webos.app.";
    if (id.compare(0, prefix.size(), prefix) != 0) return false;
    const std::string rest = id.substr(prefix.size());
    const auto digitAfter = [&rest](const char *word) {
        const size_t length = std::strlen(word);
        return rest.compare(0, length, word) == 0 && rest.size() > length &&
               std::isdigit(static_cast<unsigned char>(rest[length]));
    };
    return digitAfter("hdmi") || rest.compare(0, 6, "livetv") == 0 || digitAfter("av") ||
           digitAfter("comp");
}

bool systemOn(const std::string &raw) {
    return names::powerState(Json::text(raw))["systemOn"].boolean();
}

bool fullHomeReason(const std::string &reason) {
    for (const char *known : kFullHomeReasons)
        if (reason == known) return true;
    return false;
}

Json failure(const std::string &error) {
    Json result = Json::object();
    result.set("ok", Json::flag(false));
    result.set("error", Json::text(error));
    return result;
}

bool truthyString(const Json &value) { return value.isString() && !value.string().empty(); }
} // namespace

HomeOrchestrator::HomeOrchestrator(HomeOrchestratorDeps deps)
    : deps_(std::move(deps)), breaker_(deps_.watch.exitLimit, deps_.watch.windowMs) {}

HomeOrchestrator::~HomeOrchestrator() { stop(); }

void HomeOrchestrator::say(const std::string &line) const {
    if (deps_.log) deps_.log(line);
}

void HomeOrchestrator::sayError(const std::string &line) const {
    if (deps_.logError) deps_.logError(line);
}

void HomeOrchestrator::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_ = true;
    cancelWakeLocked();
    if (preloadHoldTimer_ && deps_.cancel) deps_.cancel(preloadHoldTimer_);
    preloadHoldTimer_ = 0;
}

void HomeOrchestrator::cancelWakeLocked() {
    if (wakeTimer_ && deps_.cancel) deps_.cancel(wakeTimer_);
    wakeTimer_ = 0;
    wakeActive_ = false;
}

// ---------------------------------------------------------------- start

void HomeOrchestrator::start() {
    // First, whatever follows: Home can be switched on later from the dashboard.
    armPreloadHold();
    if (!cleanLegacy()) return;
    Json c = deps_.config();
    // Saved only where it differs from the default, so a start writes nothing otherwise.
    if (!c["hideLgHomeAtStart"].isBoolean() && deps_.blankKept()) {
        c = Json::parse(c.dump());
        c.set("hideLgHomeAtStart", Json::flag(true));
        std::string error;
        if (!deps_.update([](Json &document) { document.set("hideLgHomeAtStart", Json::flag(true)); },
                          error))
            sayError("home: could not save the inferred LG Home start layout");
    }

    const bool enabled = c["enabled"].boolean();
    /*
     * Switched on where it is not offered: a webOS 10 TV updated to webOS 11,
     * say, or allowExperimentalHome taken out. LG's Home is put back rather
     * than taken on a version it was never tried on.
     */
    const Json supported = deps_.supported();
    if (enabled && !supported["ok"].boolean() && supported["probe"].isObject()) {
        // The version is one it was tried on, the native client is not: Home
        // stays switched on, for the next start or switching it on again, and
        // LG's is not given up to a client that cannot draw.
        giveBack("client", supported["reason"].string());
        return;
    }
    if (enabled && !supported["ok"].boolean()) {
        say("home: not offered on this TV (" + supported["reason"].string() + "); switched off");
        const Json result = deps_.disable();
        if (!result["ok"].boolean()) sayError("home: " + result["error"].string());
        return;
    }
    if (c["restorePending"].boolean() || (!enabled && deps_.mounted())) {
        const Json result = deps_.disable();
        if (!result["ok"].boolean()) sayError("home: " + result["error"].string());
        return;
    }
    if (!deps_.startingNotice(enabled))
        sayError(std::string("home: could not ") + (enabled ? "set" : "clear") +
                 " the Home key's starting notice");
    if (!enabled) return;

    const bool blank = deps_.startLayoutAvailable() && c["hideLgHomeAtStart"].boolean();
    Json result = deps_.takeover(blank);
    if (result["ok"].boolean() && deps_.config()["enabled"].boolean()) {
        if (!deps_.startingNotice(true)) {
            sayError("home: could not set the Home key's starting notice");
            result = failure("the TV did not take the Home key's starting notice");
        }
    }
    if (result["ok"].boolean()) say("home: Native Tv Home is in LG's Home's place");
    else sayError("home: could not take LG's Home's place: " + result["error"].string());
    syncAutoLaunch();
}

// ---------------------------------------------------------------- what earlier builds left

/*
 * Earlier builds set defaultApps.home and the start-up app to their own app,
 * and the remote's Home key to open it. Each that still names it is put back:
 * the start-up app as the snapshot had it, or Live TV.
 */
bool HomeOrchestrator::cleanLegacy() {
    const Json c = deps_.config();
    const Json snapshot = c["snapshot"].isObject() ? c["snapshot"]["settings"] : Json();
    const Json snap = snapshot.isObject() ? snapshot : Json::object();

    // An earlier build switched the screen saver off while the view was in
    // front. It is put back once, as the snapshot had it.
    const Json savedSaver = snap["screenSaverEnabled"];
    if (savedSaver.isBoolean() || savedSaver.isString() || savedSaver.isNumber()) {
        Json put = Json::object();
        put.set("screenSaverEnabled", savedSaver);
        if (!deps_.writeSettings(put)) {
            sayError("home: could not put the screen saver setting back");
            return false;
        }
        say("home: screen saver setting put back as " +
            (savedSaver.isBoolean() ? std::string(savedSaver.boolean() ? "true" : "false")
                                    : savedSaver.string()));
    }

    Json keys = Json::array();
    keys.add(Json::text("defaultApps"));
    for (const char *key : kLegacyInputKeys) keys.add(Json::text(key));
    const Json current = deps_.readSettings(keys);
    if (!current.isObject()) return false;

    Json put = Json::object();
    const Json defaults = current["defaultApps"];
    if (defaults.isObject() &&
        (defaults["home"].string() == kLegacyApp || defaults["idleApp"].string() == kLegacyApp)) {
        Json apps = Json::object();
        for (const std::string &key : defaults.keys()) {
            if ((key == "home" || key == "idleApp") && defaults[key].string() == kLegacyApp) {
                const Json saved = snap["defaultApps"][key];
                if (truthyString(saved) && saved.string() != kLegacyApp) apps.set(key, saved);
                continue;
            }
            apps.set(key, defaults[key]);
        }
        put.set("defaultApps", apps);
    }
    for (const char *key : kLegacyInputKeys) {
        if (current[key].string() != kLegacyApp) continue;
        const Json saved = snap[key];
        put.set(key, truthyString(saved) && saved.string() != kLegacyApp ? saved : Json::text(kLiveTv));
    }
    if (put.keys().empty()) return finishLegacy();

    const bool ok = deps_.writeSettings(put);
    std::string changed;
    for (const std::string &key : put.keys()) changed += (changed.empty() ? "" : ", ") + key;
    say(ok ? "home: settings an earlier build changed put back (" + changed + ")"
           : "home: could not put back the settings an earlier build changed");
    return ok && finishLegacy();
}

bool HomeOrchestrator::finishLegacy() {
    const bool hadSnapshot = deps_.config()["snapshot"].isObject();
    if (deps_.homeKeyApp() == kLegacyApp && !deps_.clearHomeKey()) {
        sayError("home: the Home key still opens the old Native Tv Home app");
        return false;
    }
    if (hadSnapshot) {
        std::string error;
        if (!deps_.update([](Json &document) { document.set("snapshot", Json()); }, error))
            return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        legacyPending_ = true;
    }
    // Not waited for: the removal waits for the old app to be closed.
    deps_.blocking([this]() { tryRemoveLegacyApp(); });
    return true;
}

/*
 * The old app, once nothing has it open. After a cold start the TV could have
 * opened it through its input app (com.webos.app.inputcommon), which sam then
 * names as its process and cannot close, and a removal waits on "app closing"
 * with the app locked until the TV restarts (a G3, webOS 9.2).
 */
void HomeOrchestrator::tryRemoveLegacyApp() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!legacyPending_ || legacyRemoving_ || !deps_.removeLegacyApp) return;
        if (foreground_ == kLegacyApp) return;
        legacyRemoving_ = true;
    }
    const auto finish = [this]() {
        std::lock_guard<std::mutex> lock(mutex_);
        legacyRemoving_ = false;
    };
    const int installed = deps_.legacyAppInstalled();
    if (installed != 1) {
        if (installed == 0) {
            std::lock_guard<std::mutex> lock(mutex_);
            legacyPending_ = false;
        }
        finish();
        return;
    }
    // Its appinfo keeps it running in the background, so on a G3 it stayed
    // until the TV restarted. Its page is closed through WebAppMgr, and the
    // removal still waits until sam no longer lists it.
    int running = deps_.legacyAppRunning();
    if (running == 1) {
        deps_.killLegacyApp();
        running = deps_.legacyAppRunning();
    }
    if (running == 0) removeLegacyApp();
    finish();
}

void HomeOrchestrator::removeLegacyApp() {
    if (deps_.legacyAppInstalled() != 1) return;
    deps_.removeLegacyApp();
    if (deps_.legacyAppInstalled() == 0) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            legacyPending_ = false;
        }
        say("home: the old Native Tv Home app removed");
    }
}

// ---------------------------------------------------------------- the live state

void HomeOrchestrator::onForeground(const std::string &appId) {
    bool pending;
    std::string was;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        was = foreground_;
        foreground_ = appId;
        if (wakeActive_ && !appId.empty() && appId != was) {
            if (wakeInput_.empty() && isInputApp(appId)) wakeInput_ = appId;
            else if (wakeInput_ != appId) cancelWakeLocked();
        }
        pending = legacyPending_;
    }
    if (pending && deps_.blocking) deps_.blocking([this]() { tryRemoveLegacyApp(); });
    if (was != appId) watchForeground(was, appId);
}

void HomeOrchestrator::onScreen(bool active) {
    bool was;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        was = powerActive_;
        powerActive_ = active;
        if (!active) cancelWakeLocked();
    }
    if (was != active && deps_.config()["enabled"].boolean())
        deps_.blocking([this]() { syncAutoLaunch(); });
    if (!was && active && deps_.holdLgHomePreload && deps_.blocking)
        deps_.blocking([this]() { resendPreloadHold(); });
}

// ---------------------------------------------------------------- the preload manager

/*
 * The preload manager launched LG's Home three times in 90 s while an app
 * played a 4K stream on a B4 (webOS 10.3.2), and the third brought Home in
 * front of the app: Native Tv Home, now it holds LG's Home's id. With the
 * policy off it launched none in nine minutes (measured by upstream
 * Glasshouse; not on a TV of ours). The takeover sets the policy; this sends
 * it again, since the preload manager forgets it when it restarts.
 */
void HomeOrchestrator::armPreloadHold() {
    if (!deps_.holdLgHomePreload || !deps_.after || !deps_.blocking) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_) return;
    if (preloadHoldTimer_ && deps_.cancel) deps_.cancel(preloadHoldTimer_);
    preloadHoldTimer_ = deps_.after(deps_.preloadHoldMs, [this]() {
        deps_.blocking([this]() {
            resendPreloadHold();
            armPreloadHold();
        });
    });
}

void HomeOrchestrator::resendPreloadHold() {
    if (!deps_.holdLgHomePreload || !mayWatch()) return;
    const bool taken = deps_.holdLgHomePreload();
    bool report;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Refused every time on a C5 (webOS 11.2.0), where it is asked again each
        // minute: said once.
        report = !taken && !preloadRefusalLogged_;
        preloadRefusalLogged_ = !taken;
    }
    if (report) sayError("home: the preload manager did not take LG's Home off");
}

/*
 * Switched back on before the TV had powered off, it wakes from Active Standby
 * without a boot, so the boot hook does not run, and with auto launch held off
 * bootd opens the last input (a G3, webOS 9.2.2, off and on 6 s apart). Where
 * the owner had auto launch on, Native Tv Home is opened over that input, as
 * bootd would have opened it.
 */
void HomeOrchestrator::onPowerState(const std::string &raw) {
    std::string was;
    bool hadState;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        hadState = havePowerState_;
        was = powerState_;
        if (hadState && was == raw) return;
        havePowerState_ = true;
        powerState_ = raw;
        cancelWakeLocked();
    }
    if (!hadState || systemOn(was) || !systemOn(raw)) return;
    const Json c = deps_.config();
    if (!c["enabled"].boolean() ||
        (!c["openOnPowerOn"].boolean() && c["lgAutoLaunch"].string() != "on"))
        return;
    std::lock_guard<std::mutex> lock(mutex_);
    const unsigned long long token = ++wakeToken_;
    wakeActive_ = true;
    wakeInput_ = isInputApp(foreground_) ? foreground_ : std::string();
    wakeTimer_ = deps_.after(kWakeSettleMs, [this, token]() {
        deps_.blocking([this, token]() { fireWake(token); });
    });
}

bool HomeOrchestrator::wakeMayOpen(unsigned long long token) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!wakeActive_ || wakeToken_ != token) return false;
        if (!systemOn(powerState_) || !powerActive_) return false;
        if (!isInputApp(foreground_) || foreground_ != wakeInput_) return false;
    }
    const Json current = deps_.config();
    return current["enabled"].boolean() &&
           (current["openOnPowerOn"].boolean() || current["lgAutoLaunch"].string() == "on") &&
           !deps_.escaped() && deps_.mounted();
}

void HomeOrchestrator::fireWake(unsigned long long token) {
    const auto stillWanted = [this, token]() {
        std::lock_guard<std::mutex> lock(mutex_);
        return wakeActive_ && wakeToken_ == token;
    };
    const auto cancelIfWanted = [this, token]() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (wakeActive_ && wakeToken_ == token) cancelWakeLocked();
    };
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!wakeActive_ || wakeToken_ != token) return;
        wakeTimer_ = 0;
    }
    if (!wakeMayOpen(token)) return cancelIfWanted();
    const std::string reason = deps_.powerOnReason();
    if (!stillWanted()) return;
    if (!fullHomeReason(reason) || !wakeMayOpen(token)) return cancelIfWanted();
    std::string input;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        input = foreground_;
        cancelWakeLocked();
    }
    say("home: opening Native Tv Home over " + input + " after a wake by " + reason);
    if (!deps_.launch(kLgHome)) sayError("home: could not open Native Tv Home after the wake");
}

// LG's Home auto launch follows the screen; one at a time, and one more when
// asked for while it ran.
void HomeOrchestrator::syncAutoLaunch() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (syncBusy_) {
            syncAgain_ = true;
            return;
        }
        syncBusy_ = true;
    }
    for (;;) {
        const Json result = deps_.syncAutoLaunch(deps_.escaped());
        if (!result["ok"].boolean())
            sayError("home: could not sync LG's Home auto launch" +
                     (result["error"].isString() ? ": " + result["error"].string() : std::string()));
        std::lock_guard<std::mutex> lock(mutex_);
        if (syncAgain_) {
            syncAgain_ = false;
            continue;
        }
        syncBusy_ = false;
        return;
    }
}

// ---------------------------------------------------------------- Native Tv Home

/*
 * Native Tv Home is the native client itself, started by sam under LG's Home's
 * id (home_takeover_stager), so a client that cannot stay up leaves the Home
 * key with nothing to open. Nothing here sees the process; the foreground app
 * and sam's running list tell it: Home leaves the front, nothing or an input
 * (what sam puts in front when the app in front is gone) comes there, and sam
 * no longer lists Home. An app opened from Home also takes it out of the front,
 * and sam may close Home in the background later; that is not counted.
 * Standby, a screen that is off, LG's Home opened on purpose and a takeover
 * that is not in place are not watched.
 */
void HomeOrchestrator::watchForeground(const std::string &was, const std::string &now) {
    if (!deps_.blocking) return;
    // The running list is read at once: sam starts Home again at the next press
    // of the key, and a list read later would show that one.
    if (was == kLgHome && (now.empty() || isInputApp(now)) && deps_.nowMs && deps_.homeRunning &&
        deps_.after)
        deps_.blocking([this, now]() { checkHomeLeft(now); });
    if (now == kLgHome && was != kLgHome && deps_.closeApp)
        deps_.blocking([this]() { closeTileClient(); });
}

void HomeOrchestrator::checkHomeLeft(const std::string &next) {
    if (!mayWatch() || deps_.homeRunning() != 0) return;
    // A standby that closed Home may announce itself after the foreground
    // change; that is one exit, and three within two minutes is not a standby.
    countExit("Native Tv Home was gone as " + (next.empty() ? std::string("nothing") : next) +
              " came to the front");
}

/*
 * The Native Tv tile (LG's launcher, the Homebrew Channel's tile, an app key)
 * opens the same client as an app of its own, com.tvweb.dashboard, beside
 * Native Tv Home's. Whenever Native Tv Home comes to the front while it holds
 * the place, that app is closed, so the TV keeps one client running, Home's;
 * Home's own Native Tv card opens the settings in Home's client. sam answers a
 * close of an app that is not running with a refusal ("no valid life handler",
 * webOS 11.2.0), which says nothing here.
 */
void HomeOrchestrator::closeTileClient() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (foreground_ != kLgHome) return;
    }
    if (!mayWatch()) return;
    if (deps_.closeApp(kHomeClientApp))
        say("home: the Native Tv app closed as Native Tv Home came to the front; Home has its own");
}

bool HomeOrchestrator::mayWatch() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (halted_ || !powerActive_ || (havePowerState_ && !systemOn(powerState_))) return false;
    }
    return deps_.config()["enabled"].boolean() && deps_.mounted() && !deps_.escaped();
}

void HomeOrchestrator::countExit(const std::string &why) {
    const long long now = deps_.nowMs();
    bool tripped;
    int recent;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (halted_) return;
        tripped = breaker_.recordExit(now, why);
        recent = breaker_.recentExits(now);
    }
    say("home: the native client failed to hold Home (" + why + "): " + std::to_string(recent) +
        " of " + std::to_string(breaker_.limit()) + " within " +
        std::to_string(breaker_.windowMs() / 1000) + " s");
    if (!tripped) return;
    Json variables = Json::object();
    variables.set("count", Json::text(std::to_string(breaker_.limit())));
    variables.set("seconds", Json::text(std::to_string(breaker_.windowMs() / 1000)));
    giveBack("crashLoop",
             say::msg("srv.home.crashLoop",
                      "Native Tv's Home screen stopped {count} times within {seconds} seconds.", variables));
}

/*
 * LG's Home back in its place, as when the server stops: home.json still says
 * Home is on, so the next start takes the place again, and until then the
 * ways back to Native Tv Home (the escape's return, a wake) find it halted.
 */
void HomeOrchestrator::giveBack(const std::string &cause, const std::string &reason) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (halted_) return;
        halted_ = true;
        haltCause_ = cause;
        haltReason_ = reason;
        haltedAt_ = deps_.nowMs ? deps_.nowMs() : 0;
        cancelWakeLocked();
    }
    sayError("home: " + reason + " LG's Home is back until Native Tv restarts or Home is switched on again");
    // ponytail: one try; if the TV refuses, LG's Home stays covered until
    // `tvwebctl restore-nativetv-home` or a restart (the log above says so).
    if (deps_.release && deps_.mounted()) {
        const Json released = deps_.release();
        if (!released["ok"].boolean())
            sayError("home: LG's Home could not be put back: " +
                     released["error"].string("the TV did not say why"));
    }
    if (!deps_.startingNotice(false))
        sayError("home: could not clear the Home key's starting notice");
    syncAutoLaunch();
}

void HomeOrchestrator::rearm() {
    std::lock_guard<std::mutex> lock(mutex_);
    breaker_.reset();
    halted_ = false;
    haltCause_.clear();
    haltReason_.clear();
}

bool HomeOrchestrator::halted() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return halted_;
}

Json HomeOrchestrator::nativeStatus() const {
    const long long now = deps_.nowMs ? deps_.nowMs() : 0;
    Json status = Json::object();
    status.set("watching", Json::flag(deps_.nowMs && deps_.homeRunning && deps_.after && deps_.blocking));
    status.set("crashes", breaker_.state(now));
    std::lock_guard<std::mutex> lock(mutex_);
    status.set("halted", Json::flag(halted_));
    if (halted_) {
        status.set("cause", Json::text(haltCause_));
        status.set("reason", Json::text(haltReason_));
        status.set("haltedSecondsAgo", Json::numeric(static_cast<double>((now - haltedAt_) / 1000)));
    }
    return status;
}

} // namespace daemon
} // namespace nativetv
