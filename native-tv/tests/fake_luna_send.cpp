#include "core/json.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <sys/file.h>
#include <thread>
#include <utility>
#include <unistd.h>
#include <vector>

using namespace nativetv;

namespace {
std::string environment(const char *key) {
    const char *value = std::getenv(key);
    return value ? value : "";
}

bool startsWith(const std::string &value, const std::string &prefix) {
    return value.compare(0, prefix.size(), prefix) == 0;
}

std::string utcStamp(std::time_t value) {
    std::tm parts = {};
    if (!gmtime_r(&value, &parts)) return "";
    char rendered[32] = {};
    if (std::strftime(rendered, sizeof(rendered), "%Y,%m,%d,%H,%M,%S", &parts) == 0)
        return "";
    return rendered;
}

Json readJsonFile(const std::string &path) {
    if (path.empty()) return Json();
    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
    if (!input) return Json();
    std::string contents((std::istreambuf_iterator<char>(input)),
                         std::istreambuf_iterator<char>());
    return Json::parse(contents);
}

void writeJsonFile(const std::string &path, const Json &value) {
    if (path.empty()) return;
    std::ofstream output(path.c_str(), std::ios::out | std::ios::trunc | std::ios::binary);
    if (output) output << value.dump();
}

class JsonFileLock {
  public:
    explicit JsonFileLock(const std::string &path) {
        if (path.empty()) return;
        const std::string lockPath = path + ".lock";
        descriptor_ = open(lockPath.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        if (descriptor_ >= 0 && flock(descriptor_, LOCK_EX) != 0) {
            close(descriptor_);
            descriptor_ = -1;
        }
    }
    ~JsonFileLock() {
        if (descriptor_ >= 0) {
            (void)flock(descriptor_, LOCK_UN);
            close(descriptor_);
        }
    }
    JsonFileLock(const JsonFileLock &) = delete;
    JsonFileLock &operator=(const JsonFileLock &) = delete;

  private:
    int descriptor_ = -1;
};
} // namespace

int main(int argc, char **argv) {
    std::string uri;
    std::string applicationId;
    Json callPayload = Json::object();
    bool subscription = false;
    for (int i = 1; i < argc; ++i) {
        std::string argument = argv[i];
        if (argument == "-a" && i + 1 < argc) {
            applicationId = argv[++i];
            continue;
        }
        if (argument == "-i")
            subscription = true;
        if (startsWith(argument, "luna://"))
            uri = argument.substr(7);
        else if (!argument.empty() && (argument[0] == '{' || argument[0] == '['))
            callPayload = Json::parse(argument);
    }
    if (environment("NATIVETV_FAKE_LUNA_MODE") == "slow")
        std::this_thread::sleep_for(std::chrono::milliseconds(160));

    Json reply = Json::object();
    const std::string mode = environment("NATIVETV_FAKE_LUNA_MODE");
    // home-breaker: the Home takeover's SAM and the live foreground app together,
    // with the apps the app manager lists read from NATIVETV_FAKE_LUNA_RUNNING_FILE.
    const bool homeTakeoverMode = mode == "home-takeover" || mode == "home-breaker";
    const bool stateGroupsMode = mode == "state-groups" || mode == "home-breaker";
    const std::string logPath = environment("NATIVETV_FAKE_LUNA_LOG");
    const std::string detailPath = environment("NATIVETV_FAKE_LUNA_DETAILS");
    size_t loggedCalls = 0;
    size_t previousPictureReads = 0;
    bool previousSoundOutputWrite = false;
    bool previousPiccapRunning = false;
    size_t previousHomeAdds = 0;
    std::vector<std::pair<std::string, std::string>> homePoints = {
        {"com.webos.app.hdmi2", "200"}};
    if (!logPath.empty()) {
        std::ofstream log(logPath.c_str(), std::ios::app);
        log << (subscription ? "subscribe " : "call ") << uri << '\n';
        log.close();
        std::ifstream previous(logPath.c_str());
        std::string line;
        while (std::getline(previous, line))
            ++loggedCalls;
    }
    if (!detailPath.empty()) {
        std::ifstream previous(detailPath.c_str());
        std::string line;
        while (std::getline(previous, line)) {
            const Json record = Json::parse(line);
            if (uri == "com.webos.service.settings/getSystemSettings" &&
                callPayload["category"].string() == "picture" &&
                record["uri"].string() ==
                    "com.webos.service.settings/getSystemSettings" &&
                record["payload"]["category"].string() == "picture")
                ++previousPictureReads;
            if (record["uri"].string() ==
                    "com.webos.service.settings/setSystemSettings" &&
                record["payload"]["category"].string() == "sound" &&
                record["payload"]["settings"]["soundOutput"].isString())
                previousSoundOutputWrite = true;
            if (record["uri"].string() == "org.webosbrew.piccap.service/start")
                previousPiccapRunning = true;
            else if (record["uri"].string() == "org.webosbrew.piccap.service/stop")
                previousPiccapRunning = false;
            if (mode == "home-screen") {
                const std::string previousUri = record["uri"].string();
                if (previousUri == "com.webos.applicationManager/addLaunchPoint") {
                    homePoints.emplace_back(record["payload"]["id"].string(),
                        std::to_string(1001 + previousHomeAdds));
                    ++previousHomeAdds;
                } else if (previousUri == "com.webos.applicationManager/removeLaunchPoint") {
                    const std::string launchPointId =
                        record["payload"]["launchPointId"].string();
                    homePoints.erase(std::remove_if(homePoints.begin(), homePoints.end(),
                        [&launchPointId](const std::pair<std::string, std::string> &point) {
                            return point.second == launchPointId;
                        }), homePoints.end());
                } else if (previousUri == "com.webos.applicationManager/moveLaunchPoint") {
                    const std::string launchPointId =
                        record["payload"]["launchPointId"].string();
                    auto point = std::find_if(homePoints.begin(), homePoints.end(),
                        [&launchPointId](const std::pair<std::string, std::string> &item) {
                            return item.second == launchPointId;
                        });
                    if (point != homePoints.end()) {
                        const std::pair<std::string, std::string> moved = *point;
                        homePoints.erase(point);
                        const size_t position = static_cast<size_t>(
                            std::max(0.0, record["payload"]["position"].number()));
                        homePoints.insert(homePoints.begin() +
                            std::min(position, homePoints.size()), moved);
                    }
                }
            }
        }
        Json record = Json::object();
        record.set("uri", Json::text(uri));
        record.set("applicationId", Json::text(applicationId));
        record.set("payload", callPayload);
        if (mode == "remote-keys-native" &&
            uri == "com.webos.settingsservice/setSystemSettings" &&
            environment("NATIVETV_FAKE_REMOTE_KEYS_RESTORE_FAIL") == "1" &&
            callPayload["settings"].has("mapping_info"))
            record.set("rejected", Json::flag(true));
        std::ofstream detail(detailPath.c_str(), std::ios::app);
        detail << record.dump() << '\n';
    }
    const std::string hdmiMap = environment("NATIVETV_FAKE_HDMI_MAP");
    const bool asksInputMap = uri == "com.webos.service.config/getConfigs" &&
        callPayload["configNames"][0].string().compare(0, 9, "inputMap.") == 0;
    if (!hdmiMap.empty() && asksInputMap) {
        // configd's input map: no answer at all (a timed-out luna-send), a refusal, or a C4's.
        if (hdmiMap == "silent") return 0;
        if (hdmiMap == "refused") {
            reply.set("returnValue", Json::flag(false));
            reply.set("errorText", Json::text("Unknown method"));
        } else {
            reply = Json::parse(
                "{\"returnValue\":true,\"configs\":{\"inputMap.videoInputMapIndexInfo0\":["
                "{\"assignment\":{\"hdmi1\":\"3\",\"hdmi2\":\"2\",\"hdmi3\":\"1\",\"hdmi4\":\"0\"}}]}}");
        }
    } else if (mode == "screen-time" &&
        (uri == "com.webos.notification/createToast" ||
         uri == "com.webos.applicationManager/launch" ||
         uri == "com.webos.service.tvpower/power/powerOff" ||
         uri == "com.webos.service.tvpower/power/turnOffScreen")) {
        reply.set("returnValue", Json::flag(true));
    } else if (mode == "home-starting-notice" &&
               uri == "com.webos.notification/createToast") {
        reply.set("returnValue", Json::flag(
            environment("NATIVETV_FAKE_HOME_NOTICE_FAIL") != "1"));
    } else if (mode == "home-startup-route" &&
               uri == "com.webos.service.config/getConfigs") {
        Json configs = Json::object();
        Json chain = readJsonFile(
            environment("NATIVETV_FAKE_HOME_STARTUP_CHAIN"));
        configs.set("com.webos.surfacemanager.keyFilters",
                    chain.isArray() ? chain : Json::array());
        reply.set("returnValue", Json::flag(true));
        reply.set("configs", configs);
    } else if (mode == "home-startup-route" &&
               uri == "com.webos.service.config/setConfigs") {
        const Json chain = callPayload["configs"][
            "com.webos.surfacemanager.keyFilters"];
        if (chain.isArray())
            writeJsonFile(environment("NATIVETV_FAKE_HOME_STARTUP_CHAIN"),
                          chain);
        reply.set("returnValue", Json::flag(true));
    } else if (mode == "home-startup-route" &&
               uri == "com.webos.service.settings/setSystemSettings") {
        reply.set("returnValue", Json::flag(
            access(environment("NATIVETV_FAKE_HOME_STARTUP_SETTINGS_FAIL").c_str(),
                   F_OK) != 0));
    } else if (mode == "home-boot" &&
        uri == "com.webos.service.tvpower/power/getPowerOnReason") {
        const std::string reason = environment("NATIVETV_FAKE_HOME_BOOT_REASON");
        reply.set("returnValue", Json::flag(true));
        reply.set("reason", Json::text(reason.empty() ? "remoteKey" : reason));
    } else if (mode == "home-boot" &&
               uri == "com.webos.service.config/getConfigs") {
        Json configs = Json::object();
        Json reasons = Json::parse(
            environment("NATIVETV_FAKE_HOME_BOOT_ALLOWED_REASONS"));
        if (!reasons.isArray()) {
            reasons = Json::array();
            reasons.add(Json::text("aiKey"));
            reasons.add(Json::text("lastPowerOn"));
            reasons.add(Json::text("localKey"));
            reasons.add(Json::text("remoteKey"));
            reasons.add(Json::text("seniorHelp"));
        }
        configs.set("com.webos.service.tvpower.fullHomeBootingPowerOnReasons", reasons);
        reply.set("returnValue", Json::flag(true));
        reply.set("configs", configs);
    } else if (mode == "home-boot" &&
               uri == "com.webos.applicationManager/getForegroundAppInfo") {
        const std::string foreground =
            environment("NATIVETV_FAKE_HOME_BOOT_FOREGROUND");
        reply.set("returnValue", Json::flag(true));
        reply.set("appId", Json::text(foreground.empty()
            ? "com.tvweb.splash" : foreground));
    } else if (mode == "home-boot" &&
               uri == "com.webos.applicationManager/launch") {
        reply.set("returnValue", Json::flag(true));
    } else if ((mode == "home-recents" || mode == "home-recents-unavailable") &&
        uri == "com.webos.service.recent/listRecents") {
        const std::string statePath = environment("NATIVETV_FAKE_HOME_RECENTS_STATE");
        const Json state = readJsonFile(statePath);
        if (mode == "home-recents-unavailable" || state["unavailable"].boolean()) {
            reply.set("returnValue", Json::flag(false));
        } else {
            const Json recents = statePath.empty()
                ? Json::parse(environment("NATIVETV_FAKE_HOME_RECENTS"))
                : state["cards"];
            reply.set("returnValue", Json::flag(true));
            reply.set("recentsAppList", recents.isArray() ? recents : Json::array());
        }
    } else if (mode == "home-recents" &&
               uri == "com.webos.applicationManager/running") {
        const Json state = readJsonFile(environment("NATIVETV_FAKE_HOME_RECENTS_STATE"));
        if (state["runningUnavailable"].boolean()) {
            reply.set("returnValue", Json::flag(false));
        } else {
            reply.set("returnValue", Json::flag(true));
            reply.set("running", state["running"].isArray()
                                      ? state["running"] : Json::array());
        }
    } else if (mode == "home-recents" &&
               (uri == "com.webos.applicationManager/closeByAppId" ||
                uri == "com.palm.webappmanager/killApp")) {
        const std::string statePath = environment("NATIVETV_FAKE_HOME_RECENTS_STATE");
        JsonFileLock stateLock(statePath);
        Json state = readJsonFile(statePath);
        const std::string id = callPayload["id"].isString()
            ? callPayload["id"].string() : callPayload["appId"].string();
        const bool refused = state["refused"].boolean();
        reply.set("returnValue", Json::flag(!refused));
        if (!refused && !state["ackOnly"].boolean()) {
            Json running = Json::array();
            for (const Json &app : state["running"].items())
                if (app["id"].string() != id) running.add(app);
            state.set("running", running);
            if (state["newCard"].boolean()) {
                bool present = false;
                for (const Json &app : state["cards"].items())
                    if (app["appId"].string() == "new.app") present = true;
                if (!present) {
                    Json card = Json::object();
                    card.set("appId", Json::text("new.app"));
                    card.set("title", Json::text("new.app"));
                    Json cards = state["cards"].isArray() ? state["cards"] : Json::array();
                    cards.add(card);
                    state.set("cards", cards);
                    Json app = Json::object();
                    app.set("id", Json::text("new.app"));
                    app.set("appType", Json::text("native"));
                    Json runningWithNew = state["running"].isArray()
                        ? state["running"] : Json::array();
                    runningWithNew.add(app);
                    state.set("running", runningWithNew);
                }
            }
        }
        writeJsonFile(statePath, state);
    } else if (mode == "home-recents" &&
               uri == "com.webos.applicationManager/launch") {
        reply.set("returnValue", Json::flag(true));
    } else if (mode == "home-recents" &&
               uri == "com.webos.service.recent/removeRecent") {
        const std::string statePath = environment("NATIVETV_FAKE_HOME_RECENTS_STATE");
        JsonFileLock stateLock(statePath);
        Json state = readJsonFile(statePath);
        const std::string id = callPayload["appId"].string();
        Json cards = Json::array();
        for (const Json &app : state["cards"].items())
            if (app["appId"].string() != id) cards.add(app);
        state.set("cards", cards);
        writeJsonFile(statePath, state);
        reply.set("returnValue", Json::flag(true));
    } else if (mode == "home-restore" &&
               uri == "com.webos.service.settings/getSystemSettings") {
        Json settings = Json::parse(
            environment("NATIVETV_FAKE_HOME_RESTORE_SETTINGS"));
        reply.set("returnValue", Json::flag(true));
        reply.set("settings", settings.isObject() ? settings : Json::object());
    } else if (mode == "home-restore" &&
               uri == "com.webos.service.settings/setSystemSettings") {
        reply.set("returnValue", Json::flag(true));
    } else if (mode == "home-restore" &&
               uri == "com.webos.applicationManager/getAppInfo" &&
               callPayload["id"].string() == "com.webos.app.home") {
        Json appInfo = Json::object();
        appInfo.set("id", Json::text("com.webos.app.home"));
        appInfo.set("type", Json::text("native"));
        reply.set("returnValue", Json::flag(true));
        reply.set("appInfo", appInfo);
    } else if (mode == "home-restore" &&
               (uri == "com.webos.service.config/setConfigs" ||
                uri == "com.palm.webappmanager/killApp")) {
        reply.set("returnValue", Json::flag(true));
    } else if (homeTakeoverMode &&
               uri == "com.webos.service.settings/getSystemSettings") {
        reply.set("returnValue", Json::flag(true));
        reply.set("settings", Json::object());
    } else if (homeTakeoverMode &&
               uri == "com.webos.applicationManager/getAppInfo" &&
               callPayload["id"].string() == "com.webos.app.home") {
        const std::string typePath = environment("NATIVETV_FAKE_HOME_TAKEOVER_TYPE");
        std::ifstream typeInput(typePath.c_str());
        std::string type;
        std::getline(typeInput, type);
        Json appInfo = Json::object();
        appInfo.set("id", Json::text("com.webos.app.home"));
        appInfo.set("type", Json::text(type.empty() ? "native" : type));
        if (type == "web") {
            Json category = Json::array();
            category.add(Json::text("home"));
            appInfo.set("appCategory", category);
        }
        reply.set("returnValue", Json::flag(true));
        reply.set("appInfo", appInfo);
    } else if (homeTakeoverMode &&
               uri == "com.webos.service.config/getConfigs") {
        Json configs = Json::object();
        configs.set("profile.blockedAppList", Json::array());
        reply.set("returnValue", Json::flag(true));
        reply.set("configs", configs);
    } else if (homeTakeoverMode &&
               (uri == "com.webos.service.config/setConfigs" ||
                uri == "com.webos.applicationManager/blockSystemApp" ||
                uri == "com.webos.applicationManager/releaseBlockSystemApp")) {
        const std::string typePath = environment("NATIVETV_FAKE_HOME_TAKEOVER_TYPE");
        const std::string mountedPath =
            environment("NATIVETV_FAKE_HOME_TAKEOVER_MOUNTED");
        const std::string refusePath =
            environment("NATIVETV_FAKE_HOME_TAKEOVER_REFUSE");
        if (refusePath.empty() || ::access(refusePath.c_str(), F_OK) != 0) {
            std::ofstream typeOutput(typePath.c_str(), std::ios::out | std::ios::trunc);
            typeOutput << (::access(mountedPath.c_str(), F_OK) == 0
                ? "web\n" : "native\n");
        }
        reply.set("returnValue", Json::flag(true));
    } else if (homeTakeoverMode &&
               uri == "com.palm.webappmanager/killApp") {
        reply.set("returnValue", Json::flag(true));
    } else if (mode == "screensaver-boot" &&
               uri == "com.webos.applicationManager/getAppInfo" &&
               callPayload["id"].string() == "com.webos.app.screensaver") {
        Json appInfo = Json::object();
        appInfo.set("id", Json::text("com.webos.app.screensaver"));
        const std::string type = environment("NATIVETV_FAKE_SCREENSAVER_SAM_TYPE");
        appInfo.set("type", Json::text(type.empty() ? "qml" : type));
        reply.set("returnValue", Json::flag(true));
        reply.set("appInfo", appInfo);
    } else if (mode == "screensaver-boot" &&
               uri == "com.webos.service.config/getConfigs") {
        Json configs = Json::object();
        configs.set("profile.blockedAppList", Json::array());
        reply.set("returnValue", Json::flag(true));
        reply.set("configs", configs);
    } else if (mode == "screensaver-boot" &&
               (uri == "com.webos.service.config/setConfigs" ||
                uri == "com.webos.applicationManager/blockSystemApp" ||
                uri == "com.webos.applicationManager/releaseBlockSystemApp")) {
        reply.set("returnValue", Json::flag(true));
    } else if (mode == "sam-rescan" && uri == "com.webos.service.config/getConfigs") {
        Json blocked = Json::array();
        blocked.add(Json::text("com.webos.app.roomconnect"));
        blocked.add(Json::text("netflix"));
        Json configs = Json::object();
        configs.set("profile.blockedAppList", blocked);
        reply.set("returnValue", Json::flag(true));
        reply.set("configs", configs);
    } else if (mode == "home-now" &&
               uri == "com.webos.applicationManager/getForegroundAppInfo") {
        reply.set("returnValue", Json::flag(true));
        const std::string foreground = environment("NATIVETV_FAKE_HOME_NOW_APP");
        reply.set("appId", Json::text(foreground.empty()
            ? "com.webos.app.netflix" : foreground));
    } else if (mode == "home-now" &&
               uri == "com.webos.applicationManager/listLaunchPoints") {
        Json points = Json::array();
        Json netflix = Json::object();
        netflix.set("id", Json::text("com.webos.app.netflix"));
        netflix.set("title", Json::text("Netflix"));
        points.add(netflix);
        Json youtube = Json::object();
        youtube.set("id", Json::text("com.webos.app.youtube"));
        youtube.set("title", Json::text("YouTube"));
        points.add(youtube);
        reply.set("returnValue", Json::flag(true));
        reply.set("launchPoints", points);
    } else if (mode == "home-now" &&
               uri == "com.webos.applicationManager/getAppInfo" &&
               callPayload["id"].string() == "com.webos.app.home") {
        Json appInfo = Json::object();
        appInfo.set("id", Json::text("com.webos.app.home"));
        appInfo.set("type", Json::text("web"));
        reply.set("returnValue", Json::flag(true));
        reply.set("appInfo", appInfo);
    } else if (mode == "home-status" &&
               uri == "com.webos.applicationManager/getForegroundAppInfo") {
        reply.set("returnValue", Json::flag(true));
        reply.set("appId", Json::text(
            environment("NATIVETV_FAKE_HOME_STATUS_APP")));
    } else if (mode == "home-status" &&
               uri == "com.webos.applicationManager/getAppInfo" &&
               callPayload["id"].string() == "com.webos.app.home") {
        Json appInfo = Json::object();
        appInfo.set("id", Json::text("com.webos.app.home"));
        appInfo.set("type", Json::text("web"));
        reply.set("returnValue", Json::flag(true));
        reply.set("appInfo", appInfo);
    } else if (mode == "home-now" &&
               uri == "com.webos.service.eim/getAllInputStatus") {
        Json devices = Json::array();
        Json plugged = Json::object();
        plugged.set("id", Json::text("HDMI_2"));
        plugged.set("connected", Json::flag(true));
        Json subList = Json::array();
        Json receiver = Json::object();
        receiver.set("id", Json::text("SIMPLINK"));
        receiver.set("osdName", Json::text("AV Receiver"));
        receiver.set("depth", Json::numeric(1));
        receiver.set("physicalAddress", Json::numeric(4096));
        subList.add(receiver);
        Json console = Json::object();
        console.set("id", Json::text("SIMPLINK"));
        console.set("osdName", Json::text("Game Console"));
        console.set("depth", Json::numeric(2));
        console.set("physicalAddress", Json::numeric(8192));
        subList.add(console);
        plugged.set("subList", subList);
        devices.add(plugged);
        reply.set("returnValue", Json::flag(true));
        reply.set("devices", devices);
    } else if (mode == "home-now" &&
               uri == "com.webos.service.utp.boot/getLastChannelId") {
        reply.set("returnValue", Json::flag(true));
        reply.set("channelId", Json::text("channel-1"));
    } else if (mode == "home-now" &&
               uri == "com.webos.service.apiadapter/tv/getChannelList") {
        Json channels = Json::array();
        Json channel = Json::object();
        channel.set("channelId", Json::text("channel-1"));
        channel.set("channelNumber", Json::text("1"));
        channel.set("channelName", Json::text("BBC One HD"));
        channels.add(channel);
        reply.set("returnValue", Json::flag(true));
        reply.set("channelList", channels);
    } else if (mode == "home-now" &&
               uri == "com.webos.service.apiadapter/tv/getChannelCurrentProgramInfo") {
        const std::time_t now = std::time(nullptr);
        reply.set("returnValue", Json::flag(true));
        reply.set("programName", Json::text("Current News"));
        reply.set("startTime", Json::text(utcStamp(now - 3600)));
        reply.set("endTime", Json::text(utcStamp(now + 3600)));
    } else if ((mode == "home-catalogue" || mode == "home-open" ||
                mode == "home-open-refused") &&
               uri == "com.webos.applicationManager/listLaunchPoints") {
        Json points = Json::array();
        Json netflix = Json::object();
        netflix.set("id", Json::text("com.webos.app.netflix"));
        netflix.set("title", Json::text("Netflix"));
        netflix.set("largeIcon", Json::text("netflix.svg"));
        netflix.set("icon", Json::text("netflix.png"));
        netflix.set("iconColor", Json::text("#AABBCC"));
        netflix.set("appDescription", Json::text("Films and series"));
        points.add(netflix);
        Json youtube = Json::object();
        youtube.set("id", Json::text("com.webos.app.youtube"));
        youtube.set("title", Json::text("YouTube"));
        youtube.set("extraLargeIcon", Json::text("youtube.png"));
        points.add(youtube);
        if (mode == "home-open" || mode == "home-open-refused") {
            Json netflixNative = Json::object();
            netflixNative.set("id", Json::text("netflix"));
            netflixNative.set("title", Json::text("Netflix"));
            points.add(netflixNative);
            Json youtubeNative = Json::object();
            youtubeNative.set("id", Json::text("youtube.leanback.v4"));
            youtubeNative.set("title", Json::text("YouTube"));
            points.add(youtubeNative);
            Json titleApp = Json::object();
            titleApp.set("id", Json::text("com.animemania.app"));
            titleApp.set("title", Json::text("AnimeMania"));
            points.add(titleApp);
        }
        Json bookmark = Json::object();
        bookmark.set("id", Json::text("com.example.bookmark"));
        bookmark.set("lptype", Json::text("bookmark"));
        points.add(bookmark);
        Json hidden = Json::object();
        hidden.set("id", Json::text("com.example.hidden"));
        hidden.set("visible", Json::flag(false));
        points.add(hidden);
        Json home = Json::object();
        home.set("id", Json::text("com.tvweb.home"));
        points.add(home);
        Json input = Json::object();
        input.set("id", Json::text("com.webos.app.hdmi1"));
        points.add(input);
        reply.set("returnValue", Json::flag(true));
        reply.set("launchPoints", points);
    } else if ((mode == "home-catalogue" || mode == "home-open" ||
                mode == "home-open-refused") &&
               uri == "com.webos.applicationManager/getAppInfo") {
        const std::string id = callPayload["id"].string();
        Json app = Json::object();
        app.set("id", Json::text(id));
        if (id == "com.webos.app.notificationcenter") {
            app.set("title", Json::text("Notifications"));
            app.set("icon", Json::text("notification.png"));
            app.set("folderPath", Json::text("/apps/notifications"));
            reply.set("returnValue", Json::flag(true));
            reply.set("appInfo", app);
        } else {
            reply.set("returnValue", Json::flag(false));
        }
    } else if ((mode == "home-catalogue" || mode == "home-open" ||
                mode == "home-open-refused") &&
               uri == "com.webos.service.eim/getAllInputStatus") {
        Json devices = Json::array();
        Json unplugged = Json::object();
        unplugged.set("id", Json::text("HDMI_1"));
        unplugged.set("connected", Json::flag(false));
        devices.add(unplugged);
        Json plugged = Json::object();
        plugged.set("id", Json::text("HDMI_2"));
        plugged.set("connected", Json::flag(true));
        Json subList = Json::array();
        Json receiver = Json::object();
        receiver.set("id", Json::text("SIMPLINK"));
        receiver.set("osdName", Json::text("AV Receiver"));
        receiver.set("depth", Json::numeric(1));
        receiver.set("physicalAddress", Json::numeric(4096));
        subList.add(receiver);
        Json console = Json::object();
        console.set("id", Json::text("SIMPLINK"));
        console.set("osdName", Json::text("Game Console"));
        console.set("depth", Json::numeric(2));
        console.set("physicalAddress", Json::numeric(8192));
        subList.add(console);
        plugged.set("subList", subList);
        devices.add(plugged);
        reply.set("returnValue", Json::flag(true));
        reply.set("devices", devices);
    } else if ((mode == "home-open" || mode == "home-open-refused") &&
               uri == "com.webos.service.apiadapter/tv/getChannelList") {
        Json channels = Json::array();
        Json channel = Json::object();
        channel.set("channelId", Json::text("channel-1"));
        channel.set("channelNumber", Json::text("1"));
        channel.set("channelName", Json::text("BBC One HD"));
        channels.add(channel);
        reply.set("returnValue", Json::flag(true));
        reply.set("channelList", channels);
    } else if ((mode == "home-open" || mode == "home-open-refused") &&
               uri == "com.webos.applicationManager/launch") {
        reply.set("returnValue",
                  Json::flag(mode == "home-open"));
    } else if ((mode == "home-open" || mode == "home-open-refused") &&
               uri == "com.webos.service.apiadapter/tv/openChannel") {
        reply.set("returnValue",
                  Json::flag(mode == "home-open"));
    } else if (mode == "home-channels" &&
               uri == "com.webos.service.apiadapter/tv/getChannelList") {
        Json channels = Json::array();
        Json first = Json::object();
        first.set("channelId", Json::text("1_1_1_0_0_0_0"));
        first.set("channelNumber", Json::text("1"));
        first.set("channelName", Json::text("BBC One HD"));
        channels.add(first);
        Json radio = Json::object();
        radio.set("channelId", Json::text("radio-2"));
        radio.set("channelNumber", Json::text("2"));
        radio.set("channelName", Json::text("Radio 2"));
        radio.set("Radio", Json::flag(true));
        channels.add(radio);
        Json skipped = Json::object();
        skipped.set("channelId", Json::text("skipped"));
        skipped.set("skipped", Json::flag(true));
        channels.add(skipped);
        Json invisible = Json::object();
        invisible.set("channelId", Json::text("invisible"));
        invisible.set("Invisible", Json::flag(true));
        channels.add(invisible);
        reply.set("returnValue", Json::flag(true));
        reply.set("channelList", channels);
    } else if (mode == "home-guide" &&
               uri == "com.webos.service.apiadapter/tv/getChannelCurrentProgramInfo") {
        const std::string channelId = callPayload["channelId"].string();
        if (channelId == "guide-live") {
            const std::time_t now = std::time(nullptr);
            reply.set("returnValue", Json::flag(true));
            reply.set("programName", Json::text("Evening News"));
            reply.set("startTime", Json::text(utcStamp(now - 3600)));
            reply.set("endTime", Json::text(utcStamp(now + 3600)));
        } else if (channelId == "guide-expired") {
            const std::time_t now = std::time(nullptr);
            reply.set("returnValue", Json::flag(true));
            reply.set("programName", Json::text("Earlier News"));
            reply.set("startTime", Json::text(utcStamp(now - 7200)));
            reply.set("endTime", Json::text(utcStamp(now - 3600)));
        } else {
            reply.set("returnValue", Json::flag(false));
        }
    } else if (mode == "home-picture" &&
               uri == "com.webos.service.settings/getSystemSettings" &&
               callPayload["category"].string() == "picture") {
        Json settings = Json::object();
        const Json keys = callPayload["keys"];
        const std::string dynamicRange =
            callPayload["dimension"]["dynamicRange"].string();
        const std::string pictureMode =
            callPayload["dimension"]["pictureMode"].string();
        for (const Json &key : keys.items()) {
            if (key.string() == "pictureMode")
                settings.set("pictureMode", Json::text(
                    dynamicRange == "hdr" ? "hdrFilmMaker"
                    : dynamicRange == "dolbyHdr" ? "dolbyHdrCinemaBright"
                    : "standard"));
            else if (key.string() == "backlight")
                settings.set("backlight", Json::text(
                    pictureMode == "hdrFilmMaker" ? "100" : "47"));
        }
        reply.set("returnValue", Json::flag(true));
        reply.set("settings", settings);
    } else if (mode == "home-picture" &&
               uri == "com.webos.service.videooutput/getStatus") {
        const Json video = Json::parse(environment("NATIVETV_FAKE_HOME_VIDEO"));
        reply.set("returnValue", Json::flag(true));
        reply.set("video", video["video"].isArray() ? video["video"] : Json::array());
    } else if (mode == "home-screen" &&
               uri == "com.webos.applicationManager/getForegroundAppInfo") {
        reply.set("returnValue", Json::flag(true));
        const std::string foreground = environment("NATIVETV_FAKE_HOME_FOREGROUND");
        if (!foreground.empty()) reply.set("appId", Json::text(foreground));
    } else if (mode == "home-screen" &&
               uri == "com.webos.applicationManager/listLaunchPoints") {
        Json points = Json::array();
        for (const auto &entry : homePoints) {
            Json point = Json::object();
            point.set("id", Json::text(entry.first));
            point.set("launchPointId", Json::text(entry.second));
            points.add(point);
        }
        reply.set("returnValue", Json::flag(true));
        reply.set("launchPoints", points);
    } else if (mode == "saved-pages" &&
               uri == "com.webos.applicationManager/listLaunchPoints") {
        Json points = Json::parse(environment("NATIVETV_FAKE_SAVED_PAGE_POINTS"));
        reply.set("returnValue", Json::flag(true));
        reply.set("launchPoints", points.isArray() ? points : Json::array());
    } else if (mode == "saved-pages" &&
               (uri == "com.webos.applicationManager/addLaunchPoint" ||
                uri == "com.webos.applicationManager/updateLaunchPoint" ||
                uri == "com.webos.applicationManager/removeLaunchPoint")) {
        reply.set("returnValue", Json::flag(true));
        if (uri == "com.webos.applicationManager/addLaunchPoint") {
            const std::string launchPointId = environment("NATIVETV_FAKE_SAVED_PAGE_ID");
            reply.set("launchPointId", Json::text(
                launchPointId.empty() ? "saved-page-1" : launchPointId));
        }
    } else if (mode == "backup" &&
               uri == "com.webos.service.settings/getSystemSettings") {
        Json settings = Json::object();
        const std::string category = callPayload["category"].string();
        if (category == "general")
            settings.set("homePromotion", Json::text("off"));
        else if (category == "sound")
            settings.set("soundMode", Json::text("movie"));
        reply.set("returnValue", Json::flag(true));
        reply.set("settings", settings);
    } else if (mode == "backup" &&
               uri == "com.webos.service.settings/setSystemSettings") {
        reply.set("returnValue", Json::flag(true));
    } else if (mode == "backup" &&
               uri == "com.webos.applicationManager/listApps") {
        Json apps = Json::array();
        Json zed = Json::object();
        zed.set("id", Json::text("com.example.zed"));
        zed.set("title", Json::text("Zed"));
        zed.set("version", Json::numeric(2));
        zed.set("folderPath", Json::text(
            "/media/developer/apps/com.example.zed"));
        apps.add(zed);
        Json alpha = Json::object();
        alpha.set("id", Json::text("com.example.alpha"));
        alpha.set("version", Json::flag(true));
        alpha.set("folderPath", Json::text(
            "/media/developer/apps/com.example.alpha"));
        apps.add(alpha);
        Json protectedApp = Json::object();
        protectedApp.set("id", Json::text("org.webosbrew.hbchannel"));
        protectedApp.set("folderPath", Json::text(
            "/media/developer/apps/org.webosbrew.hbchannel"));
        apps.add(protectedApp);
        Json hdmi = Json::object();
        hdmi.set("id", Json::text("com.webos.app.input.hdmi2"));
        hdmi.set("folderPath", Json::text(
            "/media/developer/apps/com.webos.app.input.hdmi2"));
        apps.add(hdmi);
        Json systemApp = Json::object();
        systemApp.set("id", Json::text("com.example.system"));
        systemApp.set("folderPath", Json::text("/usr/palm/applications/system"));
        apps.add(systemApp);
        reply.set("returnValue", Json::flag(true));
        reply.set("apps", apps);
    } else if (mode == "app-install-recovery" &&
               uri == "com.webos.applicationManager/listApps") {
        std::string installMode;
        std::ifstream installModeFile(environment("NATIVETV_TEST_INSTALL_MODE"));
        std::getline(installModeFile, installMode);
        Json apps = Json::array();
        if (installMode == "closed-recovered" ||
            installMode == "timeout-recovered") {
            Json app = Json::object();
            app.set("id", Json::text("com.example.app"));
            app.set("version", Json::text("1.2.3"));
            apps.add(app);
        }
        reply.set("returnValue", Json::flag(true));
        reply.set("apps", apps);
    } else if (mode == "home-screen" &&
               uri == "com.webos.service.eim/getAllInputStatus") {
        Json device = Json::object();
        device.set("id", Json::text("HDMI_2"));
        device.set("label", Json::text("TV"));
        Json devices = Json::array();
        devices.add(device);
        reply.set("returnValue", Json::flag(true));
        reply.set("devices", devices);
    } else if (mode == "home-screen" &&
               (uri == "com.webos.applicationManager/addLaunchPoint" ||
                uri == "com.webos.applicationManager/updateLaunchPoint" ||
                uri == "com.webos.applicationManager/removeLaunchPoint" ||
                uri == "com.webos.applicationManager/moveLaunchPoint" ||
                uri == "com.webos.applicationManager/launch")) {
        reply.set("returnValue", Json::flag(true));
        if (uri == "com.webos.applicationManager/addLaunchPoint")
            reply.set("launchPointId", Json::text(std::to_string(1001 + previousHomeAdds)));
    } else if (mode == "sam-rescan" &&
               uri == "com.webos.applicationManager/getForegroundAppInfo") {
        reply.set("returnValue", Json::flag(true));
        const std::string foreground = environment("NATIVETV_FAKE_SAM_FOREGROUND");
        reply.set("appId", Json::text(foreground.empty()
            ? "com.webos.app.hdmi2" : foreground));
    } else if (mode == "sam-rescan" &&
               uri == "com.webos.applicationManager/getAppInfo") {
        Json appInfo = Json::object();
        std::string visible = environment("NATIVETV_FAKE_SAM_VISIBLE");
        const std::string visiblePath = environment("NATIVETV_FAKE_SAM_VISIBLE_FILE");
        if (!visiblePath.empty()) {
            std::ifstream state(visiblePath.c_str());
            std::string fromFile;
            if (std::getline(state, fromFile)) visible = fromFile;
        }
        appInfo.set("visible", Json::flag(visible == "true"));
        reply.set("returnValue", Json::flag(true));
        reply.set("appInfo", appInfo);
    } else if (mode == "sam-rescan" &&
               uri == "com.webos.applicationManager/listLaunchPoints") {
        Json points = Json::array();
        Json first = Json::object();
        first.set("id", Json::text("netflix"));
        first.set("launchPointId", Json::text("netflix"));
        points.add(first);
        Json second = Json::object();
        second.set("id", Json::text("youtube.leanback.v4"));
        second.set("launchPointId", Json::text("youtube.leanback.v4"));
        points.add(second);
        reply.set("returnValue", Json::flag(true));
        reply.set("launchPoints", points);
    } else if (mode == "sam-rescan" &&
               uri == "com.webos.applicationManager/launch") {
        reply.set("returnValue", Json::flag(true));
    } else if (mode == "sam-rescan" &&
               (uri == "com.webos.service.config/setConfigs" ||
                uri == "com.webos.applicationManager/blockSystemApp" ||
                uri == "com.webos.applicationManager/releaseBlockSystemApp")) {
        reply.set("returnValue", Json::flag(true));
    } else if (((mode == "legacy-settings" && startsWith(uri, "com.webos.service.settings/")) ||
         (mode == "legacy-systemservice" &&
          startsWith(uri, "com.webos.service.systemservice/")))) {
        reply.set("returnValue", Json::flag(false));
        reply.set("errorText", Json::text("Service does not exist"));
    } else if (mode == "audio-unknown-method" &&
               uri == "com.webos.service.audio/master/getVolume") {
        reply.set("returnValue", Json::flag(false));
        reply.set("errorText", Json::text("Unknown method getVolume"));
    } else if (mode == "media-acb-absent" &&
               uri == "com.webos.service.acb/getForegroundAppInfo") {
        reply.set("returnValue", Json::flag(false));
        reply.set("errorText", Json::text("Service does not exist: com.webos.service.acb"));
    } else if (mode == "piccap-unknown" &&
               uri == "org.webosbrew.piccap.service/status") {
        reply.set("returnValue", Json::flag(false));
        reply.set("errorText", Json::text("Service does not exist"));
    } else if (mode == "remote-battery" &&
               uri == "com.webos.service.mrcu/getConnectionStatus") {
        reply.set("returnValue", Json::flag(true));
        reply.set("connected", Json::flag(true));
        reply.set("isBleRemocon", Json::flag(subscription));
    } else if (mode == "remote-battery" &&
               uri == "com.webos.service.mrcu/getDeviceInfo") {
        reply.set("returnValue", Json::flag(true));
        const std::string batteryLevel = environment("NATIVETV_FAKE_LUNA_BATTERY_LEVEL");
        reply.set("batteryLevel", batteryLevel.empty()
                                       ? Json()
                                       : Json::numeric(std::atoi(batteryLevel.c_str())));
        reply.set("bdAddress", Json::text("00:00:5e:00:53:02"));
        reply.set("lowStrength", Json::flag(false));
        reply.set("fwVer", Json::text("23.63.04.1"));
    } else if (mode == "remote-battery" &&
               uri == "com.webos.service.bluetooth2/device/getStatus") {
        Json devices = Json::array();
        Json remote = Json::object();
        remote.set("paired", Json::flag(true));
        remote.set("typeOfDevice", Json::text("ble"));
        remote.set("name", Json::text("LGE MR24"));
        remote.set("address", Json::text("00:00:5e:00:53:02"));
        devices.add(remote);
        reply.set("devices", devices);
    } else if (mode == "remote-battery" &&
               uri == "com.webos.service.bluetooth2/gatt/connect") {
        reply.set("returnValue", Json::flag(true));
        reply.set("clientId", Json::text("battery-client"));
    } else if (mode == "remote-battery" &&
               uri == "com.webos.service.bluetooth2/gatt/discoverServices") {
        const bool failDiscovery = environment("NATIVETV_FAKE_LUNA_DISCOVER_FAIL") == "1";
        reply.set("returnValue", Json::flag(!failDiscovery));
    } else if (mode == "remote-battery" &&
               uri == "com.webos.service.bluetooth2/gatt/readCharacteristicValue") {
        reply.set("returnValue", Json::flag(true));
        Json bytes = Json::array();
        bytes.add(Json::numeric(77));
        Json value = Json::object();
        value.set("bytes", bytes);
        Json wrapped = Json::object();
        wrapped.set("value", value);
        reply.set("value", wrapped);
    } else if (mode == "oled-sysprop" &&
               uri == "com.webos.service.oledepl/getGlobalStressReduction") {
        reply.set("returnValue", Json::flag(false));
        reply.set("errorText", Json::text("Service does not exist"));
    } else if (mode == "oled-panelcontroller" &&
               uri == "com.webos.service.tv.systemproperty/getSystemProperties") {
        // A B3 on webOS 10.3.2: the model is known, the usage keys are refused.
        if (callPayload["keys"][0].string() == "modelName") {
            reply.set("returnValue", Json::flag(true));
            reply.set("modelName", Json::text("OLED65B3PUA"));
        } else {
            reply.set("returnValue", Json::flag(false));
            reply.set("errorCode", Json::text("ERROR_06"));
            reply.set("errorText", Json::text("Invalid argument"));
        }
    } else if (mode == "oled-panelcontroller" &&
               uri == "com.webos.service.panelcontroller/getPanelUsageTime") {
        if (!callPayload.has("subscribe")) {
            reply.set("returnValue", Json::flag(false));
            reply.set("errorCode", Json::numeric(3));
            reply.set("errorMessage",
                      Json::text("Failed to validate against schema: Missing mandatory field: 'subscribe'"));
        } else {
            reply.set("returnValue", Json::flag(true));
            reply.set("panelUsageTime", Json::numeric(34421));
        }
    } else if (mode == "oled-care" &&
               uri == "com.webos.service.tv.systemproperty/getSystemProperties") {
        reply.set("returnValue", Json::flag(true));
        if (callPayload["keys"][0].string() == "modelName") {
            reply.set("modelName", Json::text("OLED65C2PUA"));
        } else {
            reply.set("panelUsageTime", Json::text("6000"));
            reply.set("lastCompensationTimestamp", Json::text("5994"));
        }
    } else if (mode == "oled-care" &&
               uri == "com.webos.service.tv.display/getClearPanelNoiseStatus") {
        reply.set("returnValue", Json::flag(true));
        reply.set("status", Json::text("processing"));
    } else if (mode == "oled-care" &&
               uri == "com.webos.service.oledepl/getGlobalStressReduction") {
        reply.set("returnValue", Json::flag(true));
        reply.set("enable", Json::flag(true));
        reply.set("stressCount", Json::numeric(17));
    } else if (mode == "oled-care" &&
               uri == "com.webos.service.oledepl/getTemporalPeakControl") {
        reply.set("returnValue", Json::flag(true));
        reply.set("enable", Json::flag(false));
    } else if (mode == "oled-care" &&
               uri == "com.webos.service.settings/getSystemSettings" &&
               callPayload["category"].string() == "picture") {
        reply.set("returnValue", Json::flag(true));
        Json settings = Json::object();
        settings.set("screenShift", Json::text("on"));
        settings.set("logoLuminanceAdjust", Json::text("strong"));
        reply.set("settings", settings);
    } else if (mode == "privacy-acr" &&
               uri == "com.webos.service.settings/getSystemSettings" &&
               callPayload["category"].string() == "option") {
        std::string livePlus = "on";
        std::ifstream priorCalls(detailPath.c_str());
        std::string line;
        while (std::getline(priorCalls, line)) {
            const Json call = Json::parse(line);
            if (call["uri"].string() ==
                    "com.webos.service.settings/setSystemSettings" &&
                call["payload"]["category"].string() == "option") {
                const std::string value =
                    call["payload"]["settings"]["livePlus"].string();
                if (!value.empty()) livePlus = value;
            }
        }
        Json settings = Json::object();
        settings.set("livePlus", Json::text(livePlus));
        reply.set("returnValue", Json::flag(true));
        reply.set("settings", settings);
    } else if (mode == "privacy-lmt" &&
               uri == "com.webos.settingsservice/getSystemSettings") {
        std::string lmt = "off";
        std::ifstream priorCalls(detailPath.c_str());
        std::string line;
        while (std::getline(priorCalls, line)) {
            const Json call = Json::parse(line);
            if (call["uri"].string() ==
                    "com.webos.settingsservice/setSystemSettings" &&
                call["payload"]["category"].string() == "general") {
                const std::string value =
                    call["payload"]["settings"]["lmt"].string();
                if (!value.empty()) lmt = value;
            }
        }
        Json settings = Json::object();
        settings.set("lmt", Json::text(lmt));
        reply.set("returnValue", Json::flag(true));
        reply.set("settings", settings);
    } else if (mode == "privacy-lmt" &&
               uri == "com.webos.service.admanager/getAdid") {
        std::string lmt = "off";
        std::ifstream priorCalls(detailPath.c_str());
        std::string line;
        while (std::getline(priorCalls, line)) {
            const Json call = Json::parse(line);
            if (call["uri"].string() ==
                    "com.webos.settingsservice/setSystemSettings" &&
                call["payload"]["category"].string() == "general") {
                const std::string value =
                    call["payload"]["settings"]["lmt"].string();
                if (!value.empty()) lmt = value;
            }
        }
        reply.set("returnValue", Json::flag(true));
        reply.set("IFA", Json::text("fixture-ad-id"));
        reply.set("LMT", Json::text(lmt));
    } else if (mode == "privacy-reset-adid" &&
               uri == "com.webos.service.admanager/resetIFA") {
        reply.set("returnValue", Json::flag(true));
    } else if (mode == "privacy-reset-adid" &&
               uri == "com.webos.service.admanager/getAdid") {
        bool reset = false;
        std::ifstream priorCalls(detailPath.c_str());
        std::string line;
        while (std::getline(priorCalls, line)) {
            const Json call = Json::parse(line);
            if (call["uri"].string() ==
                "com.webos.service.admanager/resetIFA")
                reset = true;
        }
        reply.set("returnValue", Json::flag(true));
        reply.set("IFA", Json::text(reset ? "fixture-ad-id-after"
                                            : "fixture-ad-id-before"));
    } else if (mode == "privacy-read" &&
               uri == "com.webos.settingsservice/getSystemSettings") {
        Json settings = Json::object();
        const Json keys = callPayload["keys"];
        for (const Json &requested : keys.items()) {
            const std::string key = requested.string();
            if (key == "smartServiceCountryCode2")
                settings.set(key, Json::text("US"));
            else if (key == "livePlus")
                settings.set(key, Json::text("off"));
            else if (key == "lmt")
                settings.set(key, Json::text("on"));
            else if (key == "eulaInfoNetwork")
                settings.set(key, Json::parse(
                    R"({"region":"GB","eulaList":[{"id":"acr-doc","title":"Screen recognition agreement","accepted":true},{"id":"terms-doc","title":"Terms","accepted":true}]})"));
        }
        reply.set("returnValue", Json::flag(true));
        reply.set("settings", settings);
    } else if (mode == "privacy-read" &&
               uri == "com.webos.service.config/getConfigs") {
        Json configs = Json::object();
        configs.set("tv.model.supportFarFieldVoice", Json::text("on with switch"));
        reply.set("returnValue", Json::flag(true));
        reply.set("configs", configs);
    } else if (mode == "privacy-read" &&
               uri == "com.webos.service.acr/getACRSolutionStatus") {
        reply.set("returnValue", Json::flag(true));
        reply.set("ACRSolutionStatus", Json::flag(true));
    } else if (mode == "privacy-read" &&
               uri == "com.webos.service.acr/getVideoCaptureStatus") {
        reply.set("returnValue", Json::flag(true));
        reply.set("status", Json::text("capturing"));
    } else if (mode == "privacy-read" &&
               uri == "com.webos.service.admanager/getAdid") {
        reply.set("returnValue", Json::flag(true));
        reply.set("IFA", Json::text("fixture-ad-id"));
        reply.set("LMT", Json::text("off"));
    } else if ((mode == "privacy-consent" || mode == "privacy-acr") &&
               uri == "com.webos.settingsservice/getSystemSettings") {
        Json flags = mode == "privacy-acr"
            ? Json::parse(R"({"acrAllowed":false,"acrOnAllowed":false,"acrAdAllowed":true,"generalTermsAllowed":false})")
            : Json::parse(R"({"acrAllowed":true,"acrOnAllowed":false,"acrAdAllowed":true,"generalTermsAllowed":false})");
        Json documents = Json::parse(
            R"({"region":"GB","eulaList":[{"id":"acr-doc","accepted":true},{"id":"other-doc","accepted":true}]})");
        std::ifstream priorCalls(detailPath.c_str());
        std::string line;
        while (std::getline(priorCalls, line)) {
            const Json call = Json::parse(line);
            if (call["uri"].string() !=
                "com.webos.settingsservice/setSystemSettings")
                continue;
            const Json update = call["payload"]["settings"];
            if (update["eulaStatus"].isObject())
                flags = update["eulaStatus"];
            if (update["eulaInfoNetwork"].isObject())
                documents = update["eulaInfoNetwork"];
        }
        Json settings = Json::object();
        for (const Json &requested : callPayload["keys"].items()) {
            if (requested.string() == "eulaStatus")
                settings.set("eulaStatus", flags);
            else if (requested.string() == "eulaInfoNetwork")
                settings.set("eulaInfoNetwork", documents);
        }
        reply.set("returnValue", Json::flag(true));
        reply.set("settings", settings);
    } else if (mode == "media-controls" &&
               uri == "com.webos.service.acb/getForegroundAppInfo") {
        Json player = Json::object();
        const std::string state = environment("NATIVETV_FAKE_LUNA_PLAY_STATE");
        const std::string type = environment("NATIVETV_FAKE_LUNA_PLAYER_TYPE");
        player.set("playStateNow", Json::text(state.empty() ? "playing" : state));
        player.set("playerType", Json::text(type.empty() ? "MSE" : type));
        player.set("isFullScreen", Json::flag(true));
        Json acbs = Json::array();
        acbs.add(player);
        reply.set("acbs", acbs);
    } else if (mode == "media-idle" &&
               uri == "com.webos.service.acb/getForegroundAppInfo") {
        reply.set("acbs", Json::array());
    } else if (mode == "home-unavailable" &&
               uri == "com.webos.applicationManager/launch" &&
               callPayload["id"].string() == "com.webos.app.home") {
        reply.set("returnValue", Json::flag(false));
        reply.set("errorText", Json::text("App does not exist"));
    } else if (mode == "quick-access" &&
               uri == "com.webos.audio/setVolume" &&
               callPayload["volume"].number() == 0) {
        reply.set("returnValue", Json::flag(false));
        reply.set("errorText", Json::text("Current Scenario doesn't support volume"));
    } else if (mode == "quick-access" &&
               uri == "com.webos.service.networkinput/test/sendKeyCode" &&
               callPayload["keyCode"].number() == 115) {
        reply.set("returnValue", Json::flag(false));
    } else if (mode == "quick-access" &&
               uri == "com.webos.service.settings/setSystemSettings" &&
               callPayload["category"].string() == "sound" &&
               callPayload["settings"]["soundOutput"].string() == "optical") {
        reply.set("returnValue", Json::flag(false));
    } else if (mode == "quick-access" &&
               uri == "com.webos.service.tvpower/power/powerOff" &&
               callPayload["reason"].string() == "remoteKey") {
        reply.set("returnValue", Json::flag(false));
        reply.set("errorText", Json::text("initial power transition pending"));
    } else if (mode == "quick-access" &&
               uri == "com.webos.service.tvpower/power/powerOff" &&
               callPayload["reason"].string() == "localKey") {
        reply.set("returnValue", Json::flag(false));
        reply.set("errorText", Json::text("com.webos.service.tvpower/power is busy"));
    } else if (mode == "quick-access" &&
               uri == "com.webos.service.tvpower/power/powerOn") {
        reply.set("returnValue", Json::flag(false));
        reply.set("errorText", Json::text("power-on request refused"));
    } else if (mode == "remote-keys-native" &&
               uri == "com.webos.service.config/getConfigs") {
        Json configs = Json::object();
        Json nativeFilter = Json::object();
        nativeFilter.set("file", Json::text("/usr/lib/libappLaunch.so"));
        nativeFilter.set("handler", Json::text("handleNativeHotkeys"));
        Json filterChain = Json::array();
        filterChain.add(nativeFilter);
        configs.set("com.webos.surfacemanager.keyFiltersCpp", filterChain);
        configs.set("com.webos.keyaction.tvGuide",
                    Json::parse("{\"action\":\"launch\",\"id\":\"guide.app\"}"));
        configs.set("com.webos.keyaction.inputHub",
                    Json::parse("{\"action\":\"launch\",\"id\":\"hub.app\"}"));
        std::ifstream prior(detailPath.c_str());
        std::string line;
        while (std::getline(prior, line)) {
            const Json call = Json::parse(line);
            if (call["uri"].string() != "com.webos.service.config/setConfigs")
                continue;
            for (const char *name : {"tvGuide", "inputHub"}) {
                const std::string key = std::string("com.webos.keyaction.") + name;
                if (call["payload"]["configs"].has(key))
                    configs.set(key, call["payload"]["configs"][key]);
            }
        }
        reply.set("returnValue", Json::flag(true));
        reply.set("configs", configs);
    } else if (mode == "remote-keys-native" &&
               uri == "com.webos.settingsservice/getSystemSettings" &&
               callPayload["category"].string() == "other") {
        Json settings = Json::object();
        Json mappingInfo = Json::parse(
            "[{\"netflix\":{\"launch_param\":null,\"app_id\":"
            "\"netflix\",\"isActive\":true}},{\"amazon\":{"
            "\"launch_param\":null,\"app_id\":\"amazon\","
            "\"isActive\":true}}]");
        Json customShortPress = Json::parse(
            "{\"id\":\"accessibility\",\"type\":\"app\",\"params\":{}}");
        Json customLongPress = Json::parse(
            "{\"id\":\"help\",\"type\":\"app\",\"params\":{}}");
        std::ifstream prior(detailPath.c_str());
        std::string line;
        while (std::getline(prior, line)) {
            const Json call = Json::parse(line);
            if (call["rejected"].boolean())
                continue;
            if (call["uri"].string() !=
                    "com.webos.settingsservice/setSystemSettings" ||
                call["payload"]["category"].string() != "other")
                continue;
            const Json written = call["payload"]["settings"];
            if (written.has("mapping_info"))
                mappingInfo = written["mapping_info"];
            if (written.has("customShortPress"))
                customShortPress = written["customShortPress"];
            if (written.has("customLongPress"))
                customLongPress = written["customLongPress"];
        }
        for (const Json &key : callPayload["keys"].items()) {
            if (key.string() == "mapping_info") {
                settings.set("mapping_info", mappingInfo);
            } else if (key.string() == "customShortPress") {
                settings.set("customShortPress", customShortPress);
            } else if (key.string() == "customLongPress") {
                settings.set("customLongPress", customLongPress);
            }
        }
        reply.set("returnValue", Json::flag(true));
        reply.set("settings", settings);
    } else if (mode == "remote-keys-native" &&
               uri == "com.webos.applicationManager/listApps") {
        Json apps = Json::array();
        Json youtube = Json::object();
        youtube.set("id", Json::text("org.example.youtube"));
        youtube.set("title", Json::text("YouTube"));
        youtube.set("visible", Json::flag(true));
        apps.add(youtube);
        Json netflix = Json::object();
        netflix.set("id", Json::text("netflix"));
        netflix.set("title", Json::text("Netflix"));
        netflix.set("visible", Json::flag(true));
        apps.add(netflix);
        reply.set("returnValue", Json::flag(true));
        reply.set("apps", apps);
    } else if (mode == "remote-keys-native" &&
               uri == "com.webos.service.eim/getAllInputStatus") {
        Json device = Json::object();
        device.set("id", Json::text("HDMI_1"));
        device.set("label", Json::text("Fixture HDMI 1"));
        Json devices = Json::array();
        devices.add(device);
        reply.set("returnValue", Json::flag(true));
        reply.set("devices", devices);
    } else if (access(environment("NATIVETV_FAKE_NO_HOME_APP").c_str(), F_OK) == 0 &&
               uri == "com.webos.applicationManager/getAppInfo" &&
               callPayload["id"].string() == "com.webos.app.home") {
        // webOS 3-5 has no Home app.
        reply.set("returnValue", Json::flag(false));
    } else if (mode == "remote-keys-native" &&
               uri == "com.webos.applicationManager/getAppInfo" &&
               callPayload["id"].string() == "com.webos.app.quickinputpicker") {
        reply.set("returnValue", Json::flag(true));
    } else if (mode == "remote-keys-native" &&
               uri == "com.webos.applicationManager/getAppInfo" &&
               callPayload["id"].string() == "amazon.alexa.view") {
        bool blocked = false;
        std::ifstream prior(detailPath.c_str());
        std::string line;
        while (std::getline(prior, line)) {
            const Json call = Json::parse(line);
            if (call["payload"]["id"].string() != "amazon.alexa.view")
                continue;
            if (call["uri"].string() ==
                    "com.webos.applicationManager/blockSystemApp")
                blocked = true;
            else if (call["uri"].string() ==
                         "com.webos.applicationManager/releaseBlockSystemApp")
                blocked = false;
        }
        reply.set("returnValue", Json::flag(!blocked));
    } else if (mode == "remote-keys-native" &&
               (uri == "com.webos.applicationManager/blockSystemApp" ||
                uri == "com.webos.applicationManager/releaseBlockSystemApp")) {
        reply.set("returnValue", Json::flag(true));
    } else if (mode == "remote-keys-native" &&
               (uri == "com.webos.notification/disable" ||
                uri == "com.webos.notification/enable")) {
        reply.set("returnValue", Json::flag(true));
    } else if (mode == "remote-keys-native" &&
               uri == "com.webos.settingsservice/setSystemSettings" &&
               environment("NATIVETV_FAKE_REMOTE_KEYS_RESTORE_FAIL") == "1" &&
               callPayload["settings"].has("mapping_info")) {
        reply.set("returnValue", Json::flag(false));
        reply.set("errorText", Json::text("native mapping write refused"));
    } else if (mode == "remote-keys-native" &&
               (uri == "com.webos.settingsservice/setSystemSettings" ||
                uri == "com.webos.service.config/setConfigs")) {
        reply.set("returnValue", Json::flag(true));
    } else if (mode == "apps-list" &&
               uri == "com.webos.applicationManager/listApps") {
        Json apps = Json::array();
        Json youtube = Json::object();
        youtube.set("id", Json::text("org.example.youtube"));
        youtube.set("title", Json::text("YouTube"));
        youtube.set("version", Json::text("2.1"));
        youtube.set("vendor", Json::text("Example"));
        youtube.set("folderPath", Json::text(
            "/media/developer/apps/usr/palm/applications/org.example.youtube"));
        youtube.set("removable", Json::flag(true));
        youtube.set("appsize", Json::numeric(42455683));
        apps.add(youtube);
        Json gallery = Json::object();
        gallery.set("id", Json::text("com.webos.app.gallery"));
        gallery.set("title", Json::text("Gallery"));
        gallery.set("systemApp", Json::flag(true));
        gallery.set("removable", Json::flag(false));
        gallery.set("folderPath", Json::text(
            "/usr/palm/applications/com.webos.app.gallery"));
        apps.add(gallery);
        Json protectedApp = Json::object();
        protectedApp.set("id", Json::text("com.tvweb.dashboard"));
        protectedApp.set("title", Json::text("nativeTv"));
        protectedApp.set("removable", Json::flag(true));
        protectedApp.set("folderPath", Json::text(
            "/media/developer/apps/com.tvweb.dashboard"));
        apps.add(protectedApp);
        Json store = Json::object();
        store.set("id", Json::text("org.example.store"));
        store.set("title", Json::text("Store Copy"));
        store.set("removable", Json::flag(false));
        store.set("folderPath", Json::text("/media/cryptofs/apps/org.example.store"));
        apps.add(store);
        reply.set("returnValue", Json::flag(true));
        reply.set("apps", apps);
    } else if (mode == "apps-list" &&
               uri == "com.webos.applicationManager/listLaunchPoints") {
        Json points = Json::array();
        Json gallery = Json::object();
        gallery.set("id", Json::text("com.webos.app.gallery"));
        gallery.set("title", Json::text("Gallery Home"));
        gallery.set("systemApp", Json::flag(true));
        points.add(gallery);
        Json page = Json::object();
        page.set("id", Json::text("com.webos.app.browser"));
        page.set("lptype", Json::text("bookmark"));
        page.set("launchPointId", Json::text("page-1"));
        page.set("title", Json::text("Movie Site"));
        Json params = Json::object();
        params.set("target", Json::text("https://example.org/watch"));
        page.set("params", params);
        points.add(page);
        reply.set("returnValue", Json::flag(true));
        reply.set("launchPoints", points);
    } else if (mode == "apps-list" &&
               uri == "com.webos.service.config/getConfigs" &&
               callPayload["configNames"].items().size() == 1 &&
               callPayload["configNames"][size_t(0)].string() ==
                   "com.webos.surfacemanager.keyFilters") {
        Json entry = Json::object();
        entry.set("file", Json::text(environment("TVWEB_DATA_DIR") +
                                      "/remotekeys-filter.js"));
        entry.set("handler", Json::text("handleNativeTvKeys"));
        Json chain = Json::array();
        chain.add(entry);
        Json configs = Json::object();
        configs.set("com.webos.surfacemanager.keyFilters", chain);
        reply.set("returnValue", Json::flag(true));
        reply.set("configs", configs);
    } else if (mode == "apps-list" &&
               uri == "com.webos.service.eim/getAllInputStatus") {
        Json device = Json::object();
        device.set("id", Json::text("HDMI_1"));
        device.set("label", Json::text("Fixture HDMI 1"));
        Json devices = Json::array();
        devices.add(device);
        reply.set("returnValue", Json::flag(true));
        reply.set("devices", devices);
    } else if (mode == "apps-list" &&
               uri == "com.webos.applicationManager/getAppInfo" &&
               callPayload["id"].string() == "com.webos.app.quickinputpicker") {
        reply.set("returnValue", Json::flag(true));
        Json appInfo = Json::object();
        appInfo.set("id", Json::text("com.webos.app.quickinputpicker"));
        reply.set("appInfo", appInfo);
    } else if (mode == "apps-list" &&
               uri == "com.webos.service.config/setConfigs") {
        reply.set("returnValue", Json::flag(true));
    } else if (mode == "apps-uninstall" &&
               uri == "com.webos.appInstallService/remove") {
        const bool accepted = callPayload["id"].string() != "dev.app";
        reply.set("returnValue", Json::flag(accepted));
        if (!accepted)
            reply.set("errorText", Json::text("not a store app"));
    } else if (mode == "apps-uninstall" &&
               uri == "com.webos.appInstallService/dev/remove") {
        const bool accepted = callPayload["id"].string() == "dev.app";
        reply.set("returnValue", Json::flag(accepted));
        if (!accepted)
            reply.set("errorText", Json::text("developer removal refused"));
    } else {
        reply.set("returnValue", Json::flag(true));
        reply.set("uri", Json::text(uri));
        if (startsWith(uri, "com.webos.settingsservice/") ||
            startsWith(uri, "com.palm.systemservice/"))
            reply.set("legacy", Json::flag(true));
    }
    if (mode == "adblock" &&
        uri == "com.webos.settingsservice/getSystemSettings" &&
        callPayload["category"].string() == "option") {
        Json settings = Json::object();
        settings.set("smartServiceCountryCode2", Json::text("US"));
        reply.set("returnValue", Json::flag(true));
        reply.set("settings", settings);
    }
    if (reply["returnValue"].boolean() &&
        (stateGroupsMode || mode == "remote-battery" ||
         mode == "mqtt-discovery")) {
        if (uri == "com.webos.service.settings/getSystemSettings") {
            Json settings = Json::object();
            settings.set("backlight", Json::text("75"));
            if (mode == "mqtt-discovery") {
                settings.set("soundOutput", Json::text("tv_speaker"));
                settings.set("soundMode", Json::text("standard"));
            }
            reply.set("settings", settings);
        } else if (uri == "com.webos.service.tvpower/power/getPowerState") {
            reply.set("state", Json::text("Active"));
        } else if (uri == "com.webos.applicationManager/getForegroundAppInfo") {
            const Json foreground = readJsonFile(
                environment("NATIVETV_FAKE_LUNA_FOREGROUND_FILE"));
            reply.set("appId", Json::text(foreground["appId"].string(
                "com.webos.app.netflix")));
        } else if (uri == "com.webos.applicationManager/running") {
            Json running = Json::array();
            const std::string runningFile = environment("NATIVETV_FAKE_LUNA_RUNNING_FILE");
            if (!runningFile.empty()) {
                for (const Json &app : readJsonFile(runningFile)["running"].items())
                    running.add(app);
            } else {
                Json screensaver = Json::object();
                screensaver.set("id", Json::text("com.webos.app.screensaver"));
                running.add(screensaver);
            }
            reply.set("running", running);
        } else if (uri == "com.webos.surfacemanager/getForegroundAppInfo") {
            Json windows = Json::array();
            Json card = Json::object();
            card.set("appId", Json::text("com.webos.app.netflix"));
            card.set("displayId", Json::numeric(0));
            card.set("windowType", Json::text("_WEBOS_WINDOW_TYPE_CARD"));
            windows.add(card);
            reply.set("foregroundAppInfo", windows);
        } else if (uri == "com.webos.surfacemanager/getFocusedWindowInfo") {
            Json windows = Json::array();
            Json focused = Json::object();
            focused.set("appId", Json::text("com.webos.app.netflix"));
            focused.set("displayId", Json::numeric(0));
            windows.add(focused);
            reply.set("focusedWindowInfo", windows);
        } else if (uri == "com.webos.service.audio/master/getVolume") {
            Json status = Json::object();
            status.set("volume", Json::numeric(22));
            status.set("muteStatus", Json::flag(false));
            status.set("soundOutput", Json::text("tv_speaker"));
            reply.set("volumeStatus", status);
        } else if (uri == "com.webos.service.acb/getForegroundAppInfo") {
            Json players = Json::array();
            Json player = Json::object();
            player.set("playStateNow", Json::text("playing"));
            player.set("playerType", Json::text("MSE"));
            player.set("isFullScreen", Json::flag(true));
            players.add(player);
            reply.set("acbs", players);
        } else if (mode == "mqtt-discovery" &&
                   uri == "com.webos.applicationManager/listApps") {
            Json apps = Json::array();
            Json youtube = Json::object();
            youtube.set("id", Json::text("youtube.leanback.v4"));
            youtube.set("title", Json::text("YouTube"));
            youtube.set("visible", Json::flag(true));
            apps.add(youtube);
            Json netflix = Json::object();
            netflix.set("id", Json::text("netflix"));
            netflix.set("title", Json::text("Netflix"));
            netflix.set("visible", Json::flag(true));
            netflix.set("iconColor", Json::text("#AbC"));
            apps.add(netflix);
            reply.set("apps", apps);
        } else if (mode == "mqtt-discovery" &&
                   uri == "com.webos.service.settings/getSystemSettingValues" &&
                   callPayload["category"].string() == "sound") {
            Json values = Json::object();
            Json arrayExt = Json::array();
            for (const char *id : {"tv_speaker", "tv_speakerbar", "external_optical",
                                   "external_arc", "bt_soundbar", "mobile_phone",
                                   "lineout", "headphone", "tv_speaker_headphone",
                                   "tv_speaker_bluetooth", "tv_speaker_external_arc"}) {
                Json output = Json::object();
                output.set("value", Json::text(id));
                output.set("visible", Json::flag(true));
                output.set("active", Json::flag(true));
                arrayExt.add(output);
            }
            values.set("arrayExt", arrayExt);
            reply.set("values", values);
        } else if (mode == "mqtt-discovery" &&
                   uri == "com.webos.service.settings/getSystemSettingDesc") {
            Json results = Json::array();
            for (const Json &key : callPayload["keys"].items()) {
                if (key.string() != "soundMode")
                    continue;
                Json description = Json::object();
                description.set("key", Json::text("soundMode"));
                Json values = Json::object();
                Json arrayExt = Json::array();
                for (size_t index = 0; index < 3; ++index) {
                    const char *ids[] = {"standard", "movie", "news"};
                    Json item = Json::object();
                    item.set("value", Json::text(ids[index]));
                    if (index == 2)
                        item.set("visible", Json::flag(false));
                    arrayExt.add(item);
                }
                values.set("arrayExt", arrayExt);
                description.set("values", values);
                results.add(description);
            }
            reply.set("results", results);
        } else if (mode == "mqtt-discovery" &&
                   uri == "com.webos.service.settings/getSystemSettingValues") {
            Json values = Json::object();
            Json arrayExt = Json::array();
            Json hdrGame = Json::object();
            hdrGame.set("value", Json::text("hdrGame"));
            hdrGame.set("visible", Json::flag(true));
            hdrGame.set("active", Json::flag(true));
            arrayExt.add(hdrGame);
            Json cinema = Json::object();
            cinema.set("value", Json::text("cinema"));
            cinema.set("visible", Json::flag(true));
            cinema.set("active", Json::flag(false));
            arrayExt.add(cinema);
            Json hiddenMode = Json::object();
            hiddenMode.set("value", Json::text("vivid"));
            hiddenMode.set("visible", Json::flag(false));
            hiddenMode.set("active", Json::flag(true));
            arrayExt.add(hiddenMode);
            values.set("arrayExt", arrayExt);
            reply.set("values", values);
        }
    } else if (reply["returnValue"].boolean() && mode == "quick-access") {
        if (uri == "com.webos.service.oledepl/getGlobalStressReduction" ||
            uri == "com.webos.service.oledepl/getTemporalPeakControl") {
            const bool isGsr = uri ==
                "com.webos.service.oledepl/getGlobalStressReduction";
            const std::string settingUri = isGsr
                ? "com.webos.service.oledepl/setGlobalStressReduction"
                : "com.webos.service.oledepl/setTemporalPeakControl";
            bool enabled = false;
            std::ifstream priorCalls(detailPath.c_str());
            std::string line;
            while (std::getline(priorCalls, line)) {
                const Json call = Json::parse(line);
                const Json candidate = call["payload"]["enable"];
                if (call["uri"].string() == settingUri &&
                    candidate.isBoolean())
                    enabled = candidate.boolean();
            }
            reply.set("enable", Json::flag(enabled));
            if (isGsr)
                reply.set("stressCount", Json::numeric(17));
        } else if (uri == "com.webos.settingsservice/getSystemSettings") {
            bool hasServiceMenuFlag = false;
            bool serviceMenuFlag = false;
            std::ifstream priorCalls(detailPath.c_str());
            std::string line;
            while (std::getline(priorCalls, line)) {
                const Json call = Json::parse(line);
                const Json candidate =
                    call["payload"]["settings"]["svcMenuFlag"];
                if (call["uri"].string() ==
                        "com.webos.settingsservice/setSystemSettings" &&
                    candidate.isBoolean()) {
                    hasServiceMenuFlag = true;
                    serviceMenuFlag = candidate.boolean();
                }
            }
            Json settings = Json::object();
            if (hasServiceMenuFlag)
                settings.set("svcMenuFlag", Json::flag(serviceMenuFlag));
            reply.set("settings", settings);
        } else if (uri == "com.webos.service.config/getConfigs") {
            Json preload = Json::array();
            Json home = Json::object();
            home.set("id", Json::text("com.webos.app.home"));
            home.set("isEnabled", Json::flag(true));
            preload.add(home);
            Json input = Json::object();
            input.set("id", Json::text("com.webos.app.hdmi1"));
            input.set("isEnabled", Json::flag(true));
            preload.add(input);
            Json netflix = Json::object();
            netflix.set("id", Json::text("netflix"));
            netflix.set("isEnabled", Json::flag(true));
            netflix.set("requiredMemory", Json::numeric(100));
            preload.add(netflix);
            Json airplay = Json::object();
            airplay.set("id", Json::text("airplay"));
            airplay.set("isEnabled", Json::flag(false));
            preload.add(airplay);
            Json configs = Json::object();
            configs.set("com.webos.service.preloadmanager.supportAppList", preload);
            reply.set("configs", configs);
        } else if (uri == "com.webos.applicationManager/running") {
            Json running = Json::array();
            Json app = Json::object();
            app.set("id", Json::text("netflix"));
            running.add(app);
            reply.set("running", running);
        } else if (uri == "com.webos.service.settings/getSystemSettings") {
            Json settings = Json::object();
            for (unsigned key = 1; key <= 9; ++key) {
                Json value = Json::object();
                value.set("id", Json::text(key == 4 ? "youtube" : ""));
                value.set("params", Json::object());
                settings.set("magicNum" + std::to_string(key), value);
            }
            if (callPayload["category"].string() == "time") {
                // A test names the stored timer settings in a file it rewrites between calls.
                const Json stored = readJsonFile(
                    environment("NATIVETV_FAKE_TIME_SETTINGS_FILE"));
                if (stored.isObject()) {
                    for (const std::string &key : stored.keys())
                        settings.set(key, stored[key]);
                } else {
                    settings.set("onTimerEnable", Json::text("on"));
                    settings.set("onTimerAppId",
                                 Json::text("com.webos.app.livetv"));
                    settings.set("onTimerChannel", Json::text("noChannel"));
                }
            }
            reply.set("settings", settings);
            if (callPayload["category"].string() == "picture") {
                Json dimension = Json::object();
                dimension.set("input", Json::text(previousPictureReads == 0 ? "hdmi2" : "gallery"));
                reply.set("dimension", dimension);
            }
        } else if (uri == "com.webos.service.audio/master/getVolume") {
            Json status = Json::object();
            // A B8 on HDMI ARC with nothing answering never reports the new route.
            const bool soundMoved = previousSoundOutputWrite &&
                                    environment("NATIVETV_FAKE_LUNA_SOUND_STUCK") != "1";
            status.set("soundOutput", Json::text(soundMoved ? "external_optical" : "tv_speaker"));
            status.set("adjustVolume", Json::flag(!soundMoved));
            status.set("externalDeviceControl", Json::flag(soundMoved));
            reply.set("volumeStatus", status);
        } else if (uri == "com.webos.service.tvpower/power/getPowerState") {
            reply.set("state", Json::text("Active"));
        } else if (uri == "com.webos.applicationManager/listApps") {
            Json apps = Json::array();
            Json youtube = Json::object();
            youtube.set("id", Json::text("youtube"));
            youtube.set("title", Json::text("YouTube"));
            youtube.set("visible", Json::flag(true));
            apps.add(youtube);
            Json netflix = Json::object();
            netflix.set("id", Json::text("netflix"));
            netflix.set("title", Json::text("Netflix"));
            netflix.set("visible", Json::flag(true));
            apps.add(netflix);
            Json hidden = Json::object();
            hidden.set("id", Json::text("com.webos.app.self-diagnosis"));
            hidden.set("visible", Json::flag(false));
            apps.add(hidden);
            reply.set("apps", apps);
        } else if (uri == "com.webos.service.eim/getAllInputStatus") {
            Json devices = Json::array();
            Json device = Json::object();
            device.set("id", Json::text("HDMI_2"));
            device.set("label", Json::text("Apple TV"));
            devices.add(device);
            reply.set("devices", devices);
        }
    } else if (reply["returnValue"].boolean() && mode == "oled-sysprop") {
        if (uri == "com.webos.service.tv.systemproperty/getProperties") {
            std::string gsr = "false";
            std::string tpc = "true";
            std::ifstream priorCalls(detailPath.c_str());
            std::string line;
            while (std::getline(priorCalls, line)) {
                const Json call = Json::parse(line);
                const Json gsrValue = call["payload"]["OledGSR"];
                const Json tpcValue = call["payload"]["OledTPC"];
                if (call["uri"].string() ==
                        "com.webos.service.tv.systemproperty/setProperties") {
                    if (gsrValue.isString())
                        gsr = gsrValue.string();
                    if (tpcValue.isString())
                        tpc = tpcValue.string();
                }
            }
            reply.set("OledTPC", Json::text(tpc));
            reply.set("OledGSR", Json::text(gsr));
        }
    } else if (reply["returnValue"].boolean() && mode == "state-reconnect" &&
               uri == "com.webos.applicationManager/getForegroundAppInfo") {
        reply.set("appId", Json::text(loggedCalls > 1 ? "com.webos.app.youtube"
                                                       : "com.webos.app.netflix"));
    } else if (reply["returnValue"].boolean() && mode == "audio-unknown-method" &&
               uri == "com.webos.audio/getVolume") {
        reply.set("volume", Json::numeric(17));
        reply.set("muted", Json::flag(true));
        reply.set("scenario", Json::text("mastervolume_tv_speaker"));
    } else if (reply["returnValue"].boolean() && mode == "media-acb-absent" &&
               uri == "com.webos.media/getForegroundAppInfo") {
        Json windows = Json::array();
        Json player = Json::object();
        player.set("playState", Json::text("paused"));
        player.set("type", Json::text("media"));
        windows.add(player);
        reply.set("foregroundAppInfo", windows);
    }
    // The stats read's device detection (it asks for the firmware too).
    if (reply["returnValue"].boolean() && mode == "mqtt-discovery" &&
        uri == "com.webos.service.tv.systemproperty/getSystemProperties" &&
        callPayload["keys"][1].string() == "firmwareVersion") {
        reply.set("modelName", Json::text("OLED65B3PUA"));
        reply.set("firmwareVersion", Json::text("33.31.75"));
        reply.set("sdkVersion", Json::text("10.3.2"));
    }
    if (reply["returnValue"].boolean() && mode == "mqtt-discovery" &&
        uri == "com.webos.service.config/getConfigs") {
        Json configs = Json::object();
        Json bluetooth = Json::array();
        bluetooth.add(Json::text("btsound"));
        bluetooth.add(Json::text("bluetoothPlusTvSpeaker"));
        configs.set("system.supportBluetoothFeatures", bluetooth);
        configs.set("tv.model.supportHeadPhone", Json::flag(true));
        configs.set("tv.model.supportOpticalJack", Json::text("On"));
        configs.set("tv.model.supportAudioLineOut", Json::flag(true));
        configs.set("tv.model.supportWiSA", Json::flag(false));
        configs.set("com.webos.service.wowplay.supportWowCast", Json::flag(true));
        reply.set("configs", configs);
    }
    if (reply["returnValue"].boolean() && mode == "game-fps") {
        if (uri == "com.webos.applicationManager/getForegroundAppInfo") {
            const std::string foreground =
                environment("NATIVETV_FAKE_LUNA_FPS_FOREGROUND");
            reply.set("appId", Json::text(foreground == "offscreen"
                                               ? "com.webos.app.netflix"
                                               : "com.webos.app.lginput.hdmi2"));
        } else if (uri == "com.webos.service.eim/getAllInputStatus") {
            Json device = Json::object();
            device.set("id", Json::text("HDMI_2"));
            device.set("appId", Json::text("com.webos.app.lginput.hdmi2"));
            device.set("label", Json::text("Console"));
            Json devices = Json::array();
            devices.add(device);
            reply.set("devices", devices);
        } else if (subscription &&
                   uri == "com.webos.service.utp.extinputs/bind") {
            if (environment("NATIVETV_FAKE_LUNA_FPS_FORMAT") == "nosvc") {
                // A TV without the Game Optimizer pipeline.
                reply.set("returnValue", Json::flag(false));
                reply.set("errorText", Json::text("Unknown method \"bind\" for category \"/\""));
            } else {
                reply.set("broadcastId", Json::text("pipeline-hdmi2"));
            }
        } else if (uri == "com.webos.service.utp.extinputs/getVRRInfo") {
            Json info = Json::object();
            const std::string format = environment("NATIVETV_FAKE_LUNA_FPS_FORMAT");
            if (format == "legacy") {
                info.set("sublist_1", Json::text("60.0Hz"));
                info.set("sublist_2", Json::text("FIXED"));
            } else if (format == "false") {
                info.set("frameRate", Json::numeric(59));
                info.set("vrrType", Json::flag(false));
            } else {
                info.set("frameRate", Json::numeric(119.88));
                info.set("vrrType", Json::text("VRR"));
            }
            reply.set("vrrInfo", info);
            reply.set("port", Json::text("HDMI_2"));
        }
    }
    if (reply["returnValue"].boolean() && (mode == "piccap" || mode == "mqtt-discovery") &&
        uri == "org.webosbrew.piccap.service/status")
        reply.set("isRunning", Json::flag(previousPiccapRunning));
    if (subscription) {
        if (stateGroupsMode &&
            uri == "com.webos.applicationManager/getForegroundAppInfo") {
            const std::string foregroundPath =
                environment("NATIVETV_FAKE_LUNA_FOREGROUND_FILE");
            std::string previous = reply["appId"].string();
            std::cout << reply.dump() << '\n' << std::flush;
            for (;;) {
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
                const Json foreground = readJsonFile(foregroundPath);
                const std::string current = foreground["appId"].string(
                    "com.webos.app.netflix");
                if (current == previous) continue;
                previous = current;
                reply.set("appId", Json::text(current));
                std::cout << reply.dump() << '\n' << std::flush;
            }
        }
        if (mode == "home-now" &&
            uri == "com.webos.applicationManager/getForegroundAppInfo") {
            std::cout << reply.dump() << '\n' << std::flush;
            const std::string readyPath =
                environment("NATIVETV_FAKE_HOME_NOW_READY");
            if (!readyPath.empty()) {
                std::ofstream ready(readyPath.c_str());
                ready << "ready\n";
            }
            for (;;)
                std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (mode == "home-picture" &&
            uri == "com.webos.service.videooutput/getStatus") {
            std::cout << reply.dump() << '\n' << std::flush;
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            Json stopped = Json::object();
            stopped.set("returnValue", Json::flag(true));
            stopped.set("video", Json::array());
            std::cout << stopped.dump() << '\n' << std::flush;
            for (;;)
                std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (mode == "state-reconnect") {
            std::cout << reply.dump() << '\n' << std::flush;
            return 0;
        }
        if (mode == "window-refused" && uri.compare(0, 25, "com.webos.surfacemanager/") == 0) {
            // A busy service and a method the TV lacks, each ending the call.
            Json refusal = Json::object();
            refusal.set("returnValue", Json::flag(false));
            refusal.set("errorText", Json::text(uri == "com.webos.surfacemanager/getFocusedWindowInfo"
                ? "Unknown method \"getFocusedWindowInfo\" for category \"/\""
                : "Service is busy"));
            std::cout << refusal.dump() << '\n' << std::flush;
            return 0;
        }
        if (mode == "subscription" && reply["returnValue"].boolean()) {
            reply.set("count", Json::numeric(1));
            std::cout << reply.dump() << '\n' << std::flush;
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
            reply.set("count", Json::numeric(2));
            std::cout << reply.dump() << '\n' << std::flush;
            for (;;)
                std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (mode == "native-remote-keys-reconciler" && subscription &&
            reply["returnValue"].boolean()) {
            for (int count = 1; count <= 3; ++count) {
                reply.set("count", Json::numeric(count));
                std::cout << reply.dump() << '\n' << std::flush;
                if (count < 3)
                    std::this_thread::sleep_for(std::chrono::milliseconds(15));
            }
            for (;;)
                std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (mode == "subscription-pretty") {
            // SurfaceManager prints indented JSON even without -f; a brace sits
            // inside a string and the first write ends inside the object.
            const std::string pretty =
                "{\n    \"foregroundAppInfo\": [\n        {\n            \"appId\": \"test}\\\"app\",\n"
                "            \"displayId\": 0\n        }\n    ],\n    \"returnValue\": true\n}";
            std::cout << pretty.substr(0, 31) << std::flush;
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            std::cout << pretty.substr(31) << "\n{\"returnValue\":true,\"count\":2}\n" << std::flush;
            for (;;)
                std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (mode == "subscription-drop") {
            reply.set("count", Json::numeric(1));
            std::cout << reply.dump() << '\n';
            return 0;
        }
        std::cout << reply.dump() << '\n' << std::flush;
        for (;;)
            std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    std::cout << reply.dump() << '\n';
    return 0;
}
