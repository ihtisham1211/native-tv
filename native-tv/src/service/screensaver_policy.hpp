#pragma once

#include "core/json.hpp"
#include <set>
#include <string>

namespace nativetv {
namespace service {

// What the screen saver policy reads: LG's stock runner type, the webOS major
// version, the marker files of the mounted saver, which saver assets ship, and
// the config flags. Gathering is the caller's job so the rules stay pure.
struct ScreensaverState {
    std::string stockType;     // "" until the stock manifest has been seen once
    int webosMajor = 0;
    bool allowedAnyway = false;
    bool allowControl = false;
    std::string markerMode;    // trimmed .tvweb-screensaver, "" when absent
    std::string markerLevel;   // trimmed .tvweb-brightness, "" when absent
    std::set<std::string> assets; // saver ids whose QML is packaged
    bool externalActive = false;
    bool externalHook = false;
};

// True where LG's screen saver runs on another runner than ours (Flutter on
// webOS 10), so switching to or from it needs sam to reread the app.
bool screensaverSlowSwitch(const ScreensaverState &state);
// Slow-switch firmware that has been tried and is on by default (webOS 10, 11).
bool screensaverOnByDefault(const ScreensaverState &state);
bool screensaverHeldBack(const ScreensaverState &state);
// Where custom screen savers are experimental: held back, or turned on anyway.
bool screensaverExperimental(const ScreensaverState &state);
std::string screensaverMode(const ScreensaverState &state);
std::string screensaverLevel(const ScreensaverState &state);
// The `/api/screensaver` response.
Json screensaverList(const ScreensaverState &state);

} // namespace service
} // namespace nativetv
