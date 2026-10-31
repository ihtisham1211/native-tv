#include "daemon/stats_collector.hpp"
#include "domain/names.hpp"
#include "service/preload.hpp"
#include "service/stats_fields.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>

namespace nativetv {
namespace daemon {
namespace {
// The TV's Live screen asks twice a second. Only the /proc readings are made each time: LG's own services are
// cached for 3 s or more below.
const long long kStatsReuseMs = 400;
const size_t kTempHistoryMax = 120;
const long long kLiveReadTtlMs = 60000;
const long long kLivePowerTtlMs = 30000;
const long long kAppsScanMs = 60000;

const char *const kSettings = "com.webos.service.settings/getSystemSettings";

bool returnedFalse(const Json &reply) {
    return reply.isObject() && reply["returnValue"].isBoolean() && !reply["returnValue"].boolean();
}

// A reply that exists and has not said it failed.
bool good(const Json &reply) { return reply.isObject() && !returnedFalse(reply); }

std::string readTrimmed(const std::string &path) {
    std::ifstream file(path.c_str());
    if (!file) return std::string();
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string text = buffer.str();
    size_t start = 0, end = text.size();
    while (start < end && std::isspace(static_cast<unsigned char>(text[start]))) ++start;
    while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    return text.substr(start, end - start);
}

std::string rooted(const std::string &root, const std::string &path) {
    if (root.empty() || root == "/") return path;
    size_t end = root.size();
    while (end > 1 && root[end - 1] == '/') --end;
    return root.substr(0, end) + path;
}

Json settingsRequest(const char *category, std::initializer_list<const char *> keys = {}) {
    Json request = Json::object();
    request.set("category", Json::text(category));
    if (keys.size()) {
        Json list = Json::array();
        for (const char *key : keys) list.add(Json::text(key));
        request.set("keys", list);
    }
    return request;
}

bool jsOn(const Json &value) {
    return (value.isString() && value.string() == "on") || (value.isBoolean() && value.boolean());
}

std::string socArchName(const std::string &raw) {
    size_t a = 0, b = raw.size();
    while (a < b && raw[a] == '_') ++a;
    while (b > a && raw[b - 1] == '_') --b;
    std::string key = raw.substr(a, b - a);
    if (key.empty()) return std::string();
    for (char &c : key) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    static const struct { const char *key; const char *name; } kArch[] = {
        {"O22", "Alpha 9 Gen 5 (O22)"}, {"O20", "Alpha 9 Gen 3 (O20)"}, {"O18", "Alpha 9 Gen 1 (O18)"},
        {"M16P", "Alpha 7 (M16P)"}, {"M16PLUS", "Alpha 7 (M16P)"}};
    for (const auto &entry : kArch)
        if (key == entry.key) return entry.name;
    return key;
}

std::string webosVersion(const std::string &root, const std::string &sdk) {
    std::string raw = readTrimmed(rooted(root, "/etc/issue"));
    if (raw.empty()) raw = readTrimmed(rooted(root, "/etc/issue.net"));
    const auto digitsAfter = [](const std::string &text, const std::string &marker) {
        size_t at = text.find(marker);
        if (at == std::string::npos) return std::string();
        at += marker.size();
        while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at]))) ++at;
        std::string version;
        while (at < text.size() && (std::isdigit(static_cast<unsigned char>(text[at])) || text[at] == '.'))
            version.push_back(text[at++]);
        return version;
    };
    std::string version = digitsAfter(raw, "webOS TV");
    if (version.empty()) version = digitsAfter(raw, "webOS");
    if (!version.empty()) return version;
    version = digitsAfter(readTrimmed(rooted(root, "/etc/starfish-release")), "release");
    if (!version.empty()) return version;
    return sdk;
}

bool isScreenSaverState(const Json &powerState) {
    return powerState.isObject() && powerState["raw"].isString() &&
           names::powerState(powerState["raw"])["display"].string() ==
               names::powerState(Json::text("Screen Saver"))["display"].string();
}

std::string lowerText(std::string text) {
    for (char &c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

int toInt(const Json &value, int fallback) {
    if (value.isNumber()) return static_cast<int>(value.number());
    if (value.isString()) {
        const std::string text = value.string();
        char *end = nullptr;
        const long parsed = std::strtol(text.c_str(), &end, 10);
        return end != text.c_str() ? static_cast<int>(parsed) : fallback;
    }
    return fallback;
}
} // namespace

StatsCollector::~StatsCollector() {
    if (earlyThread_.joinable()) earlyThread_.join();
}

Json StatsCollector::early(const Json &system) {
    if (collected_) return Json();
    if (!earlyRunning_.exchange(true)) {
        if (earlyThread_.joinable()) earlyThread_.join();
        earlyThread_ = std::thread([this, system]() {
            (void)collect(system);
            earlyRunning_ = false;
        });
    }
    std::lock_guard<std::mutex> lock(partialMutex_);
    if (!partial_.valid()) return Json();
    // A copy of a Json is another handle on the same object: marking it loading
    // must not mark the stats the read goes on to return.
    Json copy = Json::parse(partial_.dump());
    copy.set("loading", Json::flag(true));
    return copy;
}

StatsCollector::StatsCollector(StatsDeps deps) : deps_(std::move(deps)) {
    hardware_.set("webos", Json());
    hardware_.set("socArch", Json());
    hardware_.set("ram", Json());
    hardware_.set("refreshRate", Json());
    hardware_.set("eyeSensor", Json());
    hardware_.set("cell", Json());
    hardware_.set("tconFirmware", Json());
    hardware_.set("tconModule", Json());
}

bool StatsCollector::cached(const std::string &uri, const Json &payload, long long ttlMs,
                            Json &reply) {
    const std::string key = uri + "|" + (payload.valid() ? payload.dump() : "{}");
    const long long now = deps_.nowMs();
    const auto hit = cache_.find(key);
    if (hit != cache_.end()) {
        const long long age = now - hit->second.at;
        if (age >= 0 && age < ttlMs) {
            reply = hit->second.reply;
            return true;
        }
    }
    Json answer;
    if (!deps_.luna(uri, payload.valid() ? payload : Json::object(), answer)) {
        reply = Json();
        return false;
    }
    // Only a real answer is worth pinning; a failed read should be retried.
    if (answer.isObject()) cache_[key] = Cached{now, answer};
    reply = answer;
    return true;
}

long long StatsCollector::liveTtl(const std::string &group, long long pollMs, long long liveMs) {
    return deps_.liveGroup && deps_.liveGroup(group) ? liveMs : pollMs;
}

// A collection holds mutex_ for its whole run, which can take seconds on a cold
// TV; the callers of these (a control write, a live event) must not wait on it.
// They note the invalidation and the next collection applies it, and one that
// ran meanwhile does not keep its result.
void StatsCollector::expire() {
    std::lock_guard<std::mutex> lock(pendingMutex_);
    pending_.expire = true;
}

void StatsCollector::clearCache() {
    std::lock_guard<std::mutex> lock(pendingMutex_);
    pending_.expire = true;
    pending_.clear = true;
}

void StatsCollector::forget(const std::vector<std::string> &substrings) {
    std::lock_guard<std::mutex> lock(pendingMutex_);
    pending_.expire = true;
    if (substrings.empty()) pending_.forgetAll = true;
    else pending_.forget.insert(pending_.forget.end(), substrings.begin(), substrings.end());
}

void StatsCollector::noteVolumeRefused() {
    std::lock_guard<std::mutex> lock(pendingMutex_);
    pending_.expire = true;
    pending_.volumeRefused = true;
}

void StatsCollector::applyPending() {
    Pending taken;
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        std::swap(taken, pending_);
    }
    if (taken.expire) lastStats_ = Json();
    if (taken.clear) {
        lastAppsScan_ = 0;
        lastLightSensorProbe_ = 0;
    }
    if (taken.volumeRefused) {
        haveVolumeRefusedRoute_ = !lastSoundRoute_.empty();
        volumeRefusedRoute_ = lastSoundRoute_;
    }
    if (taken.forgetAll) cache_.clear();
    for (auto it = cache_.begin(); it != cache_.end() && !taken.forget.empty();) {
        bool drop = false;
        for (const std::string &match : taken.forget)
            if (it->first.find(match) != std::string::npos) drop = true;
        it = drop ? cache_.erase(it) : std::next(it);
    }
}

