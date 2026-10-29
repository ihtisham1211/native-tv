#include "service/quick_access.hpp"
#include "domain/names.hpp"
#include <set>
#include <string>

namespace nativetv {
namespace service {
namespace {
const std::string liveTv = "com.webos.app.livetv";

std::string settingKey(unsigned key) {
    return "magicNum" + std::to_string(key);
}

bool selectableApp(const Json &app) {
    const Json id = app["id"];
    if (!id.isString() || id.string().empty())
        return false;
    const Json visible = app["visible"];
    if (visible.isBoolean() && !visible.boolean())
        return false;
    const std::string appId = id.string();
    if (appId == liveTv)
        return false;
    return appId.compare(0, 18, "com.webos.app.hdmi") != 0 &&
           appId.compare(0, 23, "com.webos.app.container") != 0;
}
} // namespace

Json quickAccessSettingKeys() {
    Json keys = Json::array();
    for (unsigned key = 1; key <= 9; ++key)
        keys.add(Json::text(settingKey(key)));
    return keys;
}

bool quickAccessSettingsSupported(const Json &settings) {
    if (!settings.isObject())
        return false;
    for (unsigned key = 1; key <= 9; ++key) {
        const Json value = settings[settingKey(key)];
        if (!value.isObject() || !value["id"].isString())
            return false;
    }
    return true;
}

bool quickAccessTargetAllowed(const std::string &id, const Json &devices,
                              const Json &apps) {
    if (id.empty())
        return false;
    if (id == liveTv)
        return true;

    for (const Json &device : devices.items()) {
        const Json key = names::hdmiKeyForPortId(device["id"].string());
        if (key.isString() && id == names::inputAppId(key.string()))
            return true;
    }

    std::set<std::string> seen;
    for (const Json &app : apps.items()) {
        if (!selectableApp(app))
            continue;
        const std::string appId = app["id"].string();
        if (!seen.insert(appId).second)
            continue;
        if (id == appId)
            return true;
    }
    return false;
}

Json quickAccessWriteRequest(unsigned key, const std::string &id) {
    Json settings = Json::object();
    Json value = Json::object();
    value.set("id", Json::text(id));
    value.set("params", Json::object());
    settings.set(settingKey(key), value);

    Json request = Json::object();
    request.set("category", Json::text("option"));
    request.set("settings", settings);
    return request;
}

} // namespace service
} // namespace nativetv
