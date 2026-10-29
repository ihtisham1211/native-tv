#include "service/stats_fields.hpp"
#include "domain/names.hpp"
#include "service/preload.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>

namespace nativetv {
namespace service {
namespace {
// parseInt(value, 10) with a fallback for NaN.
long long toInt(const Json &value, long long fallback) {
    std::string text = value.isString() ? value.string()
        : value.isNumber() ? compactNumber(value.number(), 0) : std::string();
    size_t at = 0;
    while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at]))) ++at;
    bool negative = false;
    if (at < text.size() && (text[at] == '-' || text[at] == '+')) negative = text[at++] == '-';
    if (at >= text.size() || !std::isdigit(static_cast<unsigned char>(text[at]))) return fallback;
    long long n = 0;
    while (at < text.size() && std::isdigit(static_cast<unsigned char>(text[at])) && n < (1LL << 50))
        n = n * 10 + (text[at++] - '0');
    return negative ? -n : n;
}

std::string pad2(long long v) {
    return (v < 10 ? "0" : "") + std::to_string(v);
}

bool isNumber(const Json &value) { return value.isNumber(); }

Json numberOrNull(const Json &meta, const char *key, double scale) {
    return meta.isObject() && isNumber(meta[key]) ? Json::numeric(meta[key].number() / scale) : Json();
}

Json flagOrNull(const Json &value) {
    if (value.isBoolean()) return Json::flag(value.boolean());
    if (value.isNumber()) return Json::flag(value.number() != 0 && !std::isnan(value.number()));
    return Json();
}

Json textOrNull(const Json &value) {
    return value.isString() && !value.string().empty() ? value : Json();
}

std::string hexColor(const Json &value) {
    if (!value.isString()) return std::string();
    std::string text = value.string();
    size_t start = 0, end = text.size();
    while (start < end && std::isspace(static_cast<unsigned char>(text[start]))) ++start;
    while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    text = text.substr(start, end - start);
    if (text.size() != 4 && text.size() != 7) return std::string();
    if (text[0] != '#') return std::string();
    std::string digits;
    for (size_t i = 1; i < text.size(); ++i) {
        if (!std::isxdigit(static_cast<unsigned char>(text[i]))) return std::string();
        digits.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(text[i]))));
    }
    if (digits.size() == 3) {
        std::string expanded;
        for (char c : digits) { expanded.push_back(c); expanded.push_back(c); }
        digits = expanded;
    }
    return "#" + digits;
}

bool contains(const std::vector<std::string> &list, const std::string &value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

std::vector<std::string> strings(const Json &array) {
    std::vector<std::string> out;
    if (array.isArray())
        for (const Json &item : array.items())
            if (item.isString()) out.push_back(item.string());
    return out;
}

const char *const kSoundOutOrder[] = {
    "tv_speaker", "external_optical", "tv_external_speaker", "external_arc",
    "tv_speaker_external_arc", "bt_soundbar", "tv_speaker_bluetooth", "wow_cast",
    "tv_speaker_wow_cast", "lineout", "headphone", "usb_speaker", "tv_speaker_headphone",
    "mobile_phone", "wisa_speaker"};

bool soundOutputHidden(const std::string &id, const Json &cfg) {
    // Never on LG's menu, or offered only with a soundbar or WOWCAST connected
    // (which is not read here): listed only while in use.
    static const char *const kNever[] = {
        "tv_speaker_optical_arc", "tv_speakerbar", "builtin_soundbar", "tv_speakerbar_headphone",
        "bt_audio", "tv_speaker_external_arc", "tv_speaker_wow_cast"};
    for (const char *never : kNever)
        if (id == never) return true;
    const std::vector<std::string> bluetooth = strings(cfg["system.supportBluetoothFeatures"]);
    const bool haveBluetooth = cfg["system.supportBluetoothFeatures"].isArray();
    const std::vector<std::string> profile = strings(cfg["profile.list"]);
    const bool haveProfile = cfg["profile.list"].isArray();
    const auto says = [&cfg](const char *key, bool value) {
        return cfg.has(key) && cfg[key].isBoolean() && cfg[key].boolean() == value;
    };
    if (id == "bt_soundbar") return haveBluetooth && !contains(bluetooth, "btsound");
    if (id == "tv_speaker_bluetooth")
        return haveBluetooth && !contains(bluetooth, "bluetoothPlusTvSpeaker");
    if (id == "lineout")
        return says("tv.model.supportAudioLineOut", false) || says("tv.model.supportHeadPhone", false);
    if (id == "headphone" || id == "tv_speaker_headphone")
        return says("tv.model.supportHeadPhone", false);
    if (id == "external_optical" || id == "tv_external_speaker")
        return cfg.has("tv.model.supportOpticalJack") &&
               !(cfg["tv.model.supportOpticalJack"].isString() &&
                 cfg["tv.model.supportOpticalJack"].string() == "On");
    if (id == "wisa_speaker")
        return !(cfg["tv.model.supportWiSA"].isBoolean() && cfg["tv.model.supportWiSA"].boolean());
    if (id == "wow_cast")
        return !(cfg["com.webos.service.wowplay.supportWowCast"].isBoolean() &&
                 cfg["com.webos.service.wowplay.supportWowCast"].boolean());
    if (id == "usb_speaker")
        return !(haveProfile && contains(profile, "gaming") && contains(profile, "usbSoundOutput"));
    return false;
}

std::string formatSoundOutput(const std::string &output) {
    if (output.empty()) return "TV Speaker";
    return names::soundOutput(output).isObject() ? names::soundOutput(output)["display"].string()
                                                 : output;
}

long long daysMask(const Json &mask) { return toInt(mask, 0); }

Json timerFrom(const Json &s, const std::string &prefix) {
    if (!s.has(prefix + "Enable") || !s.has(prefix + "Hour")) return Json();
    const long long hour = toInt(s[prefix + "Hour"], 0) % 24;
    const long long minute = toInt(s[prefix + "Minute"], 0);
    Json days = Json::array();
    const long long mask = daysMask(s[prefix + "Weekday"]);
    for (int day = 0; day < 7; ++day)
        if (mask & (1LL << day)) days.add(Json::numeric(day));
    Json timer = Json::object();
    timer.set("enabled", Json::flag(s[prefix + "Enable"].isString() &&
                                    s[prefix + "Enable"].string() == "on"));
    timer.set("time", Json::text(pad2(hour) + ":" + pad2(minute)));
    timer.set("days", days);
    return timer;
}
} // namespace