Json StatsCollector::hardware() const { return hardware_; }

Json StatsCollector::discoveryInputs() const {
    std::lock_guard<std::mutex> lock(deviceMutex_);
    return discoveryShared_.valid() ? Json::parse(discoveryShared_.dump()) : Json();
}

std::string StatsCollector::deviceModel() {
    {
        std::unique_lock<std::mutex> collecting(mutex_, std::try_to_lock);
        if (collecting.owns_lock() && !deviceDetected_ && deps_.luna) detectDevice();
    }
    Json source;
    {
        std::lock_guard<std::mutex> lock(deviceMutex_);
        if (deviceShared_.valid()) source = Json::parse(deviceShared_.dump());
    }
    if (!source.valid() && deps_.config) source = deps_.config()["device"];
    return service::javascriptTruthy(source["model"]) ? service::javascriptString(source["model"])
                                                       : std::string();
}

Json StatsCollector::appTitles() {
    std::lock_guard<std::mutex> lock(deviceMutex_);
    return Json::parse(titlesShared_.dump());
}

Json StatsCollector::deviceIdentity() {
    {
        std::unique_lock<std::mutex> collecting(mutex_, std::try_to_lock);
        if (collecting.owns_lock() && !deviceDetected_ && deps_.luna) detectDevice();
    }
    Json source;
    {
        std::lock_guard<std::mutex> lock(deviceMutex_);
        if (deviceShared_.valid()) source = Json::parse(deviceShared_.dump());
    }
    if (!source.valid() && deps_.config) source = deps_.config()["device"];
    Json out = Json::object();
    out.set("id", service::javascriptTruthy(source["id"]) ? source["id"] : Json::text("lg_tv"));
    out.set("name", service::javascriptTruthy(source["name"]) ? source["name"] : Json::text("LG webOS TV"));
    return out;
}

bool StatsCollector::alwaysOnSupported() {
    if (alwaysOnSupport_ != -1) return alwaysOnSupport_ == 1;
    Json request = Json::object();
    request.set("category", Json::text("general"));
    Json keys = Json::array();
    keys.add(Json::text("alwaysOn"));
    request.set("keys", keys);
    Json reply;
    const bool answered = deps_.luna("com.webos.service.settings/getSystemSettingDesc", request, reply);
    const Json description = good(reply) && reply["results"].isArray() ? reply["results"][0] : Json();
    // Remembered only once the TV has answered either way, not after a timeout.
    if (description.isObject())
        alwaysOnSupport_ = description["ui"]["visible"].isBoolean() && !description["ui"]["visible"].boolean() ? 0 : 1;
    else if (answered && reply.isObject())
        alwaysOnSupport_ = 0;
    return alwaysOnSupport_ == 1;
}

void StatsCollector::detectHardware(const std::string &sdkVersion) {
    hardware_.set("webos", [&]() {
        const std::string version = webosVersion(deps_.systemRoot, sdkVersion);
        return version.empty() ? Json() : Json::text(version);
    }());
    const std::string environment = readTrimmed(rooted(deps_.systemRoot, "/var/luna/preferences/environmentCondition"));
    if (!environment.empty()) {
        const Json env = Json::parse(environment);
        if (env.isObject()) {
            std::string board = env["boardTypeStr"].string();
            if (board.empty()) board = env["socChip"].string();
            if (board.empty()) board = readTrimmed(rooted(deps_.systemRoot, "/proc/lg/base/chip_name"));
            if (!board.empty()) {
                const std::string arch = socArchName(board);
                if (!arch.empty()) hardware_.set("socArch", Json::text(arch));
            }
            if (service::javascriptTruthy(env["ddrSize"])) hardware_.set("ram", env["ddrSize"]);
            if (service::javascriptTruthy(env["panelOutputFrameRate"]))
                hardware_.set("refreshRate", Json::text(service::javascriptString(env["panelOutputFrameRate"]) + " Hz"));
            if (service::javascriptTruthy(env["digitalEyeMode"]))
                hardware_.set("eyeSensor", env["digitalEyeMode"]);
            else if (env["isDigitalEye"].isString() && env["isDigitalEye"].string() == "true")
                hardware_.set("eyeSensor", Json::text("Digital Eye"));
        }
    }
    if (!service::javascriptTruthy(hardware_["socArch"])) {
        const std::string chip = readTrimmed(rooted(deps_.systemRoot, "/proc/lg/base/chip_name"));
        if (!chip.empty()) hardware_.set("socArch", Json::text(socArchName(chip)));
    }
    Json cell;
    if (deps_.luna("com.webos.service.panelcontroller/getOledCellInfo", Json::object(), cell) &&
        cell["cellInfo"].valid() && service::javascriptTruthy(cell["cellInfo"]))
        hardware_.set("cell", cell["cellInfo"]);
    Json tcon;
    if (deps_.luna("com.webos.service.panelcontroller/getOledTconInfo", Json::object(), tcon) &&
        tcon["tconParamForInstart"].isObject()) {
        const Json info = tcon["tconParamForInstart"];
        hardware_.set("tconFirmware", service::javascriptTruthy(info["tconFpgaFirmwareVer"])
            ? info["tconFpgaFirmwareVer"] : Json());
        hardware_.set("tconModule", service::javascriptTruthy(info["tconModuleInfo"])
            ? info["tconModuleInfo"] : Json());
    }
}

