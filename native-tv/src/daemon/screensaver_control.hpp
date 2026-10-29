#pragma once

#include "core/json.hpp"
#include "service/screensaver_policy.hpp"
#include <atomic>
#include <functional>
#include <mutex>
#include <string>

namespace nativetv {
namespace daemon {

// What the screen saver controls need from the daemon. Everything outside the
// decisions themselves comes through here, so the flow runs against a scripted
// bus in tests.
struct ScreensaverControlDeps {
    // One Luna call; true when the bus answered, with the reply in `reply`.
    std::function<bool(const std::string &uri, const Json &payload, Json &reply)> luna;
    std::function<bool()> injectBack;
    std::function<service::ScreensaverState()> state;
    std::function<Json(const std::string &mode, const std::string &level)> stage;
    std::function<void()> restoreStock;
    // Type of the manifest under the screen saver app now (qml once ours is mounted).
    std::function<std::string()> mountedType;
    // Ask SAM to read the manifest again: a block and release of the one app,
    // then a nudge. Each returns whether SAM accepted the call, not the outcome.
    std::function<bool()> samReread;
    std::function<bool()> samNudge;
    std::function<void(unsigned)> sleepMs;
    // The launched-marker the native remote input watches to close a saver.
    std::function<void(bool)> writeLaunchedMarker;
    std::function<bool()> launchedMarkerExists;
};

// Switching the screen saver and starting or closing it: the Server tab's
// screensaverMode and the dashboard's screensaver actions.
class ScreensaverControl {
  public:
    explicit ScreensaverControl(ScreensaverControlDeps deps);

    Json setMode(const std::string &mode, const std::string &level);
    Json trigger();
    // A saver started by launching its app may be showing; checks sam behind
    // the answer so it clears once the saver closes.
    bool launchedShowing();
    // At start: one an earlier run launched may still be showing.
    void adoptLaunched();

  private:
    bool settle(); // true where sam kept LG's saver (not taken)
    void restartApp();
    bool ensureKnown();
    bool running(bool &known);
    bool readMedia(Json &media);
    bool launchApp(std::string &error);
    bool refusedAfterRequest();
    void setLaunched(bool on);
    bool closeLaunched(Json &reply);

    ScreensaverControlDeps deps_;
    std::atomic<bool> launched_{false};
    bool acbAbsent_ = false;
    std::mutex mutex_;
};
} // namespace daemon
} // namespace nativetv