Json statsSignalFormat(const Json &reply) {
    const Json sinks = reply["video"];
    Json info;
    if (sinks.isArray())
        for (const Json &sink : sinks.items())
            if (sink.isObject() && sink["connected"].isBoolean() && sink["connected"].boolean() &&
                sink["videoInfo"].isObject()) {
                info = sink["videoInfo"];
                break;
            }
    if (!info.valid()) return Json();
    const Json meta = info["HDMIHDRInfo"].isObject() ? info["HDMIHDRInfo"] : Json();
    const Json colorimetry = info["colormetry"].isString() && info["colormetry"].string() == "FUTURE"
        ? info["extendedColormetry"] : info["colormetry"];
    Json eotf;
    if (meta.valid() && isNumber(meta["EOTFtype"]))
        eotf = names::signalEotf(static_cast<unsigned>(meta["EOTFtype"].number()));
    Json hdrType;
    if (info["hdrType"].isString() && !info["hdrType"].string().empty())
        hdrType = names::signalHdrType(info["hdrType"].string());

    Json out = Json::object();
    out.set("type", hdrType.valid() ? hdrType["label"] : Json());
    out.set("type_display", hdrType.valid() ? hdrType["display"] : Json());
    out.set("eotf", eotf.valid() ? eotf["label"] : Json());
    out.set("colorimetry", textOrNull(colorimetry));
    out.set("encoding", textOrNull(info["pixelEncoding"]));
    out.set("max_luminance", numberOrNull(meta, "maxDisplayMasteringLuminance", 1));
    out.set("min_luminance", numberOrNull(meta, "minDisplayMasteringLuminance", 10000));
    out.set("max_cll", numberOrNull(meta, "maximumContentLightLevel", 1));
    out.set("max_fall", numberOrNull(meta, "maximumFrameAverageLightLevel", 1));
    out.set("game_mode", flagOrNull(info["isGameMode"]));
    out.set("freesync", flagOrNull(info["freesyncEnabled"]));
    return out;
}

std::string statsVolumeControl(const Json &vs, const Json &sound) {
    if (vs.isObject() && vs["adjustVolume"].isBoolean()) {
        if (vs["adjustVolume"].boolean() ||
            (vs["volumeSyncable"].isBoolean() ? vs["volumeSyncable"].boolean()
                                              : javascriptTruthy(vs["volumeSyncable"])))
            return "level";
        return javascriptTruthy(vs["externalDeviceControl"]) ? "steps" : "none";
    }
    if (sound.isObject() && isNumber(sound["volume"]) && sound["volume"].number() < 0) {
        const std::string scenario = javascriptTruthy(sound["scenario"])
            ? javascriptString(sound["scenario"]) : std::string();
        return scenario.find("arc") != std::string::npos ? "steps" : "none";
    }
    return "level";
}

std::string statsSoundRoute(const Json &vs, const Json &sound) {
    if (vs.isObject() && javascriptTruthy(vs["soundOutput"])) return javascriptString(vs["soundOutput"]);
    if (sound.isObject() && javascriptTruthy(sound["scenario"])) return javascriptString(sound["scenario"]);
    return std::string();
}