void StatsCollector::detectDevice() {
    if (deviceDetected_) return;
    Json request = Json::object();
    Json keys = Json::array();
    for (const char *key : {"modelName", "firmwareVersion", "boardType", "sdkVersion"})
        keys.add(Json::text(key));
    request.set("keys", keys);
    Json reply;
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (deps_.luna("com.webos.service.tv.systemproperty/getSystemProperties", request, reply) &&
            service::javascriptTruthy(reply["modelName"]))
            break;
    }
    const Json config = deps_.config()["device"];
    Json device = config.isObject() ? config : Json::object();
    // What the server filled in at start: the TV's own name for the model, unless
    // the owner gave one.
    if (service::javascriptTruthy(reply["modelName"])) {
        const std::string model = service::javascriptString(reply["modelName"]);
        const std::string configuredModel = service::javascriptTruthy(device["model"])
            ? service::javascriptString(device["model"]) : std::string();
        if (configuredModel.empty() || configuredModel == "OLED65B8SLC" || configuredModel == "webOS TV")
            device.set("model", Json::text(model));
        const std::string configuredName = service::javascriptTruthy(device["name"])
            ? service::javascriptString(device["name"]) : std::string();
        if (configuredName.empty() || configuredName == "LG webOS TV" || configuredName == "LG OLED B8 TV")
            device.set("name", Json::text("LG " + model));
        if (service::javascriptTruthy(reply["firmwareVersion"]))
            device.set("sw_version", reply["firmwareVersion"]);
    }
    if (!service::javascriptTruthy(device["name"])) device.set("name", Json::text("LG webOS TV"));
    if (!service::javascriptTruthy(device["model"])) device.set("model", Json::text("webOS TV"));
    device_ = device;
    {
        std::lock_guard<std::mutex> lock(deviceMutex_);
        deviceShared_ = Json::parse(device.dump());
    }
    detectHardware(reply["sdkVersion"].isString() ? reply["sdkVersion"].string() : std::string());
    deviceDetected_ = true;
}

Json StatsCollector::remoteInfo(const Json &system) {
    Json file = system["remote"].isObject() ? system["remote"] : lastRemoteFacts_;
    if (!file.isObject()) return Json();
    lastRemoteFacts_ = file;
    const Json live = deps_.remoteBattery ? deps_.remoteBattery() : Json();
    const bool moduleOwns = live.isObject();
    const Json level = moduleOwns ? live["level"] : file["battery"];
    const bool connected = moduleOwns ? live["connected"].boolean()
        : (file["connected"].isBoolean() ? file["connected"].boolean() : false);
    const bool ble = moduleOwns ? live["ble"].boolean()
        : (file["bluetooth"].isBoolean() ? file["bluetooth"].boolean() : service::javascriptTruthy(file["mac"]));
    Json signal;
    if (!moduleOwns && file["signal"].isString()) signal = file["signal"];
    else if (moduleOwns && live["signal"].isString()) signal = live["signal"];
    Json firmware = service::javascriptTruthy(file["firmware"]) ? file["firmware"]
        : (moduleOwns && live["firmware"].valid() ? live["firmware"] : Json());
    Json out = Json::object();
    out.set("battery", level.valid() ? level : file["battery"]);
    out.set("model", file["model"]);
    out.set("mac", file["mac"]);
    out.set("firmware", firmware);
    out.set("paired", file["paired"]);
    out.set("connected", Json::flag(connected));
    out.set("bluetooth", Json::flag(!moduleOwns && file["bluetooth"].isBoolean()
        ? file["bluetooth"].boolean() : (service::javascriptTruthy(file["mac"]) || ble)));
    out.set("signal", signal);
    return out;
}

void StatsCollector::refreshInputNames() {
    const long long now = deps_.nowMs();
    if (now - lastInputScan_ < 60000 && inputNames_.keys().size() > 0) return;
    Json reply;
    if (!deps_.luna("com.webos.service.eim/getAllInputStatus", Json::object(), reply)) return;
    if (!reply["devices"].isArray() || reply["devices"].size() == 0) return;
    for (const Json &device : reply["devices"].items()) {
        const Json key = names::hdmiKeyForPortId(device["id"].string());
        if (!key.isString()) continue;
        inputNames_.set(key.string(), service::javascriptTruthy(device["label"])
            ? device["label"] : Json::text(names::input(key.string())["display"].string()));
        const std::string shortId = service::javascriptTruthy(device["appId"])
            ? names::shortAppId(device["appId"].string()) : key.string();
        if (shortId != key.string()) inputAliases_.set(shortId, key);
    }
    lastInputScan_ = now;
}

Json StatsCollector::refreshInstalledApps() {
    const long long now = deps_.nowMs();
    if (installedApps_.size() > 0 && now >= lastAppsScan_ && now - lastAppsScan_ < kAppsScanMs)
        return installedApps_;
    // appTitles() is read from other threads, so the titles are published as a copy.
    const auto publishTitles = [this]() {
        std::lock_guard<std::mutex> lock(deviceMutex_);
        titlesShared_ = Json::parse(appTitles_.dump());
    };
    Json reply;
    if (!deps_.luna("com.webos.applicationManager/listApps", Json::object(), reply)) return installedApps_;
    Json raw = reply["launchPoints"].isArray() ? reply["launchPoints"]
        : reply["apps"].isArray() ? reply["apps"] : Json();
    if (raw.isArray() && raw.size() > 0) {
        installedApps_ = service::statsAppList(raw, appTitles_);
        publishTitles();
        lastAppsScan_ = deps_.nowMs();
        return installedApps_;
    }
    Json points;
    if (deps_.luna("com.webos.applicationManager/listLaunchPoints", Json::object(), points)) {
        const Json rawPoints = points["launchPoints"].isArray() ? points["launchPoints"]
            : points["apps"].isArray() ? points["apps"] : Json();
        if (rawPoints.isArray()) {
            installedApps_ = service::statsAppList(rawPoints, appTitles_);
            publishTitles();
            lastAppsScan_ = deps_.nowMs();
        }
    }
    return installedApps_;
}

Json StatsCollector::pictureModes() {
    Json request = Json::object();
    request.set("category", Json::text("picture"));
    request.set("key", Json::text("pictureMode"));
    Json reply;
    (void)cached("com.webos.service.settings/getSystemSettingValues", request, 10000, reply);
    Json modes = Json::array();
    const Json values = reply["values"]["arrayExt"];
    if (values.isArray())
        for (const Json &value : values.items())
            if (value["visible"].isBoolean() && value["visible"].boolean() &&
                !(value["active"].isBoolean() && !value["active"].boolean())) {
                Json mode = Json::object();
                mode.set("value", value["value"]);
                mode.set("label", Json::text(names::pictureMode(
                    service::javascriptTruthy(value["value"]) ? service::javascriptString(value["value"])
                                                              : std::string("standard"))["display"].string()));
                modes.add(mode);
            }
    if (modes.size() > 0) lastPictureModes_ = modes;
    return modes;
}

