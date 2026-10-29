#include "daemon/ha_discovery.hpp"
#include "daemon/ha_entity_catalogue.hpp"
#include "domain/names.hpp"
#include <algorithm>
#include <cctype>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace nativetv {
namespace daemon {
namespace {

Json replaceTopicPrefix(const Json &value, const std::string &topicPrefix) {
    if (value.isObject()) {
        Json out = Json::object();
        const std::vector<std::string> keys = value.keys();
        for (const std::string &key : keys)
            out.set(key, replaceTopicPrefix(value[key], topicPrefix));
        return out;
    }
    if (value.isArray()) {
        Json out = Json::array();
        for (const Json &item : value.items())
            out.add(replaceTopicPrefix(item, topicPrefix));
        return out;
    }
    if (!value.isString())
        return value;

    std::string text = value.string();
    const std::string from = "tv/";
    const std::string to = topicPrefix + "/";
    size_t at = 0;
    while ((at = text.find(from, at)) != std::string::npos) {
        text.replace(at, from.size(), to);
        at += to.size();
    }
    return Json::text(text);
}

Json deviceFor(const Json &templateDevice, const Json &overrideDevice,
               const std::string &deviceId) {
    const Json base = replaceTopicPrefix(templateDevice, "tv");
    Json device = Json::object();
    for (const std::string &key : base.keys())
        device.set(key, overrideDevice.isObject() && overrideDevice.has(key)
            ? overrideDevice[key] : base[key]);
    if (overrideDevice.isObject())
        for (const std::string &key : overrideDevice.keys())
            if (!base.has(key))
                device.set(key, overrideDevice[key]);
    Json identifiers = Json::array();
    identifiers.add(Json::text(deviceId));
    device.set("identifiers", identifiers);
    return device;
}

bool validAppIconId(const std::string &id) {
    if (id.empty() || id.size() > 128)
        return false;
    const auto alphanumeric = [](unsigned char value) {
        return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
               (value >= '0' && value <= '9');
    };
    if (!alphanumeric(static_cast<unsigned char>(id[0])) && id[0] != '_' && id[0] != '-')
        return false;
    for (unsigned char value : id)
        if (!alphanumeric(value) && value != '_' && value != '-' && value != '.')
            return false;
    return true;
}

std::string appColor(const Json &value) {
    if (!value.isString())
        return std::string();
    std::string color = value.string();
    while (!color.empty() && std::isspace(static_cast<unsigned char>(color.front())))
        color.erase(color.begin());
    while (!color.empty() && std::isspace(static_cast<unsigned char>(color.back())))
        color.pop_back();
    if (color.size() != 4 && color.size() != 7)
        return std::string();
    if (color[0] != '#')
        return std::string();
    std::string hex;
    for (size_t index = 1; index < color.size(); ++index) {
        const unsigned char value = static_cast<unsigned char>(color[index]);
        if (!std::isxdigit(value))
            return std::string();
        hex += static_cast<char>(std::tolower(value));
    }
    if (hex.size() == 3) {
        std::string expanded;
        for (char value : hex) {
            expanded += value;
            expanded += value;
        }
        hex = std::move(expanded);
    }
    return "#" + hex;
}

void applyNamedSelect(Json &conf, const std::vector<std::string> &ids,
                      const Json &names, const std::string &stateExpression) {
    Json byId = Json::object();
    Json byName = Json::object();
    Json options = Json::array();
    for (const std::string &id : ids) {
        if (id.empty())
            continue;
        std::string name = names[id].string();
        if (name.empty())
            name = id;
        byId.set(id, Json::text(name));
        if (!byName.has(name)) {
            byName.set(name, Json::text(id));
            options.add(Json::text(name));
        }
    }
    conf.set("options", options);
    conf.set("command_template", Json::text(
        "{{ " + byName.dump(false) + ".get(value, value) }}"));
    conf.set("value_template", Json::text(
        "{{ " + byId.dump(false) + ".get(" + stateExpression + ", \"None\") }}"));
}

void applyPictureModes(Json &conf, const Json &requested) {
    static const char *const defaults[] = {
        "expert1", "expert2", "cinema", "game", "standard", "eco", "sports"
    };
    std::vector<std::string> ids;
    if (requested.isArray()) {
        for (const Json &mode : requested.items())
            if (mode["value"].isString())
                ids.push_back(mode["value"].string());
    }
    if (ids.empty())
        for (const char *id : defaults)
            ids.push_back(id);

    Json labels = Json::object();
    for (const std::string &id : ids)
        labels.set(id, names::pictureMode(id)["display"]);
    applyNamedSelect(conf, ids, labels,
        "((value_json.get('picture') or {}).get('mode_raw') if value_json.get('picture') else \"standard\")");
}

void applySoundOutputs(Json &conf, const Json &requested) {
    std::vector<std::string> ids;
    if (requested.isArray())
        for (const Json &output : requested.items())
            if (output.isString() && !output.string().empty())
                ids.push_back(output.string());
    if (ids.empty())
        ids = names::soundOutputs().keys();

    Json labels = Json::object();
    for (const std::string &id : ids)
        labels.set(id, names::soundOutput(id)["display"]);
    applyNamedSelect(conf, ids, labels,
        "((value_json.get('sound') or {}).get('output_raw') if value_json.get('sound') else \"tv_speaker\")");
}

void applyInstalledApps(Json &conf, const HaDiscoveryOptions &options) {
    static const std::pair<const char *, const char *> defaults[] = {
        {"com.webos.app.livetv", "Live TV"},
        {"youtube.leanback.v4", "YouTube"}, {"netflix", "Netflix"},
        {"amazon", "Prime Video"}, {"spotify-beehive", "Spotify"},
        {"com.apple.appletv", "Apple TV"}
    };
    Json byId = Json::object();
    std::set<std::string> taken;
    const auto add = [&byId, &taken](const std::string &id, const std::string &title) {
        if (id.empty() || byId.has(id))
            return;
        std::string name = title.empty() ? id : title;
        if (taken.count(name))
            name += " (" + id + ")";
        taken.insert(name);
        byId.set(id, Json::text(name));
    };
    if (options.installedApps.isArray() && options.installedApps.size() > 0) {
        for (const Json &app : options.installedApps.items())
            add(app["id"].string(), app["title"].string());
    } else {
        for (const auto &app : defaults)
            add(app.first, app.second);
    }
    add("com.palm.app.settings", "Settings");

    Json toId = Json::object();
    Json appIcons = Json::object();
    Json appColors = Json::object();
    Json attrs = Json::object();
    for (const std::string &id : byId.keys()) {
        const std::string name = byId[id].string();
        toId.set(name, Json::text(id));
        if (!options.iconBaseUrl.empty() && validAppIconId(id))
            appIcons.set(id, Json::text(options.iconBaseUrl + id + ".png"));
    }
    if (!options.iconBaseUrl.empty())
        attrs.set("app_icons", appIcons);
    for (const Json &app : options.installedApps.items()) {
        const std::string id = app["id"].string();
        const std::string color = appColor(app["iconColor"]);
        if (!color.empty() && byId.has(id))
            appColors.set(id, Json::text(color));
    }
    attrs.set("app_ids", toId);
    // Node builds app_ids first and appends optional maps afterwards.
    if (!options.iconBaseUrl.empty()) {
        Json ordered = Json::object();
        ordered.set("app_ids", toId);
        ordered.set("app_icons", appIcons);
        if (!appColors.keys().empty())
            ordered.set("app_colors", appColors);
        attrs = ordered;
    } else if (!appColors.keys().empty()) {
        Json ordered = Json::object();
        ordered.set("app_ids", toId);
        ordered.set("app_colors", appColors);
        attrs = ordered;
    }

    Json optionNames = Json::array();
    for (const std::string &name : toId.keys())
        optionNames.add(Json::text(name));
    conf.set("command_template", Json::text(
        "{{ " + toId.dump(false) + ".get(value, value) }}"));
    conf.set("value_template", Json::text(
        "{{ " + byId.dump(false) + ".get(value_json.get('app_id'), \"None\") }}"));
    conf.set("json_attributes_template", Json::text(
        "{{ " + attrs.dump(false) + " | tojson }}"));
    conf.set("options", optionNames);
}

std::string discoveryTopic(const HaDiscoveryOptions &options,
                           const std::string &type, const std::string &id) {
    return options.discoveryPrefix + "/" + type + "/" + options.deviceId + "/" +
           id + "/config";
}

const char *lgSettingRowForEntity(const std::string &id) {
    static const std::pair<const char *, const char *> rows[] = {
        {"sound_mode", "soundMode"}, {"bluetooth_speaker_mode", "btSpeakerMode"},
        {"simplink", "simplinkEnable"}, {"auto_power_sync", "simplinkAutoPowerOn"},
        {"game_genre", "gameGenre"}, {"prevent_input_delay", "inputOptimization"},
        {"allm_setting", "enableALLM"}, {"digital_sound_output", "soundOutputDigital"},
        {"balance", "audioBalance"}, {"automatic_volume", "autoVolume"},
        {"earc", "eArcSupport"}, {"ip_control", "ipControl"},
        {"vrr_gsync", "gameOptimization"}, {"freesync_premium", "freesync"},
        {"dark_room_mode", "darkMode"}, {"black_stabilizer", "blackStabilizer"},
        {"white_stabilizer", "whiteStabilizer"}, {"ai_game_sound", "aigamesound"},
        {"screen_saver_ads", "screenSaverAd"}, {"home_sponsored_tiles", "homePromotion"},
        {"home_recommendations", "contentRecommendation"},
        {"ads_while_watching", "livePromotion"}, {"smart_tips", "aiNudge"},
        {"smart_tips_in_settings", "aiSettingsNudge"}
    };
    for (const auto &row : rows)
        if (id == row.first)
            return row.second;
    return nullptr;
}

Json findLgSettingRow(const Json &rows, const std::string &id) {
    if (!rows.isArray())
        return Json();
    for (const Json &row : rows.items())
        if (row["id"].isString() && row["id"].string() == id)
            return row;
    return Json();
}

void applyLgSetting(Json &conf, const std::string &entityId, const Json &rows) {
    const char *rowName = lgSettingRowForEntity(entityId);
    if (!rowName)
        return;
    const Json row = findLgSettingRow(rows, rowName);
    if (!row.isObject())
        return;
    if (conf["min"].isNumber() && row["min"].isNumber())
        conf.set("min", row["min"]);
    if (conf["max"].isNumber() && row["max"].isNumber())
        conf.set("max", row["max"]);
    if (conf["options"].isArray()) {
        std::vector<std::string> ids;
        Json labels = Json::object();
        for (const Json &choice : row["choices"].items()) {
            if (!choice["value"].isString())
                continue;
            const std::string value = choice["value"].string();
            ids.push_back(value);
            if (choice["label"].isString())
                labels.set(value, choice["label"]);
        }
        applyNamedSelect(conf, ids, labels,
            "(value_json.get('lgs') or {}).get(\"" + std::string(rowName) + "\")");
    }
}

bool explicitlyIncluded(const Json &entity, const HaDiscoveryOptions &options,
                        const std::set<std::string> &allowed) {
    const std::string type = entity["type"].string();
    const std::string id = entity["id"].string();
    if (options.applyCapabilities && !allowed.count(type + "." + id))
        return false;
    if (!options.allowPower && type == "button" &&
        (id == "restart" || id == "power_off" || id == "power_on"))
        return false;
    if (!options.piccap && type == "switch" && id == "piccap")
        return false;
    const char *lgRow = lgSettingRowForEntity(id);
    if (lgRow && options.lgSettings.valid() &&
        !findLgSettingRow(options.lgSettings, lgRow).isObject())
        return false;
    return true;
}

} // namespace

std::string haDefaultEntityId(const std::string &type, const std::string &deviceId,
                              const std::string &entityId) {
    std::string slug;
    bool previousUnderscore = false;
    for (unsigned char ch : deviceId) {
        const char lower = static_cast<char>(std::tolower(ch));
        const bool valid = (lower >= 'a' && lower <= 'z') ||
                           (lower >= '0' && lower <= '9') || lower == '_';
        if (valid) {
            slug += lower;
            previousUnderscore = lower == '_';
        } else if (!slug.empty() && !previousUnderscore) {
            slug += '_';
            previousUnderscore = true;
        }
    }
    while (!slug.empty() && slug.back() == '_')
        slug.pop_back();
    return type + "." + (slug.empty() ? std::string() : slug + "_") + entityId;
}

HaDiscoveryBatch buildHaDiscoveryBatch(const HaDiscoveryOptions &requested) {
    HaDiscoveryOptions options = requested;
    if (options.topicPrefix.empty())
        options.topicPrefix = "tv";
    if (options.discoveryPrefix.empty())
        options.discoveryPrefix = "homeassistant";
    if (options.deviceId.empty())
        options.deviceId = "lg_tv";

    const Json metadata = haEntityCatalogue();
    const HaEntityFilterResult filtered = options.applyCapabilities
        ? filterHaEntityCatalogue(metadata, options.capabilities)
        : HaEntityFilterResult{metadata, Json::array()};
    std::set<std::string> allowed;
    for (const Json &entity : filtered.published.items())
        allowed.insert(entity["type"].string() + "." + entity["id"].string());

    const Json templates = haDiscoveryTemplates();
    HaDiscoveryBatch batch;
    for (const Json &entity : metadata.items()) {
        if (!explicitlyIncluded(entity, options, allowed))
            batch.clear.push_back(discoveryTopic(options, entity["type"].string(),
                                                 entity["id"].string()));
    }

    static const char *const retired[][2] = {
        {"sensor", "oled_screen_shift"}, {"sensor", "oled_logo_dimming"},
        {"sensor", "oled_light"}, {"sensor", "hdmi_cable_errors"},
        {"sensor", "soc_current"}, {"binary_sensor", "standby_stuck"}
    };
    for (const auto &entity : retired)
        batch.clear.push_back(discoveryTopic(options, entity[0], entity[1]));

    for (const Json &item : templates.items()) {
        const std::string type = item["type"].string();
        const std::string id = item["id"].string();
        if (!explicitlyIncluded(item, options, allowed))
            continue;

        Json conf = replaceTopicPrefix(item["conf"], options.topicPrefix);
        if (id == "app" && options.installedApps.valid())
            applyInstalledApps(conf, options);
        else if (type == "select" && id == "picture_mode" &&
                 options.pictureModes.valid())
            applyPictureModes(conf, options.pictureModes);
        else if (id == "sound_output" && options.soundOutputs.valid())
            applySoundOutputs(conf, options.soundOutputs);
        applyLgSetting(conf, id, options.lgSettings);
        conf.set("unique_id", Json::text(options.deviceId + "_" + id));
        conf.set("default_entity_id", Json::text(
            haDefaultEntityId(type, options.deviceId, id)));
        conf.set("device", deviceFor(item["conf"]["device"], options.device,
                                     options.deviceId));
        batch.publish.push_back({discoveryTopic(options, type, id), conf.dump(false)});
    }
    return batch;
}

bool publishHaDiscoveryBatch(
    const HaDiscoveryBatch &batch,
    const std::function<bool(const std::string &, const std::string &, bool)> &publish) {
    if (!publish)
        return false;
    for (const std::string &topic : batch.clear)
        if (!publish(topic, "", true))
            return false;
    for (const HaDiscoveryMessage &message : batch.publish)
        if (!publish(message.topic, message.payload, true))
            return false;
    return true;
}

} // namespace daemon
} // namespace nativetv
