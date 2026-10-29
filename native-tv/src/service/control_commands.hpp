#pragma once

#include "core/json.hpp"
#include <string>

namespace nativetv {
namespace service {

struct PowerTimerChange {
    std::string timer;
    std::string enableKey;
    Json settings;
    bool enabledProvided = false;
    bool enable = false;
    bool disable = false;
};

// Matches the legacy controls.js parseInt(value, 10) fallback and clamp.
int controlVolumeTarget(const Json &value);
int controlVolumeStep(const Json &value);
bool controlMuteTarget(const Json &value);
// Matches the legacy notification toast's String(value || 'hello') and
// 120 UTF-16-code-unit limit while keeping UTF-8 code points intact.
std::string controlToastMessage(const Json &value);
// Maps service-menu names to the two keys accepted by LG's factory app.
std::string controlServiceMenuTarget(const Json &value);
// Returns the legacy network-input key code for a recognized RCU navigation
// key, or zero so unported values can retain the Node fallback.
int controlRemoteKeyCode(const Json &value);
// True only for the legacy special Home action, which launches the Home app
// and falls back to the webOS 3–5 Home ribbon key when the app is unavailable.
bool controlRemoteHome(const Json &value);
// Returns a legacy media key code, resolving play/pause from the current media
// state. Unknown names return zero. Matching is intentionally case-sensitive.
int controlMediaKeyCode(const Json &value, const std::string &playState = std::string(),
                        const std::string &playerType = std::string());
// Returns the legacy application-manager id for a supported input, or empty.
std::string controlInputAppId(const Json &value);
// Match the legacy String(value || '').trim() handling for picture controls.
std::string controlPictureModeTarget(const Json &value);
bool controlEnergySavingTarget(const Json &value, std::string &target);
bool controlBacklightTarget(const Json &value, int &target);
bool controlIndicatorLightTarget(const Json &value);
bool controlLogoDimmingTarget(const Json &value, std::string &target);
bool controlAlwaysReadyOffStartHour(const Json &value, int &hour);
bool controlSleepTimerTarget(const Json &value, std::string &target);
bool controlPowerTimerChange(const Json &value, PowerTimerChange &change,
                             std::string &error);
std::string controlCloseAppId(const Json &value);
std::string controlSoundOutputTarget(const Json &value);
std::string controlSoundOutputAlias(const std::string &target);
bool controlLaunchUrlTarget(const Json &value, std::string &target);
bool volumeScenarioUnsupported(const Json &reply);

} // namespace service
} // namespace nativetv