void StatsCollector::soundOutputs(const std::string &current, Json &out) {
    Json valuesRequest = Json::object();
    valuesRequest.set("category", Json::text("sound"));
    valuesRequest.set("key", Json::text("soundOutput"));
    Json values;
    (void)cached("com.webos.service.settings/getSystemSettingValues", valuesRequest, 60000, values);
    Json configRequest = Json::object();
    Json configNames = Json::array();
    for (const char *name : {"tv.model.supportHeadPhone", "tv.model.supportOpticalJack",
                             "tv.model.supportAudioLineOut", "tv.model.supportWiSA",
                             "system.supportBluetoothFeatures",
                             "com.webos.service.wowplay.supportWowCast", "profile.list"})
        configNames.add(Json::text(name));
    configRequest.set("configNames", configNames);
    Json configs;
    (void)cached("com.webos.service.config/getConfigs", configRequest, 300000, configs);
    const std::vector<std::string> ids = service::statsOfferedSoundOutputs(
        values["values"]["arrayExt"], configs["configs"], current);
    if (ids.empty()) return;
    Json list = Json::array();
    for (const std::string &id : ids) {
        Json entry = Json::object();
        entry.set("id", Json::text(id));
        entry.set("name", Json::text(names::soundOutput(id)["display"].string()));
        list.add(entry);
    }
    lastSoundOutputs_ = list;
    out.set("outputs", list);
}

bool StatsCollector::media(Json &out) {
    const Json live = deps_.liveMedia ? deps_.liveMedia() : Json();
    if (live.isObject()) {
        out = live;
        return true;
    }
    Json idle = Json::object();
    idle.set("state", Json::text("idle"));
    idle.set("playerType", Json());
    idle.set("fullScreen", Json::flag(false));
    const auto fromMediaService = [&]() {
        Json reply;
        if (!cached("com.webos.media/getForegroundAppInfo", Json::object(), 4000, reply) ||
            !good(reply) || !reply["foregroundAppInfo"].isArray())
            return false;
        for (const Json &app : reply["foregroundAppInfo"].items()) {
            if (!service::javascriptTruthy(app["playState"])) continue;
            out = Json::object();
            out.set("state", Json::text(service::javascriptString(app["playState"])));
            out.set("playerType", service::javascriptTruthy(app["type"]) ? app["type"] : Json());
            out.set("fullScreen", Json::flag(true));
            return true;
        }
        out = idle;
        return true;
    };
    Json acb;
    const bool answered = cached("com.webos.service.acb/getForegroundAppInfo", Json::object(), 4000, acb);
    const Json pipe = answered && acb["acbs"].isArray() ? acb["acbs"][0] : Json();
    if (pipe.isObject() && service::javascriptTruthy(pipe["playStateNow"])) {
        out = Json::object();
        out.set("state", Json::text(service::javascriptString(pipe["playStateNow"])));
        out.set("playerType", service::javascriptTruthy(pipe["playerType"]) ? pipe["playerType"] : Json());
        out.set("fullScreen", Json::flag(!(pipe["isFullScreen"].isBoolean() && !pipe["isFullScreen"].boolean())));
        return true;
    }
    if (answered && good(acb) && acb["acbs"].isArray()) {
        out = idle;
        return true;
    }
    return fromMediaService();
}

bool StatsCollector::probeLightSensor(Json &out) {
    const long long now = deps_.nowMs();
    if (!hasLightSensor_ && lightSensorFailures_ >= 3 && now - lastLightSensorProbe_ < 600000) {
        out.set("lightSensor", Json());
        out.set("backlight", Json());
        return true;
    }
    lastLightSensorProbe_ = now;
    Json reply;
    (void)cached("com.webos.service.tv.display/getLightSensorData", Json::object(), 30000, reply);
    if (!reply.isObject() || returnedFalse(reply)) ++lightSensorFailures_;
    else lightSensorFailures_ = 0;
    Json lux;
    for (const Json &datum : reply["sensorData"].items()) {
        const std::string property = datum["property"].string();
        if (property == "visibleLuminance" || property == "luminance")
            if (datum["value"].isNumber() && datum["value"].number() != 65535) lux = datum["value"];
    }
    if (lux.valid()) {
        Json sensor = Json::object();
        sensor.set("lux", lux);
        out.set("lightSensor", sensor);
        hasLightSensor_ = true;
    } else {
        out.set("lightSensor", Json());
    }
    out.set("backlight", reply["backlightValue"].isNumber() ? reply["backlightValue"] : Json());
    return true;
}

