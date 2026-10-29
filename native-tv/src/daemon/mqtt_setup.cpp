#include "daemon/mqtt_setup.hpp"
#include <cctype>
#include <cmath>
#include <limits>
#include <sstream>
#include <vector>

namespace nativetv {
namespace daemon {
namespace {
bool truthy(const Json &value) {
    if (value.isBoolean())
        return value.boolean();
    if (value.isNumber())
        return value.number() != 0;
    if (value.isString())
        return !value.string().empty();
    return value.isArray() || value.isObject();
}

std::string trim(const Json &value) {
    if (!value.isString())
        return std::string();
    const std::string source = value.string();
    size_t first = 0;
    size_t last = source.size();
    while (first < last && std::isspace(static_cast<unsigned char>(source[first])))
        ++first;
    while (last > first && std::isspace(static_cast<unsigned char>(source[last - 1])))
        --last;
    return source.substr(first, last - first);
}

bool parseInteger(const Json &value, bool allowNull, long long &number, bool &isNull) {
    isNull = false;
    if (!value.valid()) {
        isNull = allowNull;
        return allowNull;
    }
    if (value.isString()) {
        const std::string text = value.string();
        if (allowNull && text.empty()) {
            isNull = true;
            return true;
        }
        size_t at = 0;
        while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at])))
            ++at;
        bool negative = false;
        if (at < text.size() && (text[at] == '+' || text[at] == '-')) {
            negative = text[at] == '-';
            ++at;
        }
        if (at == text.size() || !std::isdigit(static_cast<unsigned char>(text[at])))
            return false;
        unsigned long long result = 0;
        const unsigned long long maximum =
            negative ? static_cast<unsigned long long>(std::numeric_limits<long long>::max()) + 1
                     : static_cast<unsigned long long>(std::numeric_limits<long long>::max());
        while (at < text.size() && std::isdigit(static_cast<unsigned char>(text[at]))) {
            const unsigned int digit = static_cast<unsigned int>(text[at] - '0');
            if (result > (maximum - digit) / 10)
                return false;
            result = result * 10 + digit;
            ++at;
        }
        if (negative) {
            if (result == static_cast<unsigned long long>(std::numeric_limits<long long>::max()) + 1)
                number = std::numeric_limits<long long>::min();
            else
                number = -static_cast<long long>(result);
        } else {
            number = static_cast<long long>(result);
        }
        return true;
    }
    if (!value.isNumber() || !std::isfinite(value.number()))
        return false;
    const double truncated = std::trunc(value.number());
    if (truncated < static_cast<double>(std::numeric_limits<long long>::min()) ||
        truncated >= static_cast<double>(std::numeric_limits<long long>::max()))
        return false;
    number = static_cast<long long>(truncated);
    return true;
}

bool badTopic(const std::string &value) {
    if (value.empty() || value[0] == '/' || value[value.size() - 1] == '/')
        return true;
    for (size_t i = 0; i < value.size(); ++i) {
        const unsigned char character = static_cast<unsigned char>(value[i]);
        if (character == '+' || character == '#' || std::isspace(character))
            return true;
    }
    return false;
}

bool validDeviceId(const std::string &value) {
    if (value.empty() || value.size() > 64)
        return false;
    for (size_t i = 0; i < value.size(); ++i) {
        const char character = value[i];
        if (!((character >= 'a' && character <= 'z') ||
              (character >= '0' && character <= '9') || character == '_'))
            return false;
    }
    return true;
}

bool validDisabledId(const std::string &value) {
    if (value.empty() || value.size() > 64)
        return false;
    for (size_t i = 0; i < value.size(); ++i) {
        const char character = value[i];
        if (!((character >= 'a' && character <= 'z') ||
              (character >= '0' && character <= '9') || character == '_' || character == '.'))
            return false;
    }
    return true;
}

void addError(std::vector<std::string> &errors, const std::string &message) {
    errors.push_back(message);
}
} // namespace