std::string statsAudioOutput(const Json &vs, const Json &sound) {
    if (vs.isObject() && javascriptTruthy(vs["soundOutput"]))
        return formatSoundOutput(javascriptString(vs["soundOutput"]));
    if (sound.isObject() && javascriptTruthy(sound["scenario"])) {
        std::string scenario = javascriptString(sound["scenario"]);
        if (scenario.compare(0, 13, "mastervolume_") == 0) scenario = scenario.substr(13);
        return formatSoundOutput(scenario);
    }
    return "Internal";
}

std::vector<std::string> statsOfferedSoundOutputs(const Json &values, const Json &cfgValue,
                                                  const std::string &current) {
    const Json cfg = cfgValue.isObject() ? cfgValue : Json::object();
    std::vector<std::string> offered;
    if (values.isArray())
        for (const Json &value : values.items()) {
            if (!value.isObject() || !value["value"].isString() ||
                !javascriptTruthy(value["visible"]) || !javascriptTruthy(value["active"]))
                continue;
            const std::string id = value["value"].string();
            if (contains(offered, id)) continue;
            if (id == current || !soundOutputHidden(id, cfg)) offered.push_back(id);
        }
    if (offered.empty()) return offered;
    if (!current.empty() && !contains(offered, current)) offered.push_back(current);
    const size_t orderSize = sizeof(kSoundOutOrder) / sizeof(kSoundOutOrder[0]);
    std::vector<std::pair<size_t, std::string>> keyed;
    for (size_t i = 0; i < offered.size(); ++i) {
        size_t at = orderSize + i;
        for (size_t k = 0; k < orderSize; ++k)
            if (offered[i] == kSoundOutOrder[k]) at = k;
        keyed.push_back({at, offered[i]});
    }
    std::stable_sort(keyed.begin(), keyed.end(),
                     [](const std::pair<size_t, std::string> &a,
                        const std::pair<size_t, std::string> &b) { return a.first < b.first; });
    std::vector<std::string> sorted;
    for (const auto &entry : keyed) sorted.push_back(entry.second);
    return sorted;
}

std::string statsClockTime(const Json &hour, const Json &minute) {
    return pad2(toInt(hour, 0)) + ":" + pad2(toInt(minute, 0));
}

Json statsTimersFromSettings(const Json &settings) {
    if (!settings.isObject()) return Json();
    Json on = timerFrom(settings, "onTimer");
    const Json off = timerFrom(settings, "offTimer");
    Json out = Json::object();
    if (on.valid()) {
        // Switched on by the timer, LG switches the TV off again after two hours
        // without a button press, while this is on.
        if (settings.has("autoOff2HourOnTimer"))
            on.set("autoOff", Json::flag(settings["autoOff2HourOnTimer"].isString() &&
                                         settings["autoOff2HourOnTimer"].string() == "on"));
        out.set("on", on);
    }
    if (off.valid()) out.set("off", off);
    return on.valid() || off.valid() ? out : Json();
}

Json statsAppList(const Json &raw, Json &titles) {
    if (!titles.isObject()) titles = Json::object();
    std::vector<std::pair<std::string, Json>> entries;
    std::map<std::string, bool> seen;
    if (raw.isArray())
        for (const Json &app : raw.items()) {
            if (!app.isObject()) continue;
            const std::string id = javascriptTruthy(app["id"]) ? javascriptString(app["id"]) : std::string();
            if (!id.empty() && javascriptTruthy(app["title"]))
                titles.set(id, Json::text(javascriptString(app["title"])));
            const bool hidden = app["visible"].isBoolean() && !app["visible"].boolean();
            if (id.empty() || hidden || id.compare(0, 23, "com.webos.app.container") == 0 ||
                seen[id])
                continue;
            seen[id] = true;
            Json entry = Json::object();
            const std::string title = javascriptTruthy(app["title"]) ? javascriptString(app["title"]) : id;
            entry.set("id", Json::text(id));
            entry.set("title", Json::text(title));
            const std::string color = hexColor(app["iconColor"]);
            if (!color.empty()) entry.set("iconColor", Json::text(color));
            entries.push_back({title, entry});
        }
    // Titles in dictionary order, as String.localeCompare orders them where the
    // runtime has collation: letters regardless of case first, then lower case
    // before upper where only the case differs.
    const auto lower = [](std::string text) {
        for (char &c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return text;
    };
    std::stable_sort(entries.begin(), entries.end(),
                     [&lower](const std::pair<std::string, Json> &a, const std::pair<std::string, Json> &b) {
                         const std::string left = lower(a.first), right = lower(b.first);
                         if (left != right) return left < right;
                         return a.first > b.first;
                     });
    Json list = Json::array();
    for (const auto &entry : entries) list.add(entry.second);
    return list;
}

} // namespace service
} // namespace nativetv
