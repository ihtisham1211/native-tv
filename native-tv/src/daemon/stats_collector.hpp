#pragma once

#include "core/json.hpp"
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace nativetv {
namespace daemon {

// Everything the collector reads or asks for. Nothing here is the collector's
// own: the daemon passes its modules in, and a test passes scripted ones.
struct StatsDeps {
    // One Luna call; true when the bus answered (the reply may still say
    // returnValue false).
    std::function<bool(const std::string &uri, const Json &payload, Json &reply)> luna;
    // Whether a live subscription group is connected and delivering.
    std::function<bool(const std::string &group)> liveGroup;
    // config.json as it is now.
    std::function<Json()> config;
    std::string systemRoot = "/";
    std::string version;
    std::function<std::string()> address4;
    std::function<std::string(const std::string &address)> hostname;
    // The paired remote's live battery state, or an invalid Json without one.
    std::function<Json()> remoteBattery;
    // The HDMI receiver behind each input: {"1": 3, "2": 2, ...}.
    std::function<Json()> hdmiReceiverMap;
    // The player state from the live subscription, when it delivers; invalid
    // Json when it does not (the collector then asks the TV).
    std::function<Json()> liveMedia;
    std::function<Json()> adBlock;      // {enabled, count}
    std::function<Json()> powerOn;      // {onReason, onTime}
    std::function<std::string()> screensaverMode;
    std::function<std::string()> screensaverLevel;
    std::function<bool()> screensaverLaunched;
    // The frame rate of the HDMI source on screen, or invalid Json.
    std::function<Json(const std::string &appId)> sourceFrameRate;
    // Whether the panel is OLED, settled once known (false while undecided).
    std::function<bool()> isOled;
    // The OLED panel's wear figures; only asked on an OLED.
    std::function<Json(const Json &pictureSettings, const Json &powerState)> oledStats;
    std::function<void(const Json &diagnostics)> noteHdmiSeen;
    std::function<void()> noteMediaSeen;
    std::function<bool()> hasMediaState;
    std::function<long long()> nowMs;
    std::function<std::string(long long ms)> isoTime;
};

// /api/stats: the dashboard's whole picture of the TV, from the native system
// snapshot and the TV's services (legacy lib/telemetry.js collectStats).
class StatsCollector {
  public:
    explicit StatsCollector(StatsDeps deps);
    ~StatsCollector();

    // For the first dashboard load after start (?fast=1): starts the full read
    // if none is running and returns what it has so far, marked loading; an
    // invalid Json once a full read has been made, or when nothing is known yet.
    Json early(const Json &system);

    // One collection from the supplied system snapshot. Blocking; a collection
    // within 1.5 s of the last returns it.
    Json collect(const Json &system);
    // A live event or a control write: the next collection reads afresh.
    void expire();
    // A write that changes more than one read: the reads it bears on are dropped.
    void clearCache();
    // Drops the cached Luna reads whose key holds any of the given strings.
    void forget(const std::vector<std::string> &substrings);
    // The TV refused a volume or mute change on the route it is on.
    void noteVolumeRefused();

    // What the collector has learnt of the TV, for the capabilities the Home
    // Assistant entities depend on.
    Json hardware() const;
    // The TV's id and name as the settings page shows them: detected once a
    // collection has run, from config.json before that.
    // Detects it on the first ask when no collection has, unless one is running.
    Json deviceIdentity();
    // The model as detected (or configured), for names built from it; "" until known.
    std::string deviceModel();
    // Every installed app's title by id, as the last collection read them.
    Json appTitles();
    // What Home Assistant's discovery takes from the stats read, as of the last
    // collection: {device:{name,model,sw_version}, pictureModes, soundOutputs}
    // (output ids), or an invalid Json before one has run. Copies.
    Json discoveryInputs() const;

  private:
    struct Cached {
        long long at = 0;
        Json reply;
    };
    struct Pending {
        bool expire = false;
        bool clear = false;
        bool forgetAll = false;
        bool volumeRefused = false;
        std::vector<std::string> forget;
    };
    void applyPending();
    bool cached(const std::string &uri, const Json &payload, long long ttlMs, Json &reply);
    long long liveTtl(const std::string &group, long long pollMs, long long liveMs);
    bool media(Json &out);
    void detectDevice();
    void detectHardware(const std::string &sdkVersion);
    bool alwaysOnSupported();
    Json remoteInfo(const Json &system);
    void refreshInputNames();
    Json refreshInstalledApps();
    Json pictureModes();
    void soundOutputs(const std::string &current, Json &out);
    bool probeLightSensor(Json &out);

    StatsDeps deps_;
    // mutex_ is held for a whole collection; pendingMutex_ only ever briefly.
    std::mutex mutex_;
    std::mutex pendingMutex_;
    Pending pending_;
    std::map<std::string, Cached> cache_;
    Json lastStats_;
    long long lastStatsAt_ = 0;
    // Whether a collection has finished since the daemon started.
    std::atomic<bool> collected_{false};
    std::mutex partialMutex_;
    Json partial_;
    mutable std::mutex deviceMutex_;
    Json deviceShared_;
    Json titlesShared_ = Json::object();
    Json discoveryShared_;
    std::atomic<bool> earlyRunning_{false};
    std::thread earlyThread_;
    std::deque<double> tempHistory_;
    long long bootEpoch_ = 0;
    Json inputNames_ = Json::object();
    Json inputAliases_ = Json::object();
    long long lastInputScan_ = 0;
    Json installedApps_ = Json::array();
    Json appTitles_ = Json::object();
    long long lastAppsScan_ = 0;
    Json lastPictureModes_ = Json::array();
    Json lastRemoteFacts_;
    Json lastSoundInput_;
    Json lastSoundOutputs_ = Json::array();
    bool btAudioKnown_ = false;
    bool btAudio_ = false;
    int masterVolumeSupported_ = -1;
    int soundFormatSupported_ = -1;
    std::string lastSoundRoute_;
    bool haveVolumeRefusedRoute_ = false;
    std::string volumeRefusedRoute_;
    bool hasLightSensor_ = false;
    int lightSensorFailures_ = 0;
    long long lastLightSensorProbe_ = 0;
    int hasLogoLight_ = -1;
    int alwaysOnSupport_ = -1;
    bool alwaysOnAsked_ = false;
    bool alwaysReadyScreenOn_ = false;
    Json powerTimersSeen_;
    bool deviceDetected_ = false;
    Json device_ = Json::object();
    Json hardware_ = Json::object();
};
} // namespace daemon
} // namespace nativetv