bool buildPhoneMqttSettings(const Json &payload, const Json &config,
                            const Json &identity, Json &mqtt, Json &device,
                            std::string &error) {
    error.clear();
    const Json currentMqtt = config["mqtt"];
    const Json currentDevice = config["device"];
    std::vector<std::string> errors;

    mqtt = Json::object();
    mqtt.set("enabled", Json::flag(true));
    const std::string host = trim(payload["host"]);
    mqtt.set("host", Json::text(host));
    if (host.empty())
        addError(errors, "a broker address is required to enable MQTT");

    long long port = 0;
    bool nullPort = false;
    if (!parseInteger(payload["port"], true, port, nullPort) ||
        (!nullPort && (port < 1 || port > 65535))) {
        addError(errors, "port must be between 1 and 65535");
    } else if (nullPort) {
        mqtt.set("port", Json());
    } else {
        mqtt.set("port", Json::numeric(static_cast<double>(port)));
    }

    mqtt.set("tls", Json::flag(truthy(payload["tls"])));
    mqtt.set("tlsRejectUnauthorized",
             Json::flag(!(currentMqtt["tlsRejectUnauthorized"].isBoolean() &&
                          !currentMqtt["tlsRejectUnauthorized"].boolean())));
    mqtt.set("username", Json::text(trim(payload["username"])));
    if (payload["password"].isString())
        mqtt.set("password", payload["password"]);

    std::string prefix;
    if (identity.isObject() && identity["prefix"].isString())
        prefix = trim(identity["prefix"]);
    else
        prefix = trim(currentMqtt["topicPrefix"]);
    if (prefix.empty())
        prefix = "lgtv";
    if (badTopic(prefix))
        addError(errors, "topic prefix cannot contain +, # or spaces, or start or end with /");
    mqtt.set("topicPrefix", Json::text(prefix));

    std::string discovery = trim(currentMqtt["discoveryPrefix"]);
    if (discovery.empty())
        discovery = "homeassistant";
    if (badTopic(discovery))
        addError(errors, "discovery prefix cannot contain +, # or spaces, or start or end with /");
    mqtt.set("discoveryPrefix", Json::text(discovery));

    Json intervalValue = currentMqtt["telemetryIntervalMs"];
    if (!truthy(intervalValue))
        intervalValue = Json::numeric(10000);
    long long interval = 0;
    bool nullInterval = false;
    if (!parseInteger(intervalValue, false, interval, nullInterval) || nullInterval ||
        interval < 1000 || interval > 600000) {
        addError(errors, "telemetry interval must be between 1000 and 600000 ms");
    } else {
        mqtt.set("telemetryIntervalMs", Json::numeric(static_cast<double>(interval)));
    }

    const Json oldEntities = currentMqtt["entities"];
    Json entities = Json::object();
    const char *categories[] = {"controls", "oled", "video", "system", "diagnostics"};
    for (size_t i = 0; i < sizeof(categories) / sizeof(categories[0]); ++i) {
        const Json enabled = oldEntities[categories[i]];
        entities.set(categories[i], Json::flag(!enabled.isBoolean() || enabled.boolean()));
    }
    Json disabled = Json::array();
    const Json oldDisabled = oldEntities["disabled"];
    if (oldDisabled.isArray()) {
        const std::vector<Json> values = oldDisabled.items();
        for (size_t i = 0; i < values.size(); ++i) {
            if (values[i].isString() && validDisabledId(values[i].string()))
                disabled.add(values[i]);
        }
    }
    entities.set("disabled", disabled);
    mqtt.set("entities", entities);

    std::string deviceId;
    if (identity.isObject() && identity["id"].isString())
        deviceId = trim(identity["id"]);
    else
        deviceId = trim(currentDevice["id"]);
    if (!validDeviceId(deviceId))
        addError(errors, "device id must be 1-64 characters of a-z, 0-9 or _");
    device = Json::object();
    device.set("id", Json::text(deviceId));
    device.set("name", Json::text(trim(currentDevice["name"])));

    if (!errors.empty()) {
        std::ostringstream joined;
        for (size_t i = 0; i < errors.size(); ++i) {
            if (i)
                joined << "; ";
            joined << errors[i];
        }
        error = joined.str();
        return false;
    }
    return true;
}

} // namespace daemon
} // namespace nativetv
