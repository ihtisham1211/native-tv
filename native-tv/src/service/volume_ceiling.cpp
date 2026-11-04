#include "service/volume_ceiling.hpp"
#include "service/screen_time.hpp"
#include <algorithm>
#include <cmath>

namespace nativetv {
namespace service {
namespace {
bool isLevel(const Json &value, int minimum) {
    if (!value.isNumber())
        return false;
    const double number = value.number();
    return std::isfinite(number) && std::floor(number) == number &&
           number >= minimum && number <= 100;
}

bool validField(const std::string &key, const Json &value) {
    if (key == "enabled")
        return value.isBoolean();
    if (key == "maxVolume" || key == "nightMaxVolume")
        return isLevel(value, 0);
    if (key == "startupVolume")
        return isLevel(value, -1);
    if (key == "nightStart" || key == "nightEnd")
        return screenTimeValidClock(value);
    return false;
}

bool knownField(const std::string &key) {
    return key == "enabled" || key == "maxVolume" || key == "nightMaxVolume" ||
           key == "startupVolume" || key == "nightStart" || key == "nightEnd";
}

bool insideNight(const Json &settings, int64_t localNowMs) {
    if (localNowMs < 0)
        return false;
    // The night window is a bedtime window by another name.
    Json window = Json::object();
    window.set("bedtimeStart", settings["nightStart"]);
    window.set("bedtimeEnd", settings["nightEnd"]);
    return screenTimeBedtime(window, localNowMs).inside;
}
} // namespace

bool volumeCeilingValidField(const std::string &key, const Json &value) {
    return validField(key, value);
}

bool validateVolumeCeilingPatch(const Json &patch, Json &value, std::string &error) {
    value = Json::object();
    error.clear();
    if (!patch.isObject()) {
        error = "no settings";
        return false;
    }
    for (const std::string &key : patch.keys()) {
        if (!validField(key, patch[key])) {
            error = knownField(key) ? "invalid " + key : "unknown setting: " + key;
            return false;
        }
        value.set(key, patch[key]);
    }
    return true;
}

Json volumeCeilingSettings(const Json &configDocument) {
    const Json configured = configDocument["volumeCeiling"];
    Json settings = Json::object();
    settings.set("enabled", Json::flag(false));
    settings.set("maxVolume", Json::numeric(100));
    settings.set("nightMaxVolume", Json::numeric(100));
    settings.set("nightStart", Json::text(""));
    settings.set("nightEnd", Json::text(""));
    settings.set("startupVolume", Json::numeric(-1));
    for (const std::string &key : settings.keys())
        if (validField(key, configured[key]))
            settings.set(key, configured[key]);
    return settings;
}

int volumeCeilingLimit(const Json &settings, int64_t localNowMs) {
    if (!settings["enabled"].boolean())
        return 100;
    int limit = static_cast<int>(settings["maxVolume"].number());
    if (insideNight(settings, localNowMs))
        limit = std::min(limit, static_cast<int>(settings["nightMaxVolume"].number()));
    return limit;
}

int volumeCeilingStartup(const Json &settings, int64_t localNowMs) {
    const int startup = static_cast<int>(settings["startupVolume"].number());
    if (!settings["enabled"].boolean() || startup < 0)
        return -1;
    return std::min(startup, volumeCeilingLimit(settings, localNowMs));
}

} // namespace service
} // namespace nativetv
