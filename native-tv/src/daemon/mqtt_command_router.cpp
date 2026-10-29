#include "daemon/mqtt_command_router.hpp"
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <limits>

namespace nativetv {
namespace daemon {
namespace {
std::string trim(const std::string &value) {
    size_t first = 0;
    size_t last = value.size();
    while (first < last && std::isspace(static_cast<unsigned char>(value[first])))
        ++first;
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1])))
        --last;
    return value.substr(first, last - first);
}

std::string upper(std::string value) {
    for (char &character : value)
        character = static_cast<char>(std::toupper(
            static_cast<unsigned char>(character)));
    return value;
}

std::string lower(std::string value) {
    for (char &character : value)
        character = static_cast<char>(std::tolower(
            static_cast<unsigned char>(character)));
    return value;
}

Json parseIntOr(const std::string &value, const Json &fallback) {
    const char *start = value.c_str();
    while (*start && std::isspace(static_cast<unsigned char>(*start)))
        ++start;
    errno = 0;
    char *end = nullptr;
    const long long parsed = std::strtoll(start, &end, 10);
    if (end == start)
        return fallback;
    if (errno == ERANGE)
        return Json::numeric(parsed < 0
            ? static_cast<double>(std::numeric_limits<long long>::min())
            : static_cast<double>(std::numeric_limits<long long>::max()));
    return Json::numeric(static_cast<double>(parsed));
}

Json makeControl(const std::string &action, const Json &value) {
    Json payload = Json::object();
    payload.set("action", Json::text(action));
    payload.set("value", value);
    return payload;
}

const Json *findLgSetting(const Json &rows, const std::string &id,
                          Json &storage) {
    if (!rows.isArray())
        return nullptr;
    for (const Json &row : rows.items()) {
        if (row["id"].isString() && row["id"].string() == id) {
            storage = row;
            return &storage;
        }
    }
    return nullptr;
}
} // namespace

MqttControlCommand routeMqttControlTopic(const std::string &topic,
                                        const std::string &commandPrefix,
                                        const std::string &textPayload,
                                        const Json &lgSettingsRows) {
    MqttControlCommand result;
    if (commandPrefix.empty() || topic.compare(0, commandPrefix.size(), commandPrefix) != 0 ||
        topic.size() <= commandPrefix.size() || topic.size() - commandPrefix.size() > 256 ||
        textPayload.size() > 16384)
        return result;

    std::string action = topic.substr(commandPrefix.size());
    const std::string value = trim(textPayload);
    const Json text = Json::text(value);
    Json controlValue = text;
    result.action = action;

    if (action == "piccap/power") {
        result.action = "piccap";
        result.refreshTelemetry = false;
    } else if (action == "rcu") {
        result.refreshTelemetry = false;
    } else if (action == "screen") {
        result.action = upper(value) == "OFF" ? "screenOff" : "screenOn";
        controlValue = Json();
        result.refreshTelemetry = false;
    } else if (action == "reboot" || action == "powerOff" || action == "powerOn") {
        controlValue = Json();
        result.refreshTelemetry = false;
    } else if (action == "mute") {
        result.refreshTelemetry = true;
        controlValue = Json::flag(upper(value) == "ON");
    } else if (action == "volume") {
        result.refreshTelemetry = true;
        controlValue = parseIntOr(value, Json::numeric(10));
    } else if (action == "input") {
        result.refreshTelemetry = true;
        std::string input = lower(value);
        input.erase(std::remove_if(input.begin(), input.end(), [](unsigned char character) {
            return std::isspace(character) != 0;
        }), input.end());
        controlValue = Json::text(input);
    } else if (action == "powerTimer/on" || action == "powerTimer/off") {
        result.refreshTelemetry = true;
        Json timer = Json::object();
        timer.set("timer", Json::text(action.substr(11)));
        timer.set("enabled", Json::flag(upper(value) == "ON"));
        result.action = "powerTimer";
        controlValue = timer;
    } else if (action.compare(0, 4, "lgs/") == 0) {
        const std::string id = action.substr(4);
        Json rowStorage;
        const Json *row = findLgSetting(lgSettingsRows, id, rowStorage);
        if (!row || id.empty())
            return result;
        result.action = "lgSetting";
        result.refreshTelemetry = true;
        Json setting = Json::object();
        setting.set("id", Json::text(id));
        if ((*row)["type"].string() == "switch") {
            setting.set("on", Json::flag(upper(value) == "ON"));
        } else if ((*row)["type"].string() == "number") {
            setting.set("value", parseIntOr(value, (*row)["value"]));
        } else {
            setting.set("value", text);
        }
        controlValue = setting;
    } else if (action == "toast") {
        result.refreshTelemetry = false;
    } else if (action == "refresher") {
        const std::string normalized = lower(value);
        result.action = normalized == "schedule" || normalized == "on"
            ? "refresherSchedule" : "refresherCancel";
        controlValue = Json();
        result.refreshTelemetry = true;
    } else {
        // The legacy handler forwarded other command suffixes to /api/control.
        result.refreshTelemetry = true;
    }

    result.payload = makeControl(result.action, controlValue);
    result.valid = true;
    return result;
}

} // namespace daemon
} // namespace nativetv
