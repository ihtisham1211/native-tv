#include "service/hdmi_inputs.hpp"
#include "core/json.hpp"
#include "domain/names.hpp"
#include <cmath>
#include <string>

namespace nativetv {
namespace service {
namespace {
const char *inputMapKey = "inputMap.videoInputMapIndexInfo0";

bool truthy(const Json &value) {
    if (value.isBoolean())
        return value.boolean();
    if (value.isNumber())
        return value.number() != 0;
    if (value.isString())
        return !value.string().empty();
    return value.isArray() || value.isObject();
}

Json hdmiPort(const Json &fact, unsigned port) {
    const Json timing = fact["timing"];
    const bool connected = timing["connected"].boolean();
    Json result = Json::object();
    result.set("port", Json::numeric(port));
    result.set("connected", Json::flag(connected));
    const Json width = timing["width"];
    const Json height = timing["height"];
    std::string resolution;
    if (width.valid()) {
        resolution = compactNumber(width.number());
        resolution += "x";
        resolution += height.valid() ? compactNumber(height.number()) : "null";
    }
    result.set("resolution", resolution.empty() ? Json() : Json::text(resolution));
    result.set("refreshHz", timing["refreshHz"]);
    const Json pixelClock = timing["pixelClockKhz"];
    result.set("pixelClockMhz", pixelClock.isNumber()
                                    ? Json::numeric(std::round(pixelClock.number() / 100) / 10)
                                    : Json());
    result.set("colorDepth", connected ? fact["colorDepth"] : Json());
    result.set("interlaced", Json::flag(connected && fact["interlaced"].boolean()));
    return result;
}

bool hasCecDevice(const Json &device) {
    const Json id = device["lastUniqueId"];
    if (!id.valid())
        return false;
    if (id.isNumber())
        return id.number() != 255 && id.number() != -1;
    return id.string() != "255" && id.string() != "-1";
}
} // namespace

Json hdmiBaseInputMap() {
    Json result = Json::object();
    result.set("1", Json::numeric(3));
    result.set("2", Json::numeric(2));
    result.set("3", Json::numeric(1));
    result.set("4", Json::numeric(0));
    return result;
}

Json hdmiInputMapFromConfig(const Json &response) {
    if (!response.isObject() ||
        (response["returnValue"].isBoolean() && !response["returnValue"].boolean()))
        return hdmiBaseInputMap();
    const Json table = response["configs"][inputMapKey];
    const Json assignment = table.isArray() && table.size() > 0
                                ? table[size_t(0)]["assignment"]
                                : Json();
    if (!assignment.isObject())
        return hdmiBaseInputMap();

    Json result = Json::object();
    bool any = false;
    for (unsigned input = 1; input <= 4; ++input) {
        const std::string key = std::to_string(input);
        const Json raw = assignment[names::hdmiKey(input)];
        const std::string value = raw.string();
        if (value.size() == 1 && value[0] >= '0' && value[0] <= '3') {
            result.set(key, Json::numeric(value[0] - '0'));
            any = true;
        } else {
            result.set(key, Json());
        }
    }
    return any ? result : hdmiBaseInputMap();
}

Json hdmiInputs(const Json &systemSnapshot, const Json &devices,
                const Json &foreground, const Json &receiverMap) {
    Json ports = Json::array();
    Json portByNumber[4];
    const Json rawPorts = systemSnapshot["hdmiPorts"];
    const Json facts = systemSnapshot["hdmiPortFacts"];
    for (unsigned number = 0; number < 4; ++number) {
        const Json raw = rawPorts[number];
        const Json fact = facts[number];
        if (!raw.isString() || raw.string().empty() || !fact.isObject())
            continue;
        portByNumber[number] = hdmiPort(fact, number);
        ports.add(portByNumber[number]);
    }

    const std::string onScreen = foreground["appId"].string();
    Json inputs = Json::array();
    for (const Json &device : devices.items()) {
        const Json id = device["id"];
        if (!id.isString())
            continue;
        const Json key = names::hdmiKeyForPortId(id.string());
        if (!key.isString())
            continue;
        const std::string shortKey = key.string();
        const unsigned inputNumber = static_cast<unsigned>(names::hdmiPort(shortKey).number());
        const Json mappedReceiver = receiverMap[std::to_string(inputNumber)];
        const bool validReceiver = mappedReceiver.isNumber() &&
                                   mappedReceiver.number() >= 0 &&
                                   mappedReceiver.number() < 4;
        const unsigned receiver = validReceiver
                                      ? static_cast<unsigned>(mappedReceiver.number())
                                      : 4;
        std::string appId = device["appId"].string();
        if (appId.empty())
            appId = names::inputAppId(shortKey);

        Json entry = Json::object();
        entry.set("id", id);
        if (device["port"].valid())
            entry.set("port", device["port"]);
        const std::string label = device["label"].string();
        entry.set("label", Json::text(label.empty() ? id.string() : label));
        entry.set("appId", Json::text(appId));
        entry.set("active", Json::flag(truthy(device["activate"]) && appId == onScreen));
        entry.set("deviceSeen", Json::flag(
                                    hasCecDevice(device) || truthy(device["hdmiPlugIn"]) ||
                                    truthy(device["connected"]) ||
                                    device["subCount"].number() > 0));
        const Json signal = validReceiver ? portByNumber[receiver] : Json();
        entry.set("signal", signal.isObject() && signal["connected"].boolean() ? signal : Json());
        inputs.add(entry);
    }

    Json result = Json::object();
    result.set("ok", Json::flag(true));
    result.set("inputs", inputs);
    result.set("ports", ports);
    return result;
}
} // namespace service
} // namespace nativetv
