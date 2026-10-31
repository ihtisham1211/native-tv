// The /api/stats collector against a scripted TV: what test/test-telemetry.js,
// test-live-reads.js, test-play-state.js, test-always-on.js, test-input-keys.js,
// test-power.js (the stats side), test-hostname.js and test-stats-early.js assert
// of the read, plus how the collector takes invalidations. The pure field
// projections are in stats_fields.cpp.
#include "daemon/stats_collector.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace nativetv;
using namespace nativetv::daemon;

namespace {
void check(bool value, const std::string &message) {
    if (!value) throw std::runtime_error(message);
}

std::string canon(const Json &v) {
    if (!v.valid()) return "undefined";
    if (v.isObject()) {
        std::vector<std::string> keys = v.keys();
        std::sort(keys.begin(), keys.end());
        std::string text = "{";
        for (const std::string &key : keys) text += "\"" + key + "\":" + canon(v[key]) + ",";
        return text + "}";
    }
    if (v.isArray()) {
        std::string text = "[";
        for (const Json &item : v.items()) text += canon(item) + ",";
        return text + "]";
    }
    if (v.isNumber()) {
        char text[64];
        snprintf(text, sizeof(text), "%.9g", v.number());
        return text;
    }
    return v.dump();
}

// An absent key and a null both read as "no value" to the dashboards, but the
// collector must leave a key out where the legacy server did (a JS undefined).
void expectJson(const Json &actual, const std::string &expected, const std::string &what) {
    const std::string a = canon(actual), b = canon(Json::parse(expected));
    check(a == b, what + ": " + a + " != " + b);
}

const char *kPower = "com.webos.service.tvpower/power/getPowerState";
const char *kApp = "com.webos.applicationManager/getForegroundAppInfo";
const char *kSound = "com.webos.audio/getSoundOut";
const char *kVolume = "com.webos.service.audio/master/getVolume";
const char *kAcb = "com.webos.service.acb/getForegroundAppInfo";
const char *kMedia = "com.webos.media/getForegroundAppInfo";
const char *kSettings = "com.webos.service.settings/getSystemSettings";
const char *kEim = "com.webos.service.eim/getAllInputStatus";
const char *kDesc = "com.webos.service.settings/getSystemSettingDesc";
const char *kListApps = "com.webos.applicationManager/listApps";

// A TV that answers each request from a script: the queue for that request, the
// last reply repeating, and nothing (the bus did not answer) for the rest.
// Settings are told apart by category.
struct Tv {
    std::map<std::string, std::deque<Json>> replies;
    std::map<std::string, int> asked;
    std::function<void(const std::string &)> onAsk;
    std::set<std::string> live;
    long long now = 1000000;
    Json config = Json::parse("{}");
    Json powerOn;
    Json liveMedia;
    Json hdmiMap = Json::object();
    std::string address;
    std::string host;
    int hostAsked = 0;
    bool mediaSeen = false;
    bool saverLaunched = false;
    int mediaSeenCalls = 0;
    std::vector<Json> hdmiSeen;
    std::unique_ptr<StatsCollector> collector;

    static std::string key(const std::string &uri, const Json &payload) {
        return uri == "com.webos.service.settings/getSystemSettings" ||
                   uri == "com.webos.service.settings/getSystemSettingValues"
            ? uri + "#" + payload["category"].string() : uri;
    }
    void say(const std::string &name, const std::string &json) { replies[name].push_back(Json::parse(json)); }
    void settings(const std::string &category, const std::string &json) {
        say(std::string(kSettings) + "#" + category, json);
    }
    int count(const std::string &name) const {
        const auto found = asked.find(name);
        return found == asked.end() ? 0 : found->second;
    }

    Tv() {
        StatsDeps d;
        d.luna = [this](const std::string &uri, const Json &payload, Json &reply) {
            const std::string name = key(uri, payload);
            ++asked[name];
            if (name != uri) ++asked[uri];
            if (onAsk) onAsk(name);
            auto found = replies.find(name);
            if (found == replies.end() || found->second.empty()) return false;
            reply = found->second.front();
            if (found->second.size() > 1) found->second.pop_front();
            return true;
        };
        d.liveGroup = [this](const std::string &group) { return live.count(group) == 1; };
        d.config = [this]() { return config; };
        d.systemRoot = "/nonexistent-root";
        d.version = "1.2.3";
        d.address4 = [this]() { return address; };
        d.hostname = [this](const std::string &) { ++hostAsked; return host; };
        d.hdmiReceiverMap = [this]() { return hdmiMap; };
        d.liveMedia = [this]() { return liveMedia; };
        d.powerOn = [this]() { return powerOn; };
        d.noteHdmiSeen = [this](const Json &diag) { hdmiSeen.push_back(diag); };
        d.noteMediaSeen = [this]() { ++mediaSeenCalls; mediaSeen = true; };
        d.hasMediaState = [this]() { return mediaSeen; };
        d.screensaverLaunched = [this]() { return saverLaunched; };
        d.nowMs = [this]() { return now; };
        d.isoTime = [](long long ms) { return std::to_string(ms); };
        collector.reset(new StatsCollector(d));
    }