Json StatsCollector::collect(const Json &system) {
    std::lock_guard<std::mutex> lock(mutex_);
    applyPending();
    const long long started = deps_.nowMs();
    if (lastStats_.valid() && started >= lastStatsAt_ && started - lastStatsAt_ < kStatsReuseMs)
        return lastStats_;
    if (!system["ok"].boolean()) return system;

    detectDevice();
    if (!alwaysOnAsked_) {
        alwaysOnAsked_ = true;
        (void)alwaysOnSupported();
    }

    // A Bluetooth audio device connected: choosing it without one opens the TV's
    // pairing prompt.
    {
        Json bluetooth;
        if (cached("com.webos.service.bluetooth2/device/getStatus", Json::object(), 60000, bluetooth) &&
            good(bluetooth) && bluetooth["devices"].isArray()) {
            bool any = false;
            for (const Json &device : bluetooth["devices"].items()) {
                const Json profiles = device["connectedProfiles"];
                if (!profiles.isArray() || profiles.size() == 0) continue;
                const long long deviceClass = static_cast<long long>(
                    device["classOfDevice"].isNumber() ? device["classOfDevice"].number()
                    : device["classOfDevice"].isString() ? std::strtod(device["classOfDevice"].string().c_str(), nullptr)
                                                         : 0);
                if (((deviceClass >> 8) & 0x1f) == 4) any = true;
            }
            btAudio_ = any;
            btAudioKnown_ = true;
        }
    }
    refreshInputNames();

    const Json cpuInfo = system["cpu"].isObject() ? system["cpu"] : Json::object();
    const Json times = cpuInfo["times"].isObject() ? cpuInfo["times"] : Json::object();
    long long coresTotal = static_cast<long long>(cpuInfo["coresTotal"].number(0));
    Json coreLoads = cpuInfo["coreLoads"].isArray() ? cpuInfo["coreLoads"] : Json::array();
    const Json window = cpuInfo["statsWindow"].isObject() ? cpuInfo["statsWindow"] : Json::parse("{\"cores\":{},\"overall\":null}");
    {
        std::vector<std::pair<int, std::string>> measured;
        for (const std::string &core : window["cores"].keys())
            measured.push_back({std::atoi(core.c_str()), core});
        std::sort(measured.begin(), measured.end());
        if (!measured.empty()) {
            coreLoads = Json::array();
            for (const auto &core : measured) coreLoads.add(window["cores"][core.second]);
            if (!coresTotal) coresTotal = static_cast<long long>(measured.size());
        }
        if (!coresTotal) coresTotal = static_cast<long long>(coreLoads.size());
    }
    const Json net = system["net"];
    const Json rate = net.isObject() && net.has("rate") ? net["rate"] : Json();
    const Json engine = system["pictureEngine"];
    Json engineInfo;
    if (engine.isObject()) {
        engineInfo = Json::object();
        engineInfo.set("colorimetry", engine["colorimetry"]);
        engineInfo.set("hdr_mode", engine["hdr_mode"]);
    }
    const double uptime = system["uptime"].number(0);

    Json out = Json::object();
    out.set("ok", Json::flag(true));
    out.set("time", Json::numeric(static_cast<double>(deps_.nowMs())));
    out.set("tvwebVersion", Json::text(deps_.version));
    {
        Json device = Json::object();
        device.set("id", service::javascriptTruthy(device_["id"]) ? device_["id"] : Json::text("lg_tv"));
        device.set("name", service::javascriptTruthy(device_["name"]) ? device_["name"] : Json::text("LG webOS TV"));
        device.set("model", service::javascriptTruthy(device_["model"]) ? device_["model"] : Json::text("webOS TV"));
        out.set("device", device);
        Json systemInfo = Json::object();
        systemInfo.set("webos", hardware_["webos"]);
        systemInfo.set("firmware", service::javascriptTruthy(device_["sw_version"]) ? device_["sw_version"] : Json());
        out.set("system", systemInfo);
        Json hardware = Json::object();
        hardware.set("webos", hardware_["webos"]);
        hardware.set("soc_arch", hardware_["socArch"]);
        hardware.set("ram", hardware_["ram"]);
        hardware.set("refresh_rate", hardware_["refreshRate"]);
        hardware.set("eye_sensor", hardware_["eyeSensor"]);
        out.set("hardware", hardware);
    }
    if (service::javascriptTruthy(hardware_["cell"])) {
        Json silicon = Json::object();
        silicon.set("cell", hardware_["cell"]);
        silicon.set("tcon_firmware", hardware_["tconFirmware"]);
        silicon.set("tcon_module", hardware_["tconModule"]);
        out.set("panel_silicon", silicon);
    } else {
        out.set("panel_silicon", Json());
    }
    out.set("remote", remoteInfo(system));
    out.set("temp", system["temp"]);
    out.set("tempMillidegrees", system["tempMillidegrees"]);
    {
        const double temp = system["temp"].number(0);
        if (system["temp"].isNumber() && !std::isnan(temp) && temp > 0) {
            tempHistory_.push_back(temp);
            if (tempHistory_.size() > kTempHistoryMax) tempHistory_.pop_front();
        }
        Json history = Json::array();
        for (double value : tempHistory_) history.add(Json::numeric(value));
        out.set("temps", history);
    }
    if (window["overall"].isNumber()) {
        out.set("load", window["overall"]);
    } else if (coreLoads.size() > 0) {
        double sum = 0;
        for (const Json &load : coreLoads.items()) sum += load.number();
        out.set("load", Json::numeric(std::floor(sum / static_cast<double>(coreLoads.size()) + 0.5)));
    } else {
        out.set("load", cpuInfo["currentLoad"]);
    }
    if (coreLoads.size() > 0) {
        double peak = -1e300;
        for (const Json &load : coreLoads.items()) peak = std::max(peak, load.number());
        out.set("loadPeak", Json::numeric(peak));
    } else {
        out.set("loadPeak", cpuInfo["currentLoad"]);
    }
    out.set("mhz", system["mhz"]);
    out.set("cpuHz", system["cpuHz"]);
    out.set("cpuTimes", times["cpus"]);
    out.set("cores", coreLoads);
    out.set("coresTotal", Json::numeric(static_cast<double>(coresTotal)));
    out.set("mem", system["mem"]);
    out.set("swap", system["swap"]);
    out.set("uptime", Json::numeric(uptime));
    {
        const long long computed = deps_.nowMs() - static_cast<long long>(uptime * 1000);
        if (std::llabs(computed - bootEpoch_) > 30000) bootEpoch_ = computed;
        out.set("bootTime", Json::text(deps_.isoTime(bootEpoch_)));
    }
    out.set("btime", times["btime"]);
    out.set("loadavg", system["loadavg"]);
    out.set("wifi", system["wifi"]);
    out.set("net", rate);
    if (net.isObject()) {
        Json total = Json::object();
        total.set("rx", net["rx"]);
        total.set("tx", net["tx"]);
        total.set("iface", net["iface"]);
        out.set("netTotal", total);
        out.set("mac", net["mac"]);
    } else {
        out.set("netTotal", Json());
        out.set("mac", Json());
    }
    const std::string address = deps_.address4 ? deps_.address4() : std::string();
    out.set("address", address.empty() ? Json() : Json::text(address));
    {
        const std::string host = address.empty() || !deps_.hostname ? std::string() : deps_.hostname(address);
        out.set("hostname", host.empty() ? Json() : Json::text(host));
    }
    out.set("emmc", system["emmc"]);
    out.set("gpuMhz", system["gpuMhz"]);
    out.set("gpuHz", system["gpuHz"]);
    out.set("appStorage", system["appStorage"]);
    out.set("signal", Json());
    out.set("signal_timing", Json());
    out.set("hdmi_diag", Json());
    out.set("source_frame_rate", Json());
    out.set("signal_format", Json());
    out.set("hdmi_links", Json());
    out.set("hdmi_sources", Json());
    out.set("picture_engine", engineInfo);
    out.set("colorimetry", engineInfo.isObject() ? engineInfo["colorimetry"] : Json());
    // A copy: the scan sets keys in inputNames_ in place, and an earlier answer
    // may still be on its way out.
    out.set("inputs", Json::parse(inputNames_.dump()));

    // What is known so far, for the first dashboard load to show while the
    // Luna reads below are made (early()).
    {
        std::lock_guard<std::mutex> lock(partialMutex_);
        partial_ = Json::parse(out.dump());  // not `out` itself, which the rest of the read fills in
    }
    struct PartialClear {
        StatsCollector &self;
        ~PartialClear() {
            std::lock_guard<std::mutex> lock(self.partialMutex_);
            self.partial_ = Json();
        }
    } partialClear{*this};

    // The receiver behind each HDMI input, and the links and powered sources on them.
    const Json receiverMap = deps_.hdmiReceiverMap ? deps_.hdmiReceiverMap() : Json::object();
    const Json facts = system["hdmiPortFacts"];
    const auto factFor = [&facts](int receiver) {
        return facts.isArray() && receiver >= 0 && receiver < 4 ? facts[static_cast<size_t>(receiver)] : Json();
    };
    {
        Json links = Json::array();
        Json sources = Json::array();
        for (int input = 1; input <= 4; ++input) {
            const Json receiverValue = receiverMap[std::to_string(input)];
            if (!receiverValue.isNumber()) continue;
            const int receiver = static_cast<int>(receiverValue.number());
            const Json fact = factFor(receiver);
            const Json link = fact.isObject() ? fact["link"] : Json();
            if (link.isObject() && link["locked"].boolean() && service::javascriptTruthy(link["phy_mode"])) {
                Json entry = Json::object();
                entry.set("phy_mode", link["phy_mode"]);
                entry.set("chroma", link["chroma"]);
                entry.set("hdcp", link["hdcp"]);
                entry.set("tmds_clock_khz", link["tmds_clock_khz"]);
                entry.set("qms", link["qms"]);
                entry.set("input", Json::numeric(input));
                entry.set("receiver", Json::numeric(receiver));
                links.add(entry);
            }
            if (fact.isObject() && fact["sourcePowered"].isBoolean()) {
                Json entry = Json::object();
                entry.set("input", Json::numeric(input));
                entry.set("receiver", Json::numeric(receiver));
                entry.set("powered", fact["sourcePowered"]);
                sources.add(entry);
            }
        }
        out.set("hdmi_links", links);
        out.set("hdmi_sources", sources);
    }

    // In LG's Always Ready display the TV is switched off to the user, but
    // power/getPowerState still reads "Active"; only power2 has a sub state for it.
    bool alwaysReadyShowing = false;
    if (alwaysReadyScreenOn_) {
        Json power2;
        alwaysReadyShowing = deps_.luna("com.webos.service.tvpower/power2/getPowerState", Json::object(), power2) &&
                             power2["sub state"].isString() &&
                             power2["sub state"].string() == "always on display";
    }
    Json power;
    {
        const std::string uri = "com.webos.service.tvpower/power/getPowerState";
        if (deps_.liveGroup && deps_.liveGroup("power")) (void)cached(uri, Json::object(), kLivePowerTtlMs, power);
        else (void)deps_.luna(uri, Json::object(), power);
    }
    std::string rawPower = power.isObject()
        ? (service::javascriptTruthy(power["state"]) ? service::javascriptString(power["state"])
           : service::javascriptTruthy(power["processing"]) ? service::javascriptString(power["processing"])
                                                            : std::string())
        : std::string();
    if (alwaysReadyShowing && rawPower == "Active") rawPower = "Always Ready";
    // No answer is not "off": left out, the live state keeps what it last knew.
    Json powerState;
    if (!rawPower.empty()) {
        const Json mapped = names::powerState(Json::text(rawPower));
        powerState = Json::object();
        powerState.set("raw", Json::text(rawPower));
        powerState.set("label", mapped["display"]);
        powerState.set("systemOn", mapped["systemOn"]);
        powerState.set("screenOn", mapped["screenOn"]);
        const Json on = deps_.powerOn ? deps_.powerOn() : Json();
        powerState.set("onReason", on.isObject() ? on["onReason"] : Json());
        powerState.set("onTime", on.isObject() ? on["onTime"] : Json());
    }
    out.set("powerState", powerState);
    const bool screenSaver = isScreenSaverState(powerState) ||
                             (deps_.screensaverLaunched && deps_.screensaverLaunched());
    out.set("screenSaver", Json::flag(screenSaver));
    out.set("screensaverMode", Json::text(deps_.screensaverMode ? deps_.screensaverMode() : "stock"));
    out.set("screensaverLevel", Json::text(deps_.screensaverLevel ? deps_.screensaverLevel() : "dim"));

    // The whole category, for the sleep timer and LG's On and Off Timers.
    {
        Json time;
        (void)cached(kSettings, settingsRequest("time"), 30000, time);
        out.set("sleepTimer", Json::text(service::javascriptTruthy(time["settings"]["sleepTimer"])
            ? service::javascriptString(time["settings"]["sleepTimer"]) : "off"));
        const Json timers = service::statsTimersFromSettings(
            time.isObject() && !returnedFalse(time) ? time["settings"] : Json());
        if (timers.valid()) out.set("powerTimers", timers);
        if (time.isObject() && !returnedFalse(time)) powerTimersSeen_ = timers.valid() ? timers : Json::object();
    }

    if (hasLogoLight_ == -1) {
        Json keys = Json::object();
        Json list = Json::array();
        list.add(Json::text("tv.model.logoLight"));
        keys.set("keys", list);
        Json reply;
        if (deps_.luna("com.webos.service.tv.systemproperty/getSystemProperties", keys, reply))
            hasLogoLight_ = reply["tv.model.logoLight"].isBoolean() && reply["tv.model.logoLight"].boolean() ? 1 : 0;
    }
    {
        Json options;
        (void)cached(kSettings, settingsRequest("option", {"standByLight", "logoLight", "powerOnLight", "quickStartMode"}),
                     60000, options);
        const Json settings = options["settings"].isObject() ? options["settings"] : Json::object();
        Json lights = Json::object();
        lights.set("standby", Json::flag(settings["standByLight"].isString() && settings["standByLight"].string() == "on"));
        lights.set("logo", Json::flag(settings["logoLight"].isString() && settings["logoLight"].string() == "on"));
        lights.set("powerOn", Json::flag(settings["powerOnLight"].isString() && settings["powerOnLight"].string() == "on"));
        lights.set("hasLogo", Json::flag(hasLogoLight_ == 1));
        out.set("lights", lights);
        if (settings.has("quickStartMode"))
            out.set("quickBoot", Json::flag(settings["quickStartMode"].isString() &&
                                            settings["quickStartMode"].string() == "on"));
    }
    {
        Json network;
        (void)cached(kSettings, settingsRequest("network", {"wolwowlOnOff"}), 60000, network);
        const Json wol = network["settings"]["wolwowlOnOff"];
        if (wol.valid())
            out.set("wakeOnLan", Json::flag((wol.isBoolean() && wol.boolean()) ||
                                            (wol.isString() && wol.string() == "true")));
    }
    {
        Json other;
        (void)cached(kSettings, settingsRequest("other", {"lgLogoDisplay"}), 60000, other);
        const Json logo = other["settings"]["lgLogoDisplay"];
        if (logo.valid()) out.set("lgLogo", Json::flag(jsOn(logo)));
    }
    {
        Json uei;
        (void)cached(kSettings, settingsRequest("other", {"ueiEnable"}), 60000, uei);
        const Json value = good(uei) ? uei["settings"]["ueiEnable"] : Json();
        if (value.valid() && !(value.isBoolean() && !value.boolean()))
            out.set("deviceDetection", Json::flag(jsOn(value)));
    }
    {
        Json general;
        (void)cached(kSettings,
                     settingsRequest("general", {"alwaysOn", "alwaysOnDisableStartHour", "alwaysOnDisableStartMinute",
                                                 "alwaysOnDisableEndHour", "alwaysOnDisableEndMinute"}),
                     60000, general);
        const Json settings = general["settings"].isObject() ? general["settings"] : Json::object();
        const Json always = alwaysOnSupport_ == 1 ? settings["alwaysOn"] : Json();
        if (always.valid()) out.set("alwaysReady", Json::flag(jsOn(always)));
        if (always.valid() && settings["alwaysOnDisableStartHour"].valid() &&
            settings["alwaysOnDisableEndHour"].valid()) {
            Json off = Json::object();
            off.set("start", Json::text(service::statsClockTime(settings["alwaysOnDisableStartHour"],
                                                                settings["alwaysOnDisableStartMinute"])));
            off.set("end", Json::text(service::statsClockTime(settings["alwaysOnDisableEndHour"],
                                                              settings["alwaysOnDisableEndMinute"])));
            out.set("alwaysReadyOff", off);
        }
    }
    {
        Json life;
        (void)cached(kSettings, settingsRequest("general", {"lifeOnScreenMode"}), 60000, life);
        const Json mode = good(life) ? life["settings"]["lifeOnScreenMode"] : Json();
        if (service::javascriptTruthy(mode)) {
            alwaysReadyScreenOn_ = service::javascriptString(mode) != "off";
            out.set("alwaysReadyScreen", Json::flag(alwaysReadyScreenOn_));
        }
    }
    {
        Json connection;
        (void)cached("com.palm.connectionmanager/getStatus", Json::object(), 60000, connection);
        const Json ssid = connection["wifi"]["ssid"];
        out.set("ssid", service::javascriptTruthy(ssid) ? ssid : Json());
    }
    {
        Json dimming;
        (void)cached("com.webos.service.tv.display/getDimmingStatus", Json::object(), 15000, dimming);
        out.set("dimming", service::javascriptTruthy(dimming["status"]) ? dimming["status"] : Json());
    }
    probeLightSensor(out);

    // Sound.
    Json sound;
    (void)cached("com.webos.audio/getSoundOut", Json::object(), liveTtl("audio", 10000, kLiveReadTtlMs), sound);
    if (sound.isObject()) {
        // A reply without a level leaves the key out, as it does in JS.
        if (sound["volume"].valid()) out.set("volume", sound["volume"]);
        out.set("muted", Json::flag(service::javascriptTruthy(sound["muted"])));
    }
    Json vs;
    if (masterVolumeSupported_ != 0) {
        Json reply;
        (void)cached("com.webos.service.audio/master/getVolume", Json::object(),
                     liveTtl("audio", 10000, kLiveReadTtlMs), reply);
        const std::string errorText = lowerText(reply["errorText"].string());
        if (errorText.find("unknown method") != std::string::npos ||
            errorText.find("unknown service") != std::string::npos)
            masterVolumeSupported_ = 0;
        if (good(reply) && reply["volumeStatus"].isObject()) {
            vs = reply["volumeStatus"];
            masterVolumeSupported_ = 1;
        }
    }
    // With an eARC soundbar controlling the volume, getSoundOut reports 0 and the
    // newer service has the soundbar's level.
    if (vs.isObject()) {
        if (vs["volume"].isNumber()) out.set("volume", vs["volume"]);
        if (vs["muteStatus"].isBoolean()) out.set("muted", vs["muteStatus"]);
    }
    if (sound.isObject() || vs.isObject())
        out.set("audio_output", Json::text(service::statsAudioOutput(vs, sound)));
    {
        const std::string route = service::statsSoundRoute(vs, sound);
        lastSoundRoute_ = route;
        if (haveVolumeRefusedRoute_ && volumeRefusedRoute_ != route) haveVolumeRefusedRoute_ = false;
        const std::string control = haveVolumeRefusedRoute_ ? "none" : service::statsVolumeControl(vs, sound);
        out.set("volume_control", Json::text(control));
        if (control != "level") out.set("volume", Json());
    }
    {
        Json soundSettings;
        (void)cached(kSettings, settingsRequest("sound", {"soundOutput", "soundMode"}), 15000, soundSettings);
        std::string rawSound = "tv_speaker";
        if (service::javascriptTruthy(soundSettings["settings"]["soundOutput"]))
            rawSound = service::javascriptString(soundSettings["settings"]["soundOutput"]);
        else if (sound.isObject() && service::javascriptTruthy(sound["scenario"]))
            rawSound = service::javascriptString(sound["scenario"]);
        Json soundOut = Json::object();
        soundOut.set("output", Json::text(names::soundOutput(rawSound)["display"].string()));
        soundOut.set("output_raw", Json::text(rawSound));
        soundOut.set("bt_audio", btAudioKnown_ ? Json::flag(btAudio_) : Json());
        soundOut.set("mode", service::javascriptTruthy(soundSettings["settings"]["soundMode"])
            ? soundSettings["settings"]["soundMode"] : Json::text("standard"));
        // The format of what is playing: the last answer goes out and the next is
        // asked for behind it.
        if (lastSoundInput_.valid()) soundOut.set("input", lastSoundInput_);
        {
            Json property;
            (void)cached("com.webos.service.audio/tv/getAudioInputProperty", Json::object(), 3000, property);
            if (property.isObject() && returnedFalse(property) &&
                lowerText(property["errorText"].string()).find("unknown method") != std::string::npos)
                soundFormatSupported_ = 0;
            if (good(property)) {
                soundFormatSupported_ = 1;
                const Json prop = property["audioInputProperty"].isArray() ? property["audioInputProperty"][0] : Json();
                if (!prop.isObject() || !service::javascriptTruthy(prop["codec"])) {
                    lastSoundInput_ = Json();
                } else {
                    Json input = Json::object();
                    input.set("codec", names::audioCodec(prop["codec"])["display"]);
                    input.set("codec_raw", Json::text(service::javascriptString(prop["codec"])));
                    const Json immersive = names::audioImmersive(prop["immersive"]);
                    if (immersive.isObject()) {
                        input.set("immersive", immersive["display"]);
                        input.set("immersive_raw", Json::text(service::javascriptString(prop["immersive"])));
                    }
                    lastSoundInput_ = input;
                }
            }
        }
        soundOutputs(rawSound, soundOut);
        out.set("sound", soundOut);
    }

    // The player's state.
    {
        Json player;
        if (media(player)) {
            if (player["state"].string() != "idle") {
                out.set("media", player);
                if (!(deps_.liveMedia && deps_.liveMedia().isObject()) && deps_.noteMediaSeen) deps_.noteMediaSeen();
            } else if (deps_.hasMediaState && deps_.hasMediaState()) {
                out.set("media", player);
            }
        }
    }

    // What is in front.
    Json app;
    (void)cached("com.webos.applicationManager/getForegroundAppInfo", Json::object(),
                 liveTtl("application", 4000, kLiveReadTtlMs), app);
    const bool screenOff = screenSaver || (powerState.isObject() &&
        ((powerState["screenOn"].isBoolean() && !powerState["screenOn"].boolean()) ||
         lowerText(powerState["raw"].string()) == "off"));
    bool inputShown = false;
    if (app.isObject() && service::javascriptTruthy(app["appId"])) {
        const std::string appId = service::javascriptString(app["appId"]);
        const std::string shortApp = names::shortAppId(appId, inputAliases_);
        out.set("app", Json::text(shortApp));
        out.set("app_id", Json::text(appId));
        const Json mapped = inputNames_[shortApp];
        const bool isInput = service::javascriptTruthy(mapped) && service::javascriptString(mapped) != shortApp;
        std::string title = service::javascriptTruthy(mapped) ? service::javascriptString(mapped)
            : service::javascriptTruthy(appTitles_[appId]) ? service::javascriptString(appTitles_[appId]) : shortApp;
        out.set("app_name", Json::text(title));
        out.set("display_title", Json::text(isInput ? names::inputTitle(shortApp, mapped) : title));
        const Json port = names::hdmiPort(shortApp);
        if (port.isNumber() && !screenOff) {
            inputShown = true;
            const int number = static_cast<int>(port.number());
            const Json receiverValue = receiverMap[std::to_string(number)];
            Json signal, timing, diag;
            if (receiverValue.isNumber()) {
                const int receiver = static_cast<int>(receiverValue.number());
                const Json fact = factFor(receiver);
                const Json t = fact.isObject() ? fact["timing"] : Json();
                if (t.isObject() && t["connected"].boolean()) {
                    Json shape = Json::object();
                    shape.set("width", t["width"]);
                    shape.set("height", t["height"]);
                    shape.set("refresh_hz", t["refreshHz"]);
                    if (t["width"].isNumber()) {
                        std::string text = compactNumber(t["width"].number()) + "x" + compactNumber(t["height"].number());
                        if (t["refreshHz"].isNumber()) text += " @ " + compactNumber(t["refreshHz"].number()) + "Hz";
                        signal = Json::text(text);
                        timing = shape;
                    } else {
                        signal = Json::text("Connected");
                    }
                    if (fact["diagnosticConnected"].boolean()) {
                        const Json link = fact["link"].isObject() ? fact["link"] : Json::object();
                        diag = Json::object();
                        diag.set("port", Json::numeric(receiver));
                        diag.set("phy_mode", service::javascriptTruthy(link["phy_mode"])
                            ? names::hdmiPhyMode(link["phy_mode"].string(), link["tmds_clock_khz"])["display"] : Json());
                        diag.set("chroma", service::javascriptTruthy(link["chroma"])
                            ? names::hdmiChroma(link["chroma"].string())["display"] : Json());
                        diag.set("hdcp", service::javascriptTruthy(link["hdcp"])
                            ? names::hdmiHdcp(link["hdcp"].string())["display"] : Json());
                        diag.set("allm", fact["allm"]);
                        diag.set("vrr", fact["vrr"]);
                        diag.set("qms", link["qms"]);
                    }
                }
            }
            out.set("signal", signal);
            out.set("signal_timing", timing);
            out.set("hdmi_diag", diag);
            if (diag.isObject() && deps_.noteHdmiSeen) deps_.noteHdmiSeen(diag);
        }
    }
    if (screenOff) {
        out.set("signal", Json());
        out.set("signal_timing", Json());
        out.set("hdmi_diag", Json());
    }
    if (deps_.sourceFrameRate && app.isObject() && app["appId"].isString()) {
        const Json rate = deps_.sourceFrameRate(inputShown ? app["appId"].string() : std::string());
        out.set("source_frame_rate", rate.valid() ? rate : Json());
    }
    {
        Json video;
        (void)cached("com.webos.service.videooutput/getStatus", Json::object(), 4000, video);
        out.set("signal_format", service::statsSignalFormat(video));
    }

    // The picture.
    Json picture;
    (void)cached(kSettings,
                 settingsRequest("picture", {"backlight", "pictureMode", "energySaving", "screenShift", "logoLuminanceAdjust"}),
                 10000, picture);
    const std::string foregroundId = app.isObject() ? app["appId"].string() : std::string();
    const bool isGallery = (picture.isObject() && picture["dimension"]["input"].isString() &&
                            picture["dimension"]["input"].string() == "gallery") ||
        foregroundId.find("com.webos.app.gallery") == 0 || foregroundId.find("com.webos.app.igallery") == 0 ||
        foregroundId.find("com.webos.app.galleryplus") == 0;
    if (picture.isObject() && picture["settings"].isObject()) {
        const Json settings = picture["settings"];
        const Json rawRange = picture["dimension"]["dynamicRange"];
        const std::string rawMode = isGallery ? "gallery"
            : service::javascriptTruthy(settings["pictureMode"]) ? service::javascriptString(settings["pictureMode"]) : "standard";
        Json shape = Json::object();
        shape.set("dynamicRange", names::dynamicRange(service::javascriptTruthy(rawRange)
            ? service::javascriptString(rawRange) : std::string("sdr"))["display"]);
        shape.set("dynamicRange_raw", service::javascriptTruthy(rawRange) ? rawRange : Json());
        shape.set("mode", names::pictureMode(rawMode)["display"]);
        shape.set("mode_raw", Json::text(rawMode));
        shape.set("backlight", Json::numeric(toInt(settings["backlight"], 50)));
        shape.set("backlight_raw", settings["backlight"].valid() &&
                                           (settings["backlight"].isNumber() || settings["backlight"].isString())
            ? Json::numeric(toInt(settings["backlight"], 0)) : Json());
        shape.set("energySaving", service::javascriptTruthy(settings["energySaving"]) ? settings["energySaving"] : Json::text("off"));
        shape.set("screenShift", service::javascriptTruthy(settings["screenShift"]) ? settings["screenShift"] : Json::text("off"));
        shape.set("logoLuminanceAdjust", service::javascriptTruthy(settings["logoLuminanceAdjust"])
            ? settings["logoLuminanceAdjust"] : Json::text("off"));
        shape.set("modes", Json::array());
        out.set("picture", shape);
    }
    {
        const Json modes = isGallery ? Json::array() : pictureModes();
        if (out.has("picture")) {
            Json shape = out["picture"];
            shape.set("modes", modes);
            out.set("picture", shape);
        }
        out.set("apps", refreshInstalledApps());
        Json privacy = Json::object();
        privacy.set("adblock", deps_.adBlock ? deps_.adBlock() : Json::parse("{\"enabled\":false,\"count\":0}"));
        out.set("privacy", privacy);
        const bool thermal = system["thermalPresent"].boolean();
        const bool emmcWear = system["emmcWearPresent"].boolean();
        Json capabilities = Json::object();
        const bool oled = deps_.isOled && deps_.oledStats && deps_.isOled();
        capabilities.set("oled", Json::flag(oled));
        capabilities.set("thermal", Json::flag(thermal));
        capabilities.set("emmcWear", Json::flag(emmcWear));
        out.set("capabilities", capabilities);
        out.set("oled", oled ? deps_.oledStats(picture.isObject() ? picture["settings"] : Json(), powerState) : Json());
    }

    collected_ = true;
    bool invalidated;
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        invalidated = pending_.expire;
    }
    {
        Json inputs = Json::object();
        Json device = Json::object();
        for (const char *key : {"name", "model", "sw_version"})
            if (service::javascriptTruthy(device_[key])) device.set(key, device_[key]);
        inputs.set("device", device);
        inputs.set("pictureModes", lastPictureModes_);
        Json ids = Json::array();
        for (const Json &output : lastSoundOutputs_.items()) ids.add(output["id"]);
        inputs.set("soundOutputs", ids);
        std::lock_guard<std::mutex> lock(deviceMutex_);
        discoveryShared_ = Json::parse(inputs.dump());
    }
    // Something changed while this was read: it answers this request, not the next.
    if (!invalidated) {
        lastStats_ = out;
        lastStatsAt_ = deps_.nowMs();
    }
    return out;
}

} // namespace daemon
} // namespace nativetv
