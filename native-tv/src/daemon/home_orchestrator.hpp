#pragma once

#include "core/json.hpp"
#include "daemon/home_crash_breaker.hpp"
#include <functional>
#include <mutex>
#include <string>

namespace nativetv {
namespace daemon {

// How the crash watch counts. Tests shorten them.
struct HomeWatchOptions {
    // This many unexpected exits within the window give LG's Home back.
    int exitLimit = 3;
    long long windowMs = 120000;
    // How long LG's Home (nativeTv's stub) may be in front before the native
    // client is, a cold start included, before that is counted as an exit.
    long long stubGraceMs = 15000;
};

// Everything the orchestrator reads or asks for. Nothing here is its own: the
// daemon passes its modules in, and a test passes scripted ones.
struct HomeOrchestratorDeps {
    // home.json, normalised, as it is now.
    std::function<Json()> config;
    // Loads home.json, lets `mutate` edit the normalised document and saves it.
    std::function<bool(const std::function<void(Json &)> &mutate, std::string &error)> update;
    // {ok, reason}: whether nativeTv Home is offered on this TV: its webOS, and
    // the native client's probe. A refusal by the probe carries `probe` (its
    // verdict); the version's has none.
    std::function<Json()> supported;
    // nativeTv Home holds LG's Home's place.
    std::function<bool()> mounted;
    // LG's Home is open by one of the ways back to it.
    std::function<bool()> escaped;
    // webOS 9, whose start layout can be emptied.
    std::function<bool()> startLayoutAvailable;
    // The empty start layout, or LG's kept aside from it, is still on the TV.
    std::function<bool()> blankKept;
    // Takes LG's Home's place: {ok, error}.
    std::function<Json(bool blankStartLayout)> takeover;
    // The whole switch-off (LG's Home back, auto launch, what earlier builds
    // left): {ok, error}.
    std::function<Json()> disable;
    // The Home key's starting notice, on or cleared. False: the TV did not take it.
    std::function<bool(bool on)> startingNotice;
    // LG's Home auto launch brought in line with the config: {ok, error}.
    std::function<Json(bool escaped)> syncAutoLaunch;
    // applicationManager/launch.
    std::function<bool(const std::string &appId)> launch;
    // tvpower's reason for the last power on, "" where it could not be read.
    std::function<std::string()> powerOnReason;

    // What earlier builds left. The general settings by key, or an invalid Json
    // where the TV did not answer; false from the write where it refused.
    std::function<Json(const Json &keys)> readSettings;
    std::function<bool(const Json &settings)> writeSettings;
    // The app the remote's Home key opens through the key filter, "" for none;
    // and giving the key back to LG's filters.
    std::function<std::string()> homeKeyApp;
    std::function<bool()> clearHomeKey;
    // The separate app earlier builds installed: 1 yes, 0 no, -1 unknown.
    std::function<int()> legacyAppInstalled;
    std::function<int()> legacyAppRunning;
    std::function<void()> killLegacyApp;
    std::function<void()> removeLegacyApp;

    // The crash watch, off unless all three of these are set. clientRunning:
    // the native client (com.tvweb.native) is in the app manager's running
    // list, 1 yes, 0 no, -1 unknown. release: LG's Home back in its place with
    // home.json left as it is (what tvwebctl does when the server stops):
    // {ok, error}. nowMs: a steady clock.
    std::function<int()> clientRunning;
    std::function<Json()> release;
    std::function<long long()> nowMs;
    HomeWatchOptions watch;

    // Timers and blocking work, on the daemon's event loop.
    std::function<unsigned long long(long long delayMs, std::function<void()> task)> after;
    std::function<void(unsigned long long timer)> cancel;
    std::function<void(std::function<void()> work)> blocking;
    std::function<void(const std::string &line)> log;
    std::function<void(const std::string &line)> logError;
};

// What lib/home.js still did around the daemon's own Home routes: the start-up
// sequence (what earlier builds left, taking LG's Home's place, the starting
// notice, LG's auto launch), the wake from standby that opens nativeTv Home
// over the input LG put in front, and LG's auto launch following the screen.
// It also watches the native client that the Home stub starts: one that keeps
// failing, or that the probe refused, gives LG's Home back, so no TV is left
// without a Home. nativeTv's own switching on and off, layout and the ways back
// to LG's Home are the front door's, and stay there.
class HomeOrchestrator {
  public:
    explicit HomeOrchestrator(HomeOrchestratorDeps deps);
    ~HomeOrchestrator();
    HomeOrchestrator(const HomeOrchestrator &) = delete;
    HomeOrchestrator &operator=(const HomeOrchestrator &) = delete;

    // Blocking; once, at start.
    void start();
    // application/app_id changed (empty where sam names nothing in front).
    void onForeground(const std::string &appId);
    // power/state changed, as tvpower names it.
    void onPowerState(const std::string &raw);
    // power/screenOn changed.
    void onScreen(bool active);
    void stop();

    // Home was switched on or off by its owner: the crash watch starts over.
    void rearm();
    // LG's Home was given back (the client kept failing, or the probe refused
    // it) and stays so until the server restarts or rearm().
    bool halted() const;
    // {watching, halted, cause?, reason?, haltedSecondsAgo?, crashes} for GET /api/home.
    Json nativeStatus() const;

  private:
    // An unexpected exit of the native client is either of two things: it is
    // gone from the app manager's list as LG's Home (the stub) or nothing comes
    // to the front, or the stub is still in front after the grace. Three within
    // two minutes give LG's Home back.
    void watchForeground(const std::string &was, const std::string &now);
    void checkClientLeft(const std::string &next);
    void armStub();
    void stubExpired(unsigned long long token);
    bool mayWatch();
    void countExit(const std::string &why);
    void giveBack(const std::string &cause, const std::string &reason);
    void cancelStubLocked();

    bool cleanLegacy();
    bool finishLegacy();
    void tryRemoveLegacyApp();
    void removeLegacyApp();
    void cancelWakeLocked();
    void fireWake(unsigned long long token);
    bool wakeMayOpen(unsigned long long token);
    void syncAutoLaunch();
    void say(const std::string &line) const;
    void sayError(const std::string &line) const;

    HomeOrchestratorDeps deps_;
    mutable std::mutex mutex_;
    std::string foreground_;
    bool havePowerState_ = false;
    std::string powerState_;
    bool powerActive_ = true;
    bool wakeActive_ = false;
    unsigned long long wakeToken_ = 0;
    std::string wakeInput_;
    unsigned long long wakeTimer_ = 0;
    bool legacyPending_ = false;
    bool legacyRemoving_ = false;
    bool syncBusy_ = false;
    bool syncAgain_ = false;
    HomeCrashBreaker breaker_;
    bool halted_ = false;
    std::string haltCause_;
    std::string haltReason_;
    long long haltedAt_ = 0;
    bool stubWatching_ = false;
    unsigned long long stubToken_ = 0;
    unsigned long long stubTimer_ = 0;
};
} // namespace daemon
} // namespace nativetv