    static Json system(double uptime = 1000) {
        return Json::parse("{\"ok\":true,\"uptime\":" + std::to_string(uptime) +
                           ",\"temp\":50,\"mem\":{\"total\":1000},\"cpu\":{\"currentLoad\":5}}");
    }
    Json collect(const Json &sys = system()) { return collector->collect(sys); }
    // A later read, past the 0.4 s reuse and the longest short-lived Luna reads.
    Json later(long long ms = 11000, const Json &sys = system()) {
        now += ms;
        return collect(sys);
    }
};

// ---- always-on --------------------------------------------------------------

void alwaysOnNotOfferedWithoutDescription() {
    Tv tv;
    tv.say(kDesc, "{\"returnValue\":false,\"errorText\":\"no result in DB\"}");
    tv.settings("general", "{\"returnValue\":true,\"settings\":{\"alwaysOn\":\"on\","
                           "\"alwaysOnDisableStartHour\":\"1\",\"alwaysOnDisableEndHour\":\"6\"}}");
    const Json s = tv.collect();
    check(!s.has("alwaysReady"), "the leftover \"on\" must not be reported");
    check(!s.has("alwaysReadyOff"), "nor its hours");
    tv.later();
    check(tv.count(kDesc) == 1, "a refusal from the TV is remembered");
}

void alwaysOnOfferedWhereDescribed() {
    Tv tv;
    tv.say(kDesc, "{\"returnValue\":true,\"results\":[{\"category\":\"general\","
                  "\"ui\":{\"displayName\":\"alwaysOn\",\"visible\":true}}]}");
    tv.settings("general", "{\"returnValue\":true,\"settings\":{\"alwaysOn\":\"on\","
                           "\"alwaysOnDisableStartHour\":\"1\",\"alwaysOnDisableStartMinute\":\"0\","
                           "\"alwaysOnDisableEndHour\":\"6\",\"alwaysOnDisableEndMinute\":\"30\"}}");
    const Json s = tv.collect();
    check(s["alwaysReady"].boolean(), "reported where the TV describes the setting");
    expectJson(s["alwaysReadyOff"], "{\"start\":\"01:00\",\"end\":\"06:30\"}", "nightly hours");
}

void alwaysOnLostAnswerIsNotRemembered() {
    Tv tv;  // nothing answers the description
    tv.settings("general", "{\"returnValue\":true,\"settings\":{\"alwaysOn\":\"on\"}}");
    check(!tv.collect().has("alwaysReady"), "an unanswered description offers nothing");
    tv.later();
    check(tv.count(kDesc) == 1, "it is asked once per start, as telemetry asks it at init");
}

// ---- input keys -------------------------------------------------------------

void inputKeysByPortUnderUnusualAppId() {
    Tv tv;
    tv.say(kEim,
           "{\"returnValue\":true,\"devices\":["
           "{\"id\":\"HDMI_1\",\"port\":1,\"appId\":\"com.webos.app.hdmi1\",\"label\":\"HDMI 1\"},"
           "{\"id\":\"HDMI_2\",\"port\":2,\"appId\":\"com.webos.app.hdmi2\",\"label\":\"Apple TV\"},"
           "{\"id\":\"HDMI_3\",\"port\":3,\"appId\":\"com.sling.tv\",\"label\":\"Sling TV\"},"
           "{\"id\":\"HDMI_4\",\"port\":4,\"label\":\"HDMI 4\"},"
           "{\"id\":\"SLING\",\"appId\":\"com.sling.channels\",\"label\":\"Sling channels\"}]}");
    tv.say(kApp, "{\"returnValue\":true,\"appId\":\"com.sling.tv\"}");
    const Json s = tv.collect();
    expectJson(s["inputs"], "{\"hdmi1\":\"HDMI 1\",\"hdmi2\":\"Apple TV\",\"hdmi3\":\"Sling TV\",\"hdmi4\":\"HDMI 4\"}",
               "every HDMI input is keyed by its port and named as the TV names it");
    check(s["app"].string() == "hdmi3", "the input on screen is known by its port: " + s["app"].string());
    check(s["app_id"].string() == "com.sling.tv", "the app id is kept");
    check(s["display_title"].string() == "Sling TV (HDMI3)", "named as the input it is: " + s["display_title"].string());

    tv.replies[kApp].clear();
    tv.say(kApp, "{\"returnValue\":true,\"appId\":\"com.webos.app.hdmi2\"}");
    check(tv.later(11000)["app"].string() == "hdmi2", "the standard id still maps to its port");
    tv.replies[kApp].clear();
    tv.say(kApp, "{\"returnValue\":true,\"appId\":\"netflix\"}");
    check(tv.later(11000)["app"].string() == "netflix", "an app is not an input");
}

// ---- live reads -------------------------------------------------------------

void seedLiveReads(Tv &tv) {
    tv.say(kApp, "{\"returnValue\":true,\"appId\":\"netflix\"}");
    tv.say(kSound, "{\"returnValue\":true,\"muted\":false,\"volume\":9,\"scenario\":\"mastervolume_tv_speaker\"}");
    tv.say(kVolume, "{\"returnValue\":true,\"volumeStatus\":{\"soundOutput\":\"tv_speaker\",\"volume\":9,"
                    "\"adjustVolume\":true,\"externalDeviceControl\":false,\"muteStatus\":false}}");
    tv.say(kPower, "{\"returnValue\":true,\"state\":\"Active\"}");
}

void liveReadsKeepShortLivesWithoutSubscription() {
    Tv tv;
    seedLiveReads(tv);
    tv.collect();
    tv.later();  // past every short lifetime (APP 4 s, audio 10 s)
    check(tv.count(kApp) == 2, "the foreground app is asked again");
    check(tv.count(kSound) == 2 && tv.count(kVolume) == 2, "the audio reads are asked again");
    check(tv.count(kPower) == 2, "power is asked fresh each collection");
}

void liveReadsAreKeptWhileDelivering() {
    Tv polled;
    seedLiveReads(polled);
    const Json down = polled.collect();
    Tv tv;
    seedLiveReads(tv);
    tv.live = {"application", "audio", "power"};
    const Json up = tv.collect();
    tv.later();  // 11 s: inside the live 60 s and the live power 30 s
    check(tv.count(kApp) == 1 && tv.count(kSound) == 1 && tv.count(kVolume) == 1,
          "reads a delivering subscription keeps fresh are not asked again");
    check(tv.count(kPower) == 1, "power is kept 30 s");
    tv.later(25000);  // 36 s: past power's 30 s, inside the 60 s of the rest
    check(tv.count(kPower) == 2, "power is kept less long than the rest");
    check(tv.count(kApp) == 1, "the rest stay kept");
    check(canon(up["powerState"]) == canon(down["powerState"]) && up["app"].string() == down["app"].string() &&
              canon(up["volume"]) == canon(down["volume"]),
          "the stats are the same either way");
}

void liveReadsFallBackPerGroup() {
    Tv tv;
    seedLiveReads(tv);
    tv.live = {"application", "power"};  // audio is not delivering
    tv.collect();
    tv.later();
    check(tv.count(kApp) == 1, "the group still delivering stays kept");
    check(tv.count(kSound) == 2 && tv.count(kVolume) == 2, "a group that stopped is polled again");
}

// ---- power ------------------------------------------------------------------

void powerStateCarriesReasonAndTimes() {
    Tv tv;
    tv.say(kPower, "{\"returnValue\":true,\"state\":\"Active\"}");
    tv.powerOn = Json::parse("{\"onReason\":\"alwaysOn\",\"onTime\":{\"uptime\":27196.63,\"ontime\":0}}");
    const Json s = tv.collect();
    check(s["powerState"]["raw"].string() == "Active", "the raw state");
    check(s["powerState"]["onReason"].string() == "alwaysOn", "why the TV is on");
    expectJson(s["powerState"]["onTime"], "{\"uptime\":27196.63,\"ontime\":0}", "the times");
    check(s["powerState"]["systemOn"].boolean() && s["powerState"]["screenOn"].boolean(), "Active is on");
}

void powerWithoutReasonIsNull() {
    Tv tv;
    tv.say(kPower, "{\"returnValue\":true,\"state\":\"Active\"}");
    const Json s = tv.collect();
    check(s["powerState"].isObject() && !s["powerState"]["onReason"].valid() && !s["powerState"]["onTime"].valid(),
          "a TV that gives no reason gets nulls");
}

void noPowerAnswerIsNotOff() {
    Tv tv;  // the bus does not answer
    const Json s = tv.collect();
    check(!s["powerState"].valid(), "no answer leaves the state out, not off");
    check(!s["screenSaver"].boolean(), "and no screen saver");
}

void powerFallsBackToProcessing() {
    Tv tv;
    tv.say(kPower, "{\"returnValue\":true,\"processing\":\"Active Standby\"}");
    check(tv.collect()["powerState"]["raw"].string() == "Active Standby", "a reply with only `processing`");
}

void alwaysReadyDisplayIsItsOwnPowerState() {
    Tv tv;
    tv.say(kPower, "{\"returnValue\":true,\"state\":\"Active\"}");
    tv.say("com.webos.service.tvpower/power2/getPowerState",
           "{\"returnValue\":true,\"state\":\"ACTIVE\",\"sub state\":\"always on display\"}");
    tv.settings("general", "{\"returnValue\":true,\"settings\":{\"lifeOnScreenMode\":\"allEnabled\"}}");
    const Json first = tv.collect();
    check(first["alwaysReadyScreen"].boolean(), "the screen mode is reported");
    check(first["powerState"]["raw"].string() == "Active", "known only once the mode was read");
    const Json second = tv.later(11000);
    check(second["powerState"]["raw"].string() == "Always Ready", "then the display reads as its own state");
}

void screenSaverFromPowerStateOrMarker() {
    Tv tv;
    tv.say(kPower, "{\"returnValue\":true,\"state\":\"Screen Saver\"}");
    check(tv.collect()["screenSaver"].boolean(), "the power state says so");
    Tv marked;
    marked.say(kPower, "{\"returnValue\":true,\"state\":\"Active\"}");
    check(!marked.collect()["screenSaver"].boolean(), "not while the TV is simply on");
    marked.saverLaunched = true;
    check(marked.later()["screenSaver"].boolean(), "the launched marker says so");
}

// ---- play state -------------------------------------------------------------

void playStateRemembersAndReadsIdle() {
    Tv tv;
    tv.say(kAcb, "{\"returnValue\":true,\"acbs\":[]}");
    const Json none = tv.collect();
    check(!none.has("media") && tv.mediaSeenCalls == 0, "nothing reported, nothing to say");

    tv.replies[kAcb].clear();
    tv.say(kAcb, "{\"returnValue\":true,\"acbs\":[{\"playStateNow\":\"playing\",\"playerType\":\"MSE\"}]}");
    const Json playing = tv.later(5000);
    expectJson(playing["media"], "{\"state\":\"playing\",\"playerType\":\"MSE\",\"fullScreen\":true}", "published");
    check(tv.mediaSeenCalls == 1, "and remembered");

    tv.replies[kAcb].clear();
    tv.say(kAcb, "{\"returnValue\":true,\"acbs\":[]}");
    const Json idle = tv.later(5000);
    check(idle["media"]["state"].string() == "idle", "idle with nothing playing, not unknown");
    check(tv.mediaSeenCalls == 1, "idle is not remembered");
}

void playStateStaysAfterRestart() {
    Tv restarted;
    restarted.mediaSeen = true;  // the marker an earlier run left
    restarted.say(kAcb, "{\"returnValue\":true,\"acbs\":[]}");
    check(restarted.collect()["media"]["state"].string() == "idle", "not withdrawn by a restart");
}

void playStateFromMediaServiceWithoutAcb() {
    Tv tv;
    tv.say(kAcb, "{\"returnValue\":false,\"errorCode\":-1,\"errorText\":\"Service does not exist: com.webos.service.acb.\"}");
    tv.say(kMedia, "{\"returnValue\":true,\"subscribed\":false,\"foregroundAppInfo\":[]}");
    check(!tv.collect().has("media"), "withheld until something plays");
    tv.replies[kMedia].clear();
    tv.say(kMedia, "{\"returnValue\":true,\"foregroundAppInfo\":[{\"appId\":\"youtube.leanback.v4\","
                   "\"playState\":\"playing\",\"type\":\"media\",\"mediaId\":\"x\"}]}");
    const Json playing = tv.later(5000);
    check(playing["media"]["state"].string() == "playing" && playing["media"]["playerType"].string() == "media",
          "a TV without acb gets com.webos.media's state");
    tv.replies[kMedia].clear();
    tv.say(kMedia, "{\"returnValue\":true,\"foregroundAppInfo\":[]}");
    check(tv.later(5000)["media"]["state"].string() == "idle", "nothing playing, as on an input");
}

void livePlayStateSuppressesReads() {
    Tv tv;
    tv.liveMedia = Json::parse("{\"state\":\"playing\",\"playerType\":\"MSE\",\"fullScreen\":true}");
    tv.say(kAcb, "{\"returnValue\":true,\"acbs\":[]}");
    const Json s = tv.collect();
    check(s["media"]["state"].string() == "playing", "the live state flows into the stats");
    check(tv.count(kAcb) == 0 && tv.count(kMedia) == 0, "a live source suppresses the duplicate reads");
    check(tv.mediaSeenCalls == 0, "and the marker is the daemon's to write, not the collector's");
}

// ---- hostname ---------------------------------------------------------------

void hostnameIsAskedOnlyWithAnAddress() {
    Tv tv;
    const Json none = tv.collect();
    check(!none["address"].valid() && !none["hostname"].valid() && tv.hostAsked == 0, "no address, no lookup");
    tv.address = "192.0.2.17";
    const Json first = tv.later();
    check(first["address"].string() == "192.0.2.17", "the address");
    check(!first["hostname"].valid(), "collected before the lookup answered");
    tv.host = "office-tv.example.net";
    check(tv.later()["hostname"].string() == "office-tv.example.net", "then the name");
}

// ---- sound, volume and the picture ------------------------------------------

void soundVolumeKeyFollowsTheReply() {
    Tv tv;
    tv.say(kSound, "{\"returnValue\":true,\"muted\":true}");
    const Json s = tv.collect();
    check(!s.has("volume"), "a reply without a level leaves the key out");
    check(s["muted"].boolean(), "muted is kept");
    check(s["volume_control"].string() == "level", "control: " + s["volume_control"].string());
}

void earcSoundbarLevelWinsOverZero() {
    Tv tv;
    tv.say(kSound, "{\"returnValue\":true,\"muted\":false,\"volume\":0,\"scenario\":\"mastervolume_ext_speaker_arc\"}");
    tv.say(kVolume, "{\"returnValue\":true,\"volumeStatus\":{\"soundOutput\":\"external_arc\",\"volume\":24,"
                    "\"adjustVolume\":false,\"externalDeviceControl\":true,\"volumeSyncable\":true,\"muteStatus\":true}}");
    const Json s = tv.collect();
    check(s["volume"].number() == 24 && s["volume_control"].string() == "level", "the soundbar's level and control");
    check(s["muted"].boolean(), "and its mute state");
}

void volumeNullWhereThereIsNoLevelControl() {
    Tv tv;
    tv.say(kSound, "{\"returnValue\":true,\"muted\":false,\"volume\":10,\"scenario\":\"mastervolume_ext_speaker_optical\"}");
    tv.say(kVolume, "{\"returnValue\":true,\"volumeStatus\":{\"soundOutput\":\"external_optical\",\"volume\":10,"
                    "\"adjustVolume\":false,\"externalDeviceControl\":false}}");
    const Json s = tv.collect();
    check(s["volume_control"].string() == "none" && s.has("volume") && !s["volume"].valid() &&
              s["volume"].dump() == "null",
          "optical has no volume to show");
}

void audioOutputNamedFromTheVolumeService() {
    const std::string tvSpeaker = "{\"returnValue\":true,\"muted\":false,\"volume\":0,\"scenario\":\"mastervolume_tv_speaker\"}";
    const std::string noScenario = "{\"returnValue\":false,\"errorCode\":3,\"errorText\":\"failed to get parameter\"}";
    const auto status = [](const std::string &output, const std::string &extra) {
        return "{\"returnValue\":true,\"volumeStatus\":{\"soundOutput\":\"" + output + "\",\"volume\":0," +
               "\"adjustVolume\":true,\"externalDeviceControl\":false,\"volumeSyncable\":true,\"muteStatus\":false" +
               extra + "}}";
    };
    struct Case {
        const char *name;
        std::string sound, volume, expected;
    } cases[] = {
        {"TV Speaker", tvSpeaker, status("tv_speaker", ""), "TV Speaker"},
        {"Optical + TV Speaker", "{\"returnValue\":true,\"muted\":false,\"volume\":0,\"scenario\":\"mastervolume_tv_speaker_ext\"}",
         status("tv_external_speaker", ",\"volumeSyncable\":false"), "TV Speaker + Optical"},
        {"Bluetooth + TV Speaker, nothing paired", noScenario,
         status("tv_speaker_bt_surround", ",\"externalDeviceControl\":true"), "TV Speaker + Bluetooth"},
        {"neither service naming an output", noScenario, "{\"returnValue\":false,\"errorText\":\"not answering\"}", "Internal"},
        // The newer service refused for good: from here on, an older TV.
        {"a TV without the volume service",
         "{\"returnValue\":true,\"muted\":false,\"volume\":-1,\"scenario\":\"mastervolume_ext_speaker_optical\"}",
         "{\"returnValue\":false,\"errorCode\":-1,\"errorText\":\"Unknown method \\\"getVolume\\\" for category \\\"/master\\\"\"}",
         "Optical"},
        {"a TV without the volume service, no scenario", noScenario, "", "Internal"},
    };
    Tv tv;
    bool first = true;
    for (const Case &c : cases) {
        tv.replies.clear();
        tv.say(kSound, c.sound);
        if (!c.volume.empty()) tv.say(kVolume, c.volume);
        const Json s = first ? tv.collect() : tv.later();
        first = false;
        check(s["audio_output"].string() == c.expected,
              std::string(c.name) + ": " + s["audio_output"].string() + " != " + c.expected);
    }
    const int asksOfRefusedService = tv.count(kVolume);
    tv.later();
    check(tv.count(kVolume) == asksOfRefusedService, "a service that said \"unknown method\" is not asked again");
}

void refusedVolumeHoldsUntilTheSoundGoesElsewhere() {
    Tv tv;
    const auto route = [&tv](const std::string &output) {
        tv.replies.clear();
        tv.say(kSound, "{\"returnValue\":true,\"muted\":false,\"volume\":10,\"scenario\":\"mastervolume_tv_speaker\"}");
        tv.say(kVolume, "{\"returnValue\":true,\"volumeStatus\":{\"soundOutput\":\"" + output +
                            "\",\"volume\":10,\"adjustVolume\":true,\"externalDeviceControl\":false,\"muteStatus\":false}}");
    };
    route("external_optical");
    check(tv.collect()["volume_control"].string() == "level", "as the TV reports it");
    tv.collector->noteVolumeRefused();
    Json s = tv.later(11000);
    check(s["volume_control"].string() == "none" && s["volume"].dump() == "null", "refused there");
    route("tv_speaker");
    check(tv.later(11000)["volume_control"].string() == "level", "the sound moved");
    route("external_optical");
    check(tv.later(11000)["volume_control"].string() == "level", "back on optical, offered again");
}

void refusedVolumeIsAppliedToTheNextReadAlreadyInReuse() {
    Tv tv;
    tv.say(kSound, "{\"returnValue\":true,\"muted\":false,\"volume\":10,\"scenario\":\"mastervolume_tv_speaker\"}");
    tv.say(kVolume, "{\"returnValue\":true,\"volumeStatus\":{\"soundOutput\":\"tv_speaker\",\"volume\":10,"
                    "\"adjustVolume\":true,\"externalDeviceControl\":false}}");
    check(tv.collect()["volume_control"].string() == "level", "level first");
    tv.collector->noteVolumeRefused();
    tv.now += 100;  // inside the 0.4 s reuse
    check(tv.collect()["volume_control"].string() == "none", "a refusal is not served the old answer");
}

void pictureShapes() {
    Tv tv;
    tv.settings("picture", "{\"returnValue\":true,\"dimension\":{\"dynamicRange\":\"sdr\"},"
                           "\"settings\":{\"pictureMode\":\"cinema\",\"backlight\":\"57\"}}");
    const Json s = tv.collect();
    check(s["picture"]["backlight"].number() == 57 && s["picture"]["backlight_raw"].number() == 57, "backlight read from a string");
    check(s["picture"]["mode_raw"].string() == "cinema" && s["picture"]["energySaving"].string() == "off",
          "mode and defaults");

    Tv gallery;
    gallery.settings("picture", "{\"returnValue\":true,\"dimension\":{\"input\":\"gallery\",\"dynamicRange\":\"sdr\"},"
                                "\"settings\":{\"pictureMode\":\"vivid\",\"backlight\":\"80\"}}");
    const Json g = gallery.collect();
    check(g["picture"]["mode"].string() == "Gallery" && g["picture"]["mode_raw"].string() == "gallery", "Gallery mode");
    check(g["picture"]["dynamicRange"].string() == "SDR", "dynamic range");
    check(g["picture"]["modes"].isArray() && g["picture"]["modes"].size() == 0, "and no preset list");
}

// ---- HDMI -------------------------------------------------------------------

const char *kFacts =
    "[{},{},{\"timing\":{\"connected\":true,\"width\":3840,\"height\":2160,\"refreshHz\":60},"
    "\"diagnosticConnected\":true,\"sourcePowered\":true,\"allm\":false,\"vrr\":\"off\","
    "\"link\":{\"locked\":true,\"phy_mode\":\"frl\",\"chroma\":\"rgb\",\"hdcp\":\"2.3\",\"tmds_clock_khz\":594000}},"
    "{\"sourcePowered\":false}]";

Json systemWithFacts() {
    Json sys = Tv::system();
    sys.set("hdmiPortFacts", Json::parse(kFacts));
    return sys;
}

void hdmiSignalFollowsTheInputOnScreen() {
    Tv tv;
    tv.hdmiMap = Json::parse("{\"1\":2,\"2\":3}");  // input -> receiver
    tv.say(kApp, "{\"returnValue\":true,\"appId\":\"org.jellyfin.webos\"}");
    Json s = tv.collect(systemWithFacts());
    check(!s["signal"].valid() && !s["hdmi_diag"].valid() && tv.hdmiSeen.empty(), "an app yields no signal");

    tv.replies[kApp].clear();
    tv.say(kApp, "{\"returnValue\":true,\"appId\":\"com.webos.app.hdmi1\"}");
    s = tv.later(11000, systemWithFacts());
    check(s["signal"].string() == "3840x2160 @ 60Hz", "HDMI 1 yields its receiver's signal: " + s["signal"].string());
    check(s["hdmi_diag"]["port"].number() == 2, "and its diagnostics");
    check(tv.hdmiSeen.size() == 1, "which are noted as seen");

    tv.replies[kApp].clear();
    tv.say(kApp, "{\"returnValue\":true,\"appId\":\"com.webos.app.hdmi2\"}");
    s = tv.later(11000, systemWithFacts());
    check(!s["signal"].valid() && !s["hdmi_diag"].valid(), "an idle receiver is not another's signal");
}

void hdmiLinksAndSources() {
    Tv tv;
    tv.hdmiMap = Json::parse("{\"1\":2,\"2\":3}");
    const Json s = tv.collect(systemWithFacts());
    check(s["hdmi_links"].size() == 1 && s["hdmi_links"][0]["input"].number() == 1 &&
              s["hdmi_links"][0]["receiver"].number() == 2,
          "a locked link is told by the input map");
    check(s["hdmi_sources"].size() == 2 && s["hdmi_sources"][0]["powered"].boolean() &&
              !s["hdmi_sources"][1]["powered"].boolean(),
          "so is a powered source");
}

void screenOffClearsTheSignal() {
    Tv tv;
    tv.hdmiMap = Json::parse("{\"1\":2}");
    tv.say(kApp, "{\"returnValue\":true,\"appId\":\"com.webos.app.hdmi1\"}");
    tv.say(kPower, "{\"returnValue\":true,\"state\":\"Active\"}");
    check(tv.collect(systemWithFacts())["signal"].string() == "3840x2160 @ 60Hz", "a signal while the screen is on");
    tv.replies[kPower].clear();
    tv.say(kPower, "{\"returnValue\":true,\"state\":\"Screen Saver\"}");
    check(!tv.later(11000, systemWithFacts())["signal"].valid(), "a sleeping source's leftover timing is not a signal");
}

// ---- apps -------------------------------------------------------------------

void installedAppsFromLaunchPoints() {
    Tv tv;
    tv.say(kListApps,
           "{\"returnValue\":true,\"launchPoints\":["
           "{\"id\":\"netflix\",\"title\":\"Netflix\",\"iconColor\":\"#E50914\"},"
           "{\"id\":\"com.webos.app.discovery\",\"title\":\"Apps\",\"iconColor\":\"red\"},"
           "{\"id\":\"com.webos.app.container\",\"title\":\"Container\"},"
           "{\"id\":\"hidden.app\",\"title\":\"Hidden\",\"visible\":false}]}");
    const Json s = tv.collect();
    check(s["apps"].size() == 2 && s["apps"][0]["id"].string() == "com.webos.app.discovery" &&
              s["apps"][1]["id"].string() == "netflix",
          "hidden and container apps are left out: " + canon(s["apps"]));
    check(s["apps"][1]["iconColor"].string() == "#e50914" && !s["apps"][0].has("iconColor"),
          "a valid iconColor is kept and anything else dropped");
}

void installedAppsFallBackToLaunchPoints() {
    Tv tv;
    tv.say(kListApps, "{\"returnValue\":false}");
    tv.say("com.webos.applicationManager/listLaunchPoints",
           "{\"returnValue\":true,\"launchPoints\":[{\"id\":\"amazon\",\"title\":\"Prime Video\"}]}");
    const Json s = tv.collect();
    check(s["apps"].size() == 1 && s["apps"][0]["id"].string() == "amazon", "listLaunchPoints answers when listApps fails");
}

void installedAppsRescanRules() {
    Tv tv;
    tv.say(kListApps, "{\"returnValue\":true,\"launchPoints\":[{\"id\":\"netflix\",\"title\":\"Netflix\"}]}");
    tv.collect();
    tv.replies[kListApps].clear();
    tv.say(kListApps, "{\"returnValue\":true,\"launchPoints\":[{\"id\":\"other\",\"title\":\"Other\"}]}");
    tv.collector->expire();
    Json s = tv.later(2000);
    check(s["apps"][0]["id"].string() == "netflix", "a live event expires the stats but keeps the app list");
    s = tv.later(61000);
    check(s["apps"].size() == 1 && s["apps"][0]["id"].string() == "other", "the list is read again after a minute");
    tv.replies[kListApps].clear();
    tv.say(kListApps, "{\"returnValue\":true,\"launchPoints\":[{\"id\":\"third\",\"title\":\"Third\"}]}");
    tv.collector->clearCache();
    s = tv.later(2000);
    check(s["apps"][0]["id"].string() == "third", "a control write reads the list again");
}

// ---- reuse and invalidation -------------------------------------------------

void collectionIsReusedFor400ms() {
    Tv tv;
    seedLiveReads(tv);
    const Json first = tv.collect();
    const int calls = tv.count(kPower);
    tv.now += 399;
    const Json again = tv.collect();
    check(tv.count(kPower) == calls && canon(first) == canon(again), "within 0.4 s the last read is returned");
    tv.now += 1;
    tv.collect();
    check(tv.count(kPower) == calls + 1, "after 0.4 s it is made again");
}

void clockSteppedBackDoesNotServeTheLastStats() {
    Tv tv;
    tv.collect(Tv::system(1000));
    tv.now -= 600000;
    check(tv.collect(Tv::system(99999))["uptime"].number() == 99999, "a clock moved back is not a hit");
}

void bootTimeHoldsWithinThirtySeconds() {
    Tv tv;
    const std::string first = tv.collect(Tv::system(1000))["bootTime"].string();
    check(tv.later(10000, Tv::system(1010))["bootTime"].string() == first, "uptime and clock agree: same boot");
    check(tv.later(10000, Tv::system(5000))["bootTime"].string() != first, "a clock jump moves it");
}

void expireMakesTheNextReadFreshButKeepsCachedReads() {
    Tv tv;
    seedLiveReads(tv);
    tv.collect();
    tv.collector->expire();
    tv.now += 100;
    tv.collect();
    check(tv.count(kPower) == 2, "power asked again");
    check(tv.count(kSound) == 1, "the cached audio read is kept");
}

void forgetDropsOnlyMatchingReads() {
    Tv tv;
    seedLiveReads(tv);
    tv.collect();
    tv.collector->forget({"com.webos.audio/"});
    tv.now += 100;
    tv.collect();
    check(tv.count(kSound) == 2, "the matching read is asked again");
    check(tv.count(kVolume) == 1 && tv.count(kApp) == 1, "the others are kept");
    tv.collector->forget({});
    tv.now += 100;
    tv.collect();
    check(tv.count(kSound) == 3 && tv.count(kVolume) == 2 && tv.count(kApp) == 2, "no match list drops every read");
}

void settingsReadsAreForgottenByCategory() {
    Tv tv;
    tv.settings("picture", "{\"returnValue\":true,\"settings\":{\"pictureMode\":\"cinema\",\"backlight\":\"40\"}}");
    tv.collect();
    const std::string picture = std::string(kSettings) + "#picture";
    const int before = tv.count(picture);
    tv.collector->forget({"\"category\":\"sound\""});
    tv.now += 100;
    tv.collect();
    check(tv.count(picture) == before, "a sound write leaves the picture read");
    tv.collector->forget({"\"category\":\"picture\""});
    tv.now += 100;
    tv.collect();
    check(tv.count(picture) == before + 1, "a picture write drops it");
}

void invalidationDuringARunIsNotKept() {
    Tv tv;
    seedLiveReads(tv);
    bool fired = false;
    tv.onAsk = [&](const std::string &name) {
        if (name == kPower && !fired) {
            fired = true;
            tv.collector->expire();  // a live event lands while the read is made
        }
    };
    tv.collect();
    tv.now += 100;
    tv.collect();
    check(tv.count(kPower) == 2, "what was read across an event does not answer the next request");
    tv.now += 100;
    tv.collect();
    check(tv.count(kPower) == 2, "and the read after it is kept as usual");
}

void invalidationsNeverWaitOnARunningCollection() {
    Tv tv;
    std::mutex gate;
    std::condition_variable opened;
    bool open = false;
    std::atomic<bool> inside(false);
    tv.onAsk = [&](const std::string &name) {
        if (name != kPower) return;
        inside = true;
        std::unique_lock<std::mutex> lock(gate);
        opened.wait(lock, [&]() { return open; });
    };
    std::thread reader([&]() { tv.collect(); });
    for (int i = 0; i < 400 && !inside; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    check(inside, "the collection started");
    const auto begun = std::chrono::steady_clock::now();
    tv.collector->expire();
    tv.collector->clearCache();
    tv.collector->forget({"x"});
    tv.collector->noteVolumeRefused();
    const auto took = std::chrono::steady_clock::now() - begun;
    {
        std::lock_guard<std::mutex> lock(gate);
        open = true;
    }
    opened.notify_all();
    reader.join();
    check(took < std::chrono::milliseconds(500), "a control write or live event must not wait on a read in progress");
}

// ---- early read -------------------------------------------------------------

void earlyReadHasTheLocalPartMarkedLoading() {
    Tv tv;
    std::mutex gate;
    std::condition_variable opened;
    bool open = false;
    std::atomic<int> entered(0);
    // The receiver map is read after the local part is complete and before
    // any power or sound read: holding it there stands for a TV still answering.
    StatsDeps held;
    held.luna = [](const std::string &, const Json &, Json &) { return false; };
    held.config = []() { return Json::parse("{}"); };
    held.nowMs = [&tv]() { return tv.now; };
    held.isoTime = [](long long ms) { return std::to_string(ms); };
    held.hdmiReceiverMap = [&]() {
        ++entered;
        std::unique_lock<std::mutex> lock(gate);
        opened.wait(lock, [&]() { return open; });
        return Json::object();
    };
    StatsCollector collector(held);
    const Json sys = Tv::system();

    Json early;
    for (int i = 0; i < 400 && !early.valid(); ++i) {
        early = collector.early(sys);
        if (!early.valid()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(early.valid(), "an early read is given while none has completed");
    check(early["loading"].boolean() && early["ok"].boolean(), "marked loading");
    check(early.has("device") && early.has("mem") && early["uptime"].isNumber(), "what the TV itself knows is in it");
    check(!early.has("powerState"), "what the TV's services say is not");
    const int started = entered;
    check(collector.early(sys)["loading"].boolean(), "asking again is answered the same way");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    check(entered == started && started == 1, "and starts no second read");

    {
        std::lock_guard<std::mutex> lock(gate);
        open = true;
    }
    opened.notify_all();
    const Json full = collector.collect(sys);
    check(!full.has("loading"), "the full read is not marked loading");
    check(full.has("powerState"), "and carries what the services say");
    check(!collector.collect(sys).has("loading"), "nor is the one reused after it");
    check(!collector.early(sys).valid(), "after the first full read there is no early read");
}

void failedSnapshotIsReturnedAndNeverCollected() {
    Tv tv;
    const Json failure = Json::parse("{\"ok\":false,\"error\":\"native worker unavailable\"}");
    check(canon(tv.collect(failure)) == canon(failure), "worker failures propagate");
    check(tv.count("com.webos.service.tv.systemproperty/getSystemProperties") == 0, "and nothing is asked of the TV");
    check(!tv.collector->early(failure).valid(), "there is no local part to show");
    const Json ok = tv.collect();
    check(ok["ok"].boolean() && ok.has("device"), "the next good snapshot is collected");
}

void deviceDefaultsAndDetectionRetries() {
    Tv tv;
    const Json s = tv.collect();
    check(s["device"]["model"].string() == "webOS TV" && s["device"]["name"].string() == "LG webOS TV", "defaults");
    check(tv.count("com.webos.service.tv.systemproperty/getSystemProperties") == 4,
          "a lost answer at start is asked three times, and the logo light once");
    check(s["tvwebVersion"].string() == "1.2.3", "the version");
    check(!s["capabilities"]["oled"].boolean(), "not OLED without a detector");
}

void deviceModelFromTheTv() {
    Tv tv;
    tv.say("com.webos.service.tv.systemproperty/getSystemProperties",
           "{\"returnValue\":true,\"modelName\":\"OLED65C2PUA\",\"firmwareVersion\":\"03.31.50\",\"sdkVersion\":\"9.2.0\"}");
    const Json s = tv.collect();
    check(s["device"]["model"].string() == "OLED65C2PUA" && s["device"]["name"].string() == "LG OLED65C2PUA",
          "the TV's own name for the model fills in the placeholder");
    check(s["system"]["firmware"].string() == "03.31.50", "firmware");
    tv.config = Json::parse("{\"device\":{\"model\":\"My TV\",\"name\":\"Living room\"}}");
    Tv owned;
    owned.config = tv.config;
    owned.say("com.webos.service.tv.systemproperty/getSystemProperties",
              "{\"returnValue\":true,\"modelName\":\"OLED65C2PUA\"}");
    const Json o = owned.collect();
    check(o["device"]["model"].string() == "My TV" && o["device"]["name"].string() == "Living room",
          "what the owner configured is kept");
}
} // namespace

// ---- what Home Assistant's discovery takes from the read -----------------------

void discoveryInputsFollowTheStatsRead() {
    const char *kValues = "com.webos.service.settings/getSystemSettingValues";
    Tv tv;
    check(!tv.collector->discoveryInputs().valid(), "nothing is offered before a collection");
    tv.say("com.webos.service.tv.systemproperty/getSystemProperties",
           "{\"returnValue\":true,\"modelName\":\"OLED65B3PUA\",\"firmwareVersion\":\"33.31.75\","
           "\"sdkVersion\":\"10.3.2\"}");
    tv.say(std::string(kValues) + "#picture",
           "{\"returnValue\":true,\"values\":{\"arrayExt\":["
           "{\"value\":\"cinema\",\"visible\":true,\"active\":true},"
           "{\"value\":\"game\",\"visible\":true,\"active\":true},"
           "{\"value\":\"expert1\",\"visible\":true,\"active\":false},"
           "{\"value\":\"vivid\",\"visible\":false,\"active\":true}]}}");
    tv.say(std::string(kValues) + "#sound",
           "{\"returnValue\":true,\"values\":{\"arrayExt\":["
           "{\"value\":\"tv_speaker\",\"visible\":true,\"active\":true},"
           "{\"value\":\"headphone\",\"visible\":true,\"active\":true}]}}");
    tv.say("com.webos.service.config/getConfigs",
           "{\"returnValue\":true,\"configs\":{\"tv.model.supportHeadPhone\":true}}");
    tv.collect();
    Json inputs = tv.collector->discoveryInputs();
    check(inputs.isObject(), "offered once a collection has run");
    // The placeholders ("webOS TV", "webOS (tvweb)") a discovery built from config.json
    // alone published on a B3.
    check(inputs["device"]["model"].string() == "OLED65B3PUA", "the model the TV reports");
    check(inputs["device"]["sw_version"].string() == "33.31.75", "the firmware the TV reports");
    expectJson(inputs["pictureModes"],
               "[{\"value\":\"cinema\",\"label\":\"Cinema\"},{\"value\":\"game\",\"label\":\"Game\"}]",
               "the modes the TV offers now: visible and active ones");
    bool tvSpeaker = false, headphone = false;
    for (const Json &id : inputs["soundOutputs"].items()) {
        tvSpeaker = tvSpeaker || id.string() == "tv_speaker";
        headphone = headphone || id.string() == "headphone";
    }
    check(tvSpeaker && headphone, "the outputs the TV and its model offer, as ids");

    // A different picture on screen offers other modes; the next read reports them,
    // which is what makes the discovery republish.
    tv.replies[std::string(kValues) + "#picture"].clear();
    tv.say(std::string(kValues) + "#picture",
           "{\"returnValue\":true,\"values\":{\"arrayExt\":["
           "{\"value\":\"expert1\",\"visible\":true,\"active\":true},"
           "{\"value\":\"cinema\",\"visible\":true,\"active\":true}]}}");
    tv.later();
    inputs = tv.collector->discoveryInputs();
    check(inputs["pictureModes"].size() == 2 && inputs["pictureModes"][0]["value"].string() == "expert1",
          "changed modes are reported on the next read");
    // A read that finds none keeps the last ones, as the server does.
    tv.replies[std::string(kValues) + "#picture"].clear();
    tv.later();
    check(tv.collector->discoveryInputs()["pictureModes"].size() == 2, "an empty answer keeps the last modes");
    // The caller's copy is its own.
    inputs.set("pictureModes", Json::array());
    check(tv.collector->discoveryInputs()["pictureModes"].size() == 2, "a copy, not the collector's state");
}

int main() {
    struct Named {
        const char *name;
        void (*run)();
    } cases[] = {
        {"always-on: a leftover value is not offered", alwaysOnNotOfferedWithoutDescription},
        {"always-on: offered where the TV describes it", alwaysOnOfferedWhereDescribed},
        {"always-on: a lost answer offers nothing", alwaysOnLostAnswerIsNotRemembered},
        {"input keys: by port under an unusual app id", inputKeysByPortUnderUnusualAppId},
        {"live reads: short lives without a subscription", liveReadsKeepShortLivesWithoutSubscription},
        {"live reads: kept while delivering", liveReadsAreKeptWhileDelivering},
        {"live reads: polled again per group", liveReadsFallBackPerGroup},
        {"power: reason and times under powerState", powerStateCarriesReasonAndTimes},
        {"power: no reason is null", powerWithoutReasonIsNull},
        {"power: no answer is not off", noPowerAnswerIsNotOff},
        {"power: processing stands in for state", powerFallsBackToProcessing},
        {"power: Always Ready is its own state", alwaysReadyDisplayIsItsOwnPowerState},
        {"power: screen saver from state or marker", screenSaverFromPowerStateOrMarker},
        {"play state: remembered, then idle", playStateRemembersAndReadsIdle},
        {"play state: stays after a restart", playStateStaysAfterRestart},
        {"play state: com.webos.media without acb", playStateFromMediaServiceWithoutAcb},
        {"play state: a live source suppresses reads", livePlayStateSuppressesReads},
        {"hostname: only with an address, never waited on", hostnameIsAskedOnlyWithAnAddress},
        {"sound: no level leaves the key out", soundVolumeKeyFollowsTheReply},
        {"sound: an eARC soundbar's level", earcSoundbarLevelWinsOverZero},
        {"sound: optical has no volume", volumeNullWhereThereIsNoLevelControl},
        {"sound: the output is named from the volume service", audioOutputNamedFromTheVolumeService},
        {"sound: a refused volume holds for its route", refusedVolumeHoldsUntilTheSoundGoesElsewhere},
        {"sound: a refusal is not served from reuse", refusedVolumeIsAppliedToTheNextReadAlreadyInReuse},
        {"picture: shapes and Gallery", pictureShapes},
        {"hdmi: the signal follows the input on screen", hdmiSignalFollowsTheInputOnScreen},
        {"hdmi: links and sources", hdmiLinksAndSources},
        {"hdmi: screen off clears the signal", screenOffClearsTheSignal},
        {"apps: from launch points", installedAppsFromLaunchPoints},
        {"apps: listLaunchPoints fallback", installedAppsFallBackToLaunchPoints},
        {"apps: rescan rules", installedAppsRescanRules},
        {"reuse: 0.4 s", collectionIsReusedFor400ms},
        {"reuse: a clock stepped back", clockSteppedBackDoesNotServeTheLastStats},
        {"boot time: steady within 30 s", bootTimeHoldsWithinThirtySeconds},
        {"invalidate: expire", expireMakesTheNextReadFreshButKeepsCachedReads},
        {"invalidate: forget", forgetDropsOnlyMatchingReads},
        {"invalidate: forget by settings category", settingsReadsAreForgottenByCategory},
        {"invalidate: during a run is not kept", invalidationDuringARunIsNotKept},
        {"invalidate: never waits on a run", invalidationsNeverWaitOnARunningCollection},
        {"early: the local part, marked loading", earlyReadHasTheLocalPartMarkedLoading},
        {"early: a failed snapshot", failedSnapshotIsReturnedAndNeverCollected},
        {"device: defaults and retries", deviceDefaultsAndDetectionRetries},
        {"device: the TV's model", deviceModelFromTheTv},
        {"discovery inputs: device, picture modes, sound outputs", discoveryInputsFollowTheStatsRead},
    };
    int failed = 0;
    for (const Named &c : cases) {
        try {
            c.run();
            std::printf("ok   %s\n", c.name);
        } catch (const std::exception &error) {
            ++failed;
            std::printf("FAIL %s: %s\n", c.name, error.what());
        }
    }
    return failed ? 1 : 0;
}
