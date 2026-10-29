#include "service/screen_time.hpp"
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

namespace nativetv {
namespace service {
namespace {
const int64_t kDayMs = 86400000;

bool isMinutes(const Json &value, int minimum) {
    if (!value.isNumber())
        return false;
    const double number = value.number();
    return std::isfinite(number) && std::floor(number) == number &&
           number >= minimum && number <= 1440;
}

size_t utf16Length(const std::string &text) {
    size_t units = 0;
    for (size_t index = 0; index < text.size();) {
        const unsigned char first = static_cast<unsigned char>(text[index]);
        size_t count = 1;
        uint32_t codepoint = first;
        if ((first & 0xe0) == 0xc0 && index + 1 < text.size()) {
            count = 2;
            codepoint = ((first & 0x1f) << 6) |
                        (static_cast<unsigned char>(text[index + 1]) & 0x3f);
        } else if ((first & 0xf0) == 0xe0 && index + 2 < text.size()) {
            count = 3;
            codepoint = ((first & 0x0f) << 12) |
                        ((static_cast<unsigned char>(text[index + 1]) & 0x3f) << 6) |
                        (static_cast<unsigned char>(text[index + 2]) & 0x3f);
        } else if ((first & 0xf8) == 0xf0 && index + 3 < text.size()) {
            count = 4;
            codepoint = ((first & 0x07) << 18) |
                        ((static_cast<unsigned char>(text[index + 1]) & 0x3f) << 12) |
                        ((static_cast<unsigned char>(text[index + 2]) & 0x3f) << 6) |
                        (static_cast<unsigned char>(text[index + 3]) & 0x3f);
        }
        units += codepoint > 0xffff ? 2 : 1;
        index += count;
    }
    return units;
}

bool validClock(const Json &value) {
    if (!value.isString())
        return false;
    const std::string clock = value.string();
    if (clock.empty())
        return true;
    if (clock.size() != 5 || clock[2] != ':' || clock[0] < '0' || clock[0] > '2' ||
        clock[1] < '0' || clock[1] > '9' || clock[3] < '0' || clock[3] > '5' ||
        clock[4] < '0' || clock[4] > '9')
        return false;
    return clock[0] != '2' || clock[1] <= '3';
}

int minuteOf(const Json &value) {
    const std::string clock = value.string();
    if (clock.size() != 5 || clock[2] != ':')
        return -1;
    return (clock[0] - '0') * 600 + (clock[1] - '0') * 60 +
           (clock[3] - '0') * 10 + (clock[4] - '0');
}

bool validApps(const Json &value) {
    if (!value.isObject() || value.keys().size() > 50)
        return false;
    for (const std::string &key : value.keys()) {
        if (utf16Length(key) > 200 || !isMinutes(value[key], 1))
            return false;
    }
    return true;
}

bool validField(const std::string &key, const Json &value) {
    if (key == "enabled")
        return value.isBoolean();
    if (key == "dailyMinutes")
        return isMinutes(value, 0);
    if (key == "bedtimeStart" || key == "bedtimeEnd")
        return validClock(value);
    if (key == "action")
        return value.isString() && (value.string() == "off" ||
                                    value.string() == "screenOff" ||
                                    value.string() == "warn");
    if (key == "appAction")
        return value.isString() && (value.string() == "home" || value.string() == "off" ||
                                    value.string() == "screenOff" ||
                                    value.string() == "warn");
    if (key == "apps")
        return validApps(value);
    return false;
}

bool knownField(const std::string &key) {
    return key == "enabled" || key == "dailyMinutes" || key == "bedtimeStart" ||
           key == "bedtimeEnd" || key == "action" || key == "appAction" || key == "apps";
}

std::vector<std::string> patchKeys(const Json &patch) {
    if (patch.isObject())
        return patch.keys();
    std::vector<std::string> keys;
    if (patch.isArray()) {
        for (size_t index = 0; index < patch.size(); ++index)
            keys.push_back(std::to_string(index));
    }
    return keys;
}

double watchedSeconds(const Json &today) {
    double seconds = 0;
    if (!today.isObject())
        return seconds;
    for (const std::string &key : today.keys()) {
        if (key == "screen_off" || key == "screen_saver")
            continue;
        const Json value = today[key];
        if (value.isNumber() && std::isfinite(value.number()) && value.number() > 0)
            seconds += value.number();
    }
    return seconds;
}
} // namespace

bool validateScreenTimePatch(const Json &patch, Json &value, std::string &error) {
    value = Json::object();
    error.clear();
    if (!patch.isObject() && !patch.isArray()) {
        error = "no settings";
        return false;
    }
    for (const std::string &key : patchKeys(patch)) {
        if (!validField(key, patch[key])) {
            error = knownField(key) ? "invalid " + key : "unknown setting: " + key;
            return false;
        }
        value.set(key, patch.isObject() ? patch[key] : Json());
    }
    return true;
}

Json screenTimeSettings(const Json &configDocument, bool allowPower, bool hasHomeApp) {
    const Json configured = configDocument["screenTime"];
    Json settings = Json::object();
    settings.set("enabled", Json::flag(false));
    settings.set("dailyMinutes", Json::numeric(0));
    settings.set("bedtimeStart", Json::text(""));
    settings.set("bedtimeEnd", Json::text(""));
    settings.set("action", Json::text("off"));
    settings.set("appAction", Json::text("home"));
    settings.set("apps", Json::object());
    for (const std::string &key : settings.keys()) {
        const Json value = configured[key];
        if (validField(key, value))
            settings.set(key, value);
    }
    if (!hasHomeApp && settings["appAction"].string() == "home")
        settings.set("appAction", Json::text(allowPower ? "off" : "screenOff"));
    return settings;
}

ScreenTimeBedtime screenTimeBedtime(const Json &settings, int64_t localNowMs) {
    ScreenTimeBedtime result;
    if (!validClock(settings["bedtimeStart"]) || !validClock(settings["bedtimeEnd"]))
        return result;
    const int from = minuteOf(settings["bedtimeStart"]);
    const int to = minuteOf(settings["bedtimeEnd"]);
    if (from < 0 || to < 0 || from == to)
        return result;
    result.configured = true;
    int64_t localWithinDay = localNowMs % kDayMs;
    if (localWithinDay < 0)
        localWithinDay += kDayMs;
    const int64_t seconds = localWithinDay / 1000;
    const int minute = static_cast<int>(seconds / 60);
    result.inside = from < to ? minute >= from && minute < to
                              : minute >= from || minute < to;
    const int64_t secondsUntilStart = static_cast<int64_t>(from) * 60 - seconds;
    result.untilStartSeconds = (secondsUntilStart % 86400 + 86400) % 86400;
    return result;
}

ScreenTimeDue screenTimeTvDue(const Json &settings, int64_t localNowMs,
                              const Json &todaySeconds) {
    ScreenTimeDue result;
    const ScreenTimeBedtime bed = screenTimeBedtime(settings, localNowMs);
    if (bed.inside) {
        result.due = true;
        result.id = "bedtime";
        result.action = settings["action"].string("off");
        result.text = "It is bedtime.";
        return result;
    }
    const Json dailySetting = settings["dailyMinutes"];
    const int dailyMinutes = isMinutes(dailySetting, 0)
                                 ? static_cast<int>(dailySetting.number())
                                 : 0;
    if (dailyMinutes <= 0)
        return result;
    const double remaining = dailyMinutes * 60.0 - watchedSeconds(todaySeconds);
    if (remaining <= 0) {
        result.due = true;
        result.id = "daily";
        result.action = settings["action"].string("off");
        result.text = "Today's screen time is used up.";
    }
    return result;
}

ScreenTimeDue screenTimeAppDue(const Json &settings, const std::string &key,
                               double todaySeconds) {
    ScreenTimeDue result;
    const Json limit = settings["apps"][key];
    if (!isMinutes(limit, 1) || !std::isfinite(todaySeconds))
        return result;
    if (limit.number() * 60.0 - todaySeconds <= 0) {
        result.due = true;
        result.id = "app:" + key;
        result.action = settings["appAction"].string("home");
        result.text = "Today's time for this is used up.";
    }
    return result;
}

} // namespace service
} // namespace nativetv
