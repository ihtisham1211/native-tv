#include "service/config_settings.hpp"
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace nativetv {
namespace service {
namespace {
const char *const kEntityCategories[] = {
    "controls", "oled", "video", "system", "diagnostics"
};

std::string trim(const std::string &value) {
    size_t first = 0;
    while (first < value.size() &&
           std::isspace(static_cast<unsigned char>(value[first])))
        ++first;
    size_t last = value.size();
    while (last > first &&
           std::isspace(static_cast<unsigned char>(value[last - 1])))
        --last;
    return value.substr(first, last - first);
}

std::string stringValue(const Json &value) {
    return value.isString() ? value.string() : std::string();
}

bool truthy(const Json &value) {
    if (!value.valid()) return false;
    if (value.isBoolean()) return value.boolean();
    if (value.isNumber()) return value.number() != 0;
    if (value.isString()) return !value.string().empty();
    return true;
}

bool parseInt10(const Json &value, double &parsed) {
    if (value.isNumber()) {
        const double number = value.number();
        if (!std::isfinite(number)) return false;
        parsed = std::trunc(number);
        return true;
    }
    if (!value.isString()) return false;
    const std::string source = value.string();
    size_t start = 0;
    while (start < source.size() &&
           std::isspace(static_cast<unsigned char>(source[start])))
        ++start;
    size_t cursor = start;
    if (cursor < source.size() && (source[cursor] == '+' || source[cursor] == '-'))
        ++cursor;
    const size_t digits = cursor;
    while (cursor < source.size() && source[cursor] >= '0' && source[cursor] <= '9')
        ++cursor;
    if (cursor == digits) return false;
    errno = 0;
    char *end = nullptr;
    const long long integer = std::strtoll(source.c_str() + start, &end, 10);
    if (errno == ERANGE || !end || end == source.c_str() + start) return false;
    parsed = static_cast<double>(integer);
    return true;
}

bool badTopic(const std::string &value) {
    if (value.empty() || value.front() == '/' || value.back() == '/') return true;
    for (unsigned char character : value) {
        if (character == '+' || character == '#' || std::isspace(character))
            return true;
    }
    return false;
}

// A broker address is a host name or an address, with no scheme, path or space.
bool badBrokerHost(const std::string &value) {
    for (unsigned char character : value)
        if (std::isspace(character) || character == '/') return true;
    return false;
}

// What goes to Home Assistant as the device's name is text, not markup.
bool badDeviceName(const std::string &value) {
    for (unsigned char character : value)
        if (character < 0x20 || character == 0x7f || character == '<' || character == '>') return true;
    return false;
}

bool validEntityId(const std::string &value) {
    if (value.empty() || value.size() > 64) return false;
    for (unsigned char character : value) {
        if (!((character >= 'a' && character <= 'z') ||
              (character >= '0' && character <= '9') || character == '_' ||
              character == '.'))
            return false;
    }
    return true;
}

} // namespace

bool validateDashboardSettings(const Json &input, Json &patch,
                               std::vector<std::string> &errors) {
    errors.clear();
    const Json mqttInput = input.isObject() ? input["mqtt"] : Json();
    const Json deviceInput = input.isObject() ? input["device"] : Json();
    Json mqtt = Json::object();
    Json device = Json::object();

    mqtt.set("enabled", Json::flag(truthy(mqttInput["enabled"])));
    const std::string host = trim(stringValue(mqttInput["host"]));
    mqtt.set("host", Json::text(host));
    if (mqtt["enabled"].boolean() && host.empty())
        errors.push_back("a broker address is required to enable MQTT");
    else if (badBrokerHost(host))
        errors.push_back("the broker address is a host name or IP address: no spaces, no mqtt:// and no path");

    const Json portInput = mqttInput["port"];
    if (!portInput.valid() || (portInput.isString() && portInput.string().empty())) {
        mqtt.set("port", Json());
    } else {
        double port = 0;
        if (!parseInt10(portInput, port) || port < 1 || port > 65535) {
            errors.push_back("port must be between 1 and 65535");
        } else {
            mqtt.set("port", Json::numeric(port));
        }
    }

    mqtt.set("tls", Json::flag(truthy(mqttInput["tls"])));
    mqtt.set("tlsRejectUnauthorized",
             Json::flag(!mqttInput.has("tlsRejectUnauthorized") ||
                        !(mqttInput["tlsRejectUnauthorized"].isBoolean() &&
                          !mqttInput["tlsRejectUnauthorized"].boolean())));
    mqtt.set("username", Json::text(trim(stringValue(mqttInput["username"]))));
    if (mqttInput["password"].isString())
        mqtt.set("password", mqttInput["password"]);

    std::string topicPrefix = trim(stringValue(mqttInput["topicPrefix"]));
    if (topicPrefix.empty()) topicPrefix = "lgtv";
    if (badTopic(topicPrefix))
        errors.push_back("topic prefix cannot contain +, # or spaces, or start or end with /");
    mqtt.set("topicPrefix", Json::text(topicPrefix));

    std::string discoveryPrefix = trim(stringValue(mqttInput["discoveryPrefix"]));
    if (discoveryPrefix.empty()) discoveryPrefix = "homeassistant";
    if (badTopic(discoveryPrefix))
        errors.push_back("discovery prefix cannot contain +, # or spaces, or start or end with /");
    mqtt.set("discoveryPrefix", Json::text(discoveryPrefix));

    double interval = 0;
    if (!parseInt10(mqttInput["telemetryIntervalMs"], interval) ||
        interval < 1000 || interval > 600000) {
        errors.push_back("telemetry interval must be between 1000 and 600000 ms");
    } else {
        mqtt.set("telemetryIntervalMs", Json::numeric(interval));
    }

    Json entities = Json::object();
    const Json entitiesInput = mqttInput["entities"];
    for (const char *category : kEntityCategories) {
        const Json enabled = entitiesInput.isObject() ? entitiesInput[category] : Json();
        entities.set(category, Json::flag(enabled.isBoolean() ? enabled.boolean() : true));
    }
    Json disabled = Json::array();
    const Json disabledInput = entitiesInput["disabled"];
    if (disabledInput.isArray()) {
        for (const Json &item : disabledInput.items()) {
            if (item.isString() && validEntityId(item.string()))
                disabled.add(item);
        }
    }
    entities.set("disabled", disabled);
    mqtt.set("entities", entities);

    const std::string deviceId = trim(stringValue(deviceInput["id"]));
    bool validDeviceId = !deviceId.empty() && deviceId.size() <= 64;
    for (unsigned char character : deviceId) {
        if (!((character >= 'a' && character <= 'z') ||
              (character >= '0' && character <= '9') || character == '_'))
            validDeviceId = false;
    }
    if (!validDeviceId)
        errors.push_back("device id must be 1-64 characters of a-z, 0-9 or _");
    device.set("id", Json::text(deviceId));
    const std::string deviceName = trim(stringValue(deviceInput["name"]));
    if (badDeviceName(deviceName))
        errors.push_back("device name cannot contain < or > or control characters");
    device.set("name", Json::text(deviceName));

    patch = Json::object();
    patch.set("mqtt", mqtt);
    patch.set("device", device);
    if (input.isObject() && input["iconServer"].isBoolean())
        patch.set("iconServer", input["iconServer"]);
    return errors.empty();
}
} // namespace service
} // namespace nativetv
