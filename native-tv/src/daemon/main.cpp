#include "daemon/child_process.hpp"
#include "daemon/adblock_manager.hpp"
#include "daemon/app_tile_manager.hpp"
#include "daemon/apps_state.hpp"
#include "daemon/config.hpp"
#include "daemon/daily_backup_scheduler.hpp"
#include "daemon/devtools_firewall.hpp"
#include "daemon/home_probe_gate.hpp"
#include "daemon/home_screen_manager.hpp"
#include "daemon/home_boot_manager.hpp"
#include "daemon/home_orchestrator.hpp"
#include "daemon/home_restore_manager.hpp"
#include "daemon/home_takeover_manager.hpp"
#include "daemon/home_takeover_lifecycle.hpp"
#include "daemon/screensaver_control.hpp"
#include "daemon/screensaver_manager.hpp"
#include "daemon/launcher_sync.hpp"
#include "daemon/retired_files.hpp"
#include "daemon/services_keeper.hpp"
#include "daemon/tv_app_manager.hpp"
#include "daemon/event_loop.hpp"
#include "daemon/game_frame_rate.hpp"
#include "daemon/hdmi_seen.hpp"
#include "daemon/ha_discovery.hpp"
#include "daemon/heartbeat.hpp"
#include "daemon/http_frontdoor.hpp"
#include "daemon/http_server.hpp"
#include "daemon/logger.hpp"
#include "daemon/luna.hpp"
#include "daemon/json_file.hpp"
#include "daemon/mqtt_availability.hpp"
#include "daemon/mqtt_client.hpp"
#include "daemon/mqtt_command_router.hpp"
#include "daemon/ntp_client.hpp"
#include "daemon/paths.hpp"
#include "daemon/mqtt_state_publisher.hpp"
#include "daemon/mqtt_telemetry_publisher.hpp"
#include "daemon/mqtt_topics.hpp"
#include "daemon/remote_battery.hpp"
#include "daemon/remote_buttons.hpp"
#include "daemon/remote_button_log_watcher.hpp"
#include "daemon/remote_keys_reconciler.hpp"
#include "daemon/sam_rescan_manager.hpp"
#include "daemon/screen_time_enforcer.hpp"
#include "daemon/state_node_bridge.hpp"
#include "daemon/state_subscriptions.hpp"
#include "daemon/syslog_forwarder.hpp"
#include "domain/names.hpp"
#include "platform/client_config.hpp"
#include "service/piccap.hpp"
#include "service/adblock_store.hpp"
#include "service/history_recorder.hpp"
#include "service/oled_care.hpp"
#include "service/remote_keys.hpp"
#include "service/sound_outputs.hpp"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <csignal>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <condition_variable>
#include <deque>
#include <exception>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <unistd.h>

namespace {
using nativetv::Json;
using namespace nativetv::daemon;

volatile std::sig_atomic_t shutdownRequested = 0;

void requestShutdown(int) {
    shutdownRequested = 1;
}

bool installSignalHandlers() {
    struct sigaction action;
    action.sa_handler = requestShutdown;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    return sigaction(SIGTERM, &action, nullptr) == 0 && sigaction(SIGINT, &action, nullptr) == 0;
}

void usage(std::ostream &output) {
    output << "Usage: nativetv-daemon [--config PATH] [--heartbeat PATH] [--workers COUNT]\n"
           << "                         [--listen] [--assets-root PATH]\n"
           << "                         [--node-port PORT: forward to a Node upstream, tests only]\n"
           << "                         [--proc-root PATH] [--system-root PATH]\n"
           << "                         [--cpu-window-ms MS] [--restore-adblock]\n"
           << "                         [--release-home | --takeover-home | "
              "--restore-home-screen | --reconcile-screensaver | "
              "--reconcile-app-tiles | --open-home-at-boot | "
              "--show-home-starting-notice | "
              "--restore-nativetv-home]\n"
           << "                         [--restore-remote-keys] [--blank-home-layout]\n"
           << "       nativetv-daemon --help\n";
}

size_t parseWorkers(const std::string &value) {
    char *end = nullptr;
    const unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
    if (value.empty() || !end || *end || parsed < 1 || parsed > 64)
        throw std::runtime_error("--workers must be between 1 and 64");
    return static_cast<size_t>(parsed);
}

unsigned short parsePort(const std::string &value) {
    char *end = nullptr;
    const unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
    if (value.empty() || !end || *end || parsed < 1 || parsed > 65535)
        throw std::runtime_error("--node-port must be between 1 and 65535");
    return static_cast<unsigned short>(parsed);
}

unsigned parseCpuWindow(const std::string &value) {
    char *end = nullptr;
    const unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
    if (value.empty() || !end || *end || parsed < 1 || parsed > 60000)
        throw std::runtime_error("--cpu-window-ms must be between 1 and 60000");
    return static_cast<unsigned>(parsed);
}

std::string configuredLogLevel(const nativetv::Json &document) {
    const nativetv::Json logging = document["logging"];
    const nativetv::Json level = logging["level"];
    return logging.isObject() && level.isString() ? level.string() : "info";
}

bool environmentEnabled(const char *name) {
    const char *value = std::getenv(name);
    return value && std::string(value) == "1";
}

// A start with no Node (the default for --listen without --node-port): the daemon is the whole
// server, so it sets the switches native-frontdoor.js would export and the opt-in
// modules Node would otherwise run, unless the environment already says. Node
// is also what shares the proxy secret, which the in-process internal routes
// still check: with no Node there is nobody to share it with, so it is made here.
void applyNodeFreeEnvironment() {
    static const char *const kOn[] = {
        "TVWEB_NATIVE_STATE", "TVWEB_NATIVE_REMOTE_INPUT", "TVWEB_NATIVE_SYSLOG",
        "TVWEB_NATIVE_NTP", "TVWEB_NATIVE_HISTORY", "TVWEB_NATIVE_SCREENTIME",
        "TVWEB_NATIVE_STATS", "TVWEB_NATIVE_INSTALL", "TVWEB_NATIVE_SERVICES",
        "TVWEB_NATIVE_HOME_ORCH", "TVWEB_NATIVE_MQTT_PRODUCTION"};
    for (const char *name : kOn)
        setenv(name, "1", 0);
    const char *secret = std::getenv("TVWEB_PROXY_SECRET");
    if (secret && std::strlen(secret) >= 32)
        return;
    unsigned char bytes[32];
    const int device = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    ssize_t got = device < 0 ? -1 : read(device, bytes, sizeof(bytes));
    if (device >= 0)
        close(device);
    if (got != static_cast<ssize_t>(sizeof(bytes)))
        throw std::runtime_error("could not read /dev/urandom for the internal secret");
    static const char kHex[] = "0123456789abcdef";
    char text[sizeof(bytes) * 2 + 1];
    for (size_t index = 0; index < sizeof(bytes); ++index) {
        text[index * 2] = kHex[bytes[index] >> 4];
        text[index * 2 + 1] = kHex[bytes[index] & 15];
    }
    text[sizeof(text) - 1] = '\0';
    setenv("TVWEB_PROXY_SECRET", text, 1);
}

bool mqttConfigured(const nativetv::Json &document) {
    const nativetv::Json mqtt = document["mqtt"];
    return mqtt["enabled"].boolean() && mqtt["host"].isString() &&
           !mqtt["host"].string().empty();
}

nativetv::Json restoreHomeScreen(HomeScreenManager &homeScreen,
                                   CommandPolicyExecutor &policy,
                                   const CommandPolicyContext &context) {
    const Json tracked = homeScreen.state()["addedInputs"];
    for (const std::string &id : tracked.keys()) {
        Json payload = Json::object();
        payload.set("launchPointId", tracked[id]);
        struct Completion {
            std::mutex mutex;
            std::condition_variable condition;
            bool done = false;
        };
        const std::shared_ptr<Completion> completion = std::make_shared<Completion>();
        CommandPolicySpec command;
        command.action = "home-screen.luna.restore-input";
        command.uri = "com.webos.applicationManager/removeLaunchPoint";
        command.payload = payload;
        command.timeout = std::chrono::milliseconds(5000);
        command.requiresControl = false;
        command.resource = "home-screen-launch-points";
        const CommandDecision decision = policy.submit(
            command, context, false, [completion](const LunaReply &reply) {
                std::lock_guard<std::mutex> lock(completion->mutex);
                (void)reply;
                completion->done = true;
                completion->condition.notify_one();
            });
        if (decision.kind != CommandDecisionKind::Queued)
            continue;
        std::unique_lock<std::mutex> lock(completion->mutex);
        (void)completion->condition.wait_for(
            lock, std::chrono::milliseconds(6000),
            [completion]() { return completion->done; });
    }

    const Json cleared = homeScreen.setAddedInputs(Json::object());
    if (!cleared["ok"].isBoolean() || !cleared["ok"].boolean())
        return cleared;
    return homeScreen.restore();
}

bool waitForPolicyLuna(CommandPolicyExecutor &policy,
                       const CommandPolicyContext &context,
                       const std::string &action, const std::string &uri,
                       const Json &payload, const std::string &resource,
                       std::chrono::milliseconds timeout, LunaReply &reply) {
    struct Completion {
        std::mutex mutex;
        std::condition_variable condition;
        bool done = false;
        LunaReply reply;
    };
    const std::shared_ptr<Completion> completion = std::make_shared<Completion>();
    CommandPolicySpec command;
    command.action = action;
    command.uri = uri;
    command.payload = payload;
    command.timeout = timeout;
    command.requiresControl = false;
    command.resource = resource;
    const CommandDecision decision = policy.submit(
        command, context, false, [completion](const LunaReply &value) {
            std::lock_guard<std::mutex> lock(completion->mutex);
            completion->reply = value;
            completion->done = true;
            completion->condition.notify_one();
        });
    if (decision.kind != CommandDecisionKind::Queued) {
        reply = LunaReply{};
        reply.error = decision.reason;
        return false;
    }
    std::unique_lock<std::mutex> lock(completion->mutex);
    if (!completion->condition.wait_for(
            lock, timeout + std::chrono::milliseconds(1000),
            [completion]() { return completion->done; })) {
        reply = LunaReply{};
        reply.error = "command policy completion timed out";
        return false;
    }
    reply = completion->reply;
    return !reply.timedOut && reply.error.empty() && reply.response.isObject();
}

bool restoreJavaScriptRemoteKeys(CommandPolicyExecutor &policy,
                                 const CommandPolicyContext &context,
                                 const std::string &systemRoot,
                                 std::string &error) {
    error.clear();
    std::string dataRoot = dataDirectory();
    while (dataRoot.size() > 1 && dataRoot.back() == '/')
        dataRoot.pop_back();
    const std::string mapPath = dataRoot + "/remotekeys.json";
    const std::string lgPath = dataRoot + "/remotekeys-lg.json";
    const std::string filterPath = dataRoot + "/remotekeys-filter.js";
    const auto exists = [](const std::string &path) {
        return access(path.c_str(), F_OK) == 0;
    };
    if (!exists(mapPath) && !exists(filterPath) && !exists(lgPath))
        return true;
    if (!nativetv::service::hasJavaScriptRemoteKeys(systemRoot) || exists(lgPath)) {
        error = "this remote-key restore still requires the compatibility handler";
        return false;
    }

    if (unlink(mapPath.c_str()) != 0 && errno != ENOENT) {
        error = "could not remove the saved remote-key map";
        return false;
    }

    Json mapValue = Json::object();
    mapValue.set("keys", Json::object());
    Json configs = Json::object();
    configs.set("nativetv.remoteKeys", mapValue);
    Json setPayload = Json::object();
    setPayload.set("configs", configs);
    LunaReply setMapReply;
    if (!waitForPolicyLuna(policy, context, "remoteKeys.restore-map",
            "com.webos.service.config/setConfigs", setPayload, "remote-keys",
            std::chrono::milliseconds(5000), setMapReply) ||
        !setMapReply.response["returnValue"].boolean()) {
        error = setMapReply.error.empty()
            ? "configd did not clear the remote-key map" : setMapReply.error;
        return false;
    }

    Json getPayload = Json::object();
    Json configNames = Json::array();
    configNames.add(Json::text("com.webos.surfacemanager.keyFilters"));
    getPayload.set("configNames", configNames);
    LunaReply chainReply;
    if (!waitForPolicyLuna(policy, context, "remoteKeys.restore-read-chain",
            "com.webos.service.config/getConfigs", getPayload, "remote-keys",
            std::chrono::milliseconds(5000), chainReply) ||
        !chainReply.response["returnValue"].boolean() ||
        !chainReply.response["configs"]["com.webos.surfacemanager.keyFilters"].isArray()) {
        error = chainReply.error.empty()
            ? "configd did not return the remote-key filter chain" : chainReply.error;
        return false;
    }

    Json rest = Json::array();
    bool hadFilter = false;
    const Json chain = chainReply.response["configs"][
        "com.webos.surfacemanager.keyFilters"];
    for (const Json &entry : chain.items()) {
        if (entry["file"].isString() && entry["file"].string() == filterPath) {
            hadFilter = true;
            continue;
        }
        rest.add(entry);
    }
    if (hadFilter) {
        Json chainConfigs = Json::object();
        chainConfigs.set("com.webos.surfacemanager.keyFilters", rest);
        Json chainPayload = Json::object();
        chainPayload.set("configs", chainConfigs);
        LunaReply chainWriteReply;
        if (!waitForPolicyLuna(policy, context,
                "remoteKeys.restore-chain",
                "com.webos.service.config/setConfigs", chainPayload,
                "remote-keys", std::chrono::milliseconds(5000),
                chainWriteReply) ||
            !chainWriteReply.response["returnValue"].boolean()) {
            error = chainWriteReply.error.empty()
                ? "configd did not remove the Native Tv remote-key filter"
                : chainWriteReply.error;
            return false;
        }
    }
    if (unlink(filterPath.c_str()) != 0 && errno != ENOENT) {
        error = "could not remove the Native Tv remote-key filter file";
        return false;
    }
    return true;
}

bool restoreNativeRemoteKeys(HttpFrontDoor &frontDoor,
                             const std::string &dataRoot,
                             std::string &error) {
    (void)dataRoot;  // the front door has it
    return frontDoor.restoreNativeRemoteKeys(error);
}

nativetv::daemon::MqttClientOptions mqttShadowOptions(
    const nativetv::Json &document, const std::string &topicPrefix,
    bool isolated) {
    using namespace nativetv;
    using namespace nativetv::daemon;
    const Json mqtt = document["mqtt"];
    const Json device = document["device"];
    MqttClientOptions options;
    options.host = mqtt["host"].string();
    options.tls = mqtt["tls"].boolean();
    options.port = options.tls ? 8883 : 1883;
    const Json configuredPort = mqtt["port"];
    if (configuredPort.isNumber() && configuredPort.number() >= 1 &&
        configuredPort.number() <= 65535 &&
        static_cast<unsigned short>(configuredPort.number()) == configuredPort.number())
        options.port = static_cast<unsigned short>(configuredPort.number());
    options.tlsRejectUnauthorized =
        !(mqtt["tlsRejectUnauthorized"].isBoolean() &&
          !mqtt["tlsRejectUnauthorized"].boolean());
    // The broker's own CA, where the server always read it: beside config.json.
    const std::string caPath = dataDirectory() + "/mqtt-ca.pem";
    const std::string earlierCaPath = dataDirectory() + "/server/mqtt-ca.pem";
    if (options.tls && access(caPath.c_str(), R_OK) == 0)
        options.caFile = caPath;
    else if (options.tls && access(earlierCaPath.c_str(), R_OK) == 0)
        options.caFile = earlierCaPath;
    options.username = mqtt["username"].string();
    options.password = mqtt["password"].string();
    std::string deviceId = device["id"].string();
    if (deviceId.empty()) deviceId = "lg_b8_tv";
    std::string clientId = mqtt["clientId"].string();
    if (clientId.empty()) clientId = deviceId + "_tvweb";
    options.clientId = clientId + (isolated ? "_native" : "");
    options.willTopic = MqttTopics(topicPrefix).status();
    options.willPayload = "offline";
    options.willRetain = true;
    return options;
}

nativetv::Json mqttShadowDiscoveryCapabilities(
    const nativetv::Json &userEntities, const nativetv::Json &system,
    const nativetv::Json &probes, const std::string &dataRoot,
    bool remoteButtons, bool watchHistory, bool isOled) {
    using namespace nativetv;
    Json capabilities = Json::object();
    const auto setProbe = [&capabilities, &probes](const char *name) {
        if (probes[name].isBoolean())
            capabilities.set(name, probes[name]);
    };
    capabilities.set("hasRemoteInfo",
        Json::flag(system["remote"]["paired"].boolean()));
    capabilities.set("hasPnwash", Json::flag(system["hasPnwash"].boolean()));
    capabilities.set("hasCell", Json::flag(probes["hasCell"].boolean()));
    capabilities.set("hasHdmiProc", Json::flag(system["hasHdmiProc"].boolean()));
    capabilities.set("hasMediaState", Json::flag(
        access((dataRoot + "/media_seen").c_str(), F_OK) == 0));
    setProbe("hasSoundFormat");
    capabilities.set("hasHdrStatus", Json::flag(system["hasHdrStatus"].boolean()));
    if (system["socArch"].isString() && !system["socArch"].string().empty())
        capabilities.set("socArch", system["socArch"]);
    setProbe("hasLogoLight");
    setProbe("hasOnTimer");
    setProbe("hasOffTimer");
    capabilities.set("thermalPresent", Json::flag(system["thermalPresent"].boolean()));
    capabilities.set("emmcWearPresent",
        Json::flag(system["emmcWearPresent"].boolean()));
    setProbe("hasLightSensor");
    capabilities.set("hasRemoteButtons", Json::flag(remoteButtons));
    capabilities.set("hasGpuClock", Json::flag(system["gpuHz"].isNumber()));
    capabilities.set("watchHistory", Json::flag(watchHistory));
    capabilities.set("isOled", Json::flag(isOled));
    capabilities.set("hdmiSeen",
        daemon::readHdmiSeen(dataRoot + "/hdmi_seen"));
    capabilities.set("userEntities", userEntities);
    return capabilities;
}

bool mqttShadowIsOled(const nativetv::Json &config,
                      const std::string &systemRoot,
                      const std::string &detectedModel = std::string()) {
    using namespace nativetv;
    Json forced = config["panel"];
    if (!forced.isString() || forced.string().empty())
        forced = config["device"]["panel"];
    if (forced.isString() && !forced.string().empty()) {
        std::string panel = forced.string();
        std::transform(panel.begin(), panel.end(), panel.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return panel.find("oled") != std::string::npos;
    }
    const std::string model = detectedModel.empty()
        ? config["device"]["model"].string() : detectedModel;
    return service::hasOledPanelTypeMarker(systemRoot) ||
           service::oledModel(model) || service::hasOledPanelRecords(systemRoot);
}

void mqttShadowObserveHdmiFacts(const nativetv::Json &system,
                                const std::string &dataRoot) {
    using namespace nativetv;
    Json observed = Json::object();
    for (const Json &port : system["hdmiPortFacts"].items()) {
        const Json link = port["link"];
        for (const auto &field : {std::pair<const char *, const char *>{"phy_mode", "phy_mode"},
                                  {"chroma", "chroma"}, {"hdcp", "hdcp"}}) {
            const Json value = link[field.first];
            if (value.valid())
                observed.set(field.second, value);
        }
        for (const char *field : {"allm", "vrr"})
            if (port[field].valid())
                observed.set(field, port[field]);
    }
    if (!observed.keys().empty())
        (void)daemon::recordHdmiSeen(dataRoot + "/hdmi_seen", observed);
}

class MqttCommandDispatcher {
  public:
    MqttCommandDispatcher(EventLoop &loop, HttpFrontDoor &frontDoor,
                          MqttTelemetryPublisher &telemetry, MqttTopics topics,
                          Logger &logger)
        : loop_(loop), frontDoor_(frontDoor), telemetry_(telemetry),
          topics_(std::move(topics)), logger_(logger) {
        worker_ = std::thread(&MqttCommandDispatcher::run, this);
    }

    ~MqttCommandDispatcher() {
        stop();
        join();
    }

    MqttCommandDispatcher(const MqttCommandDispatcher &) = delete;
    MqttCommandDispatcher &operator=(const MqttCommandDispatcher &) = delete;

    bool enqueue(const MqttMessage &message) {
        const std::string prefix = topics_.commandPrefix();
        if (message.topic.compare(0, prefix.size(), prefix) != 0 ||
            message.topic.size() - prefix.size() > 256 ||
            message.payload.size() > 16384)
            return false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_ || pending_.size() >= kQueueLimit)
                return false;
            pending_.push_back(message);
        }
        condition_.notify_one();
        return true;
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
            pending_.clear();
        }
        condition_.notify_all();
    }

    bool idle() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return pending_.empty() && !active_;
    }

    void join() {
        if (worker_.joinable())
            worker_.join();
    }

  private:
    static const size_t kQueueLimit = 64;

    void run() {
        for (;;) {
            MqttMessage message;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                condition_.wait(lock, [this]() { return stopping_ || !pending_.empty(); });
                if (pending_.empty() && stopping_)
                    return;
                message = std::move(pending_.front());
                pending_.pop_front();
                active_ = true;
            }

            try {
                const std::string prefix = topics_.commandPrefix();
                const std::string action = message.topic.substr(prefix.size());
                Json lgSettingsRows = Json::array();
                if (action.compare(0, 4, "lgs/") == 0)
                    lgSettingsRows = frontDoor_.collectMqttLgSettings()["rows"];
                const MqttControlCommand command = routeMqttControlTopic(
                    message.topic, prefix, message.payload, lgSettingsRows);
                if (command.valid) {
                    const Json response = frontDoor_.executeMqttControl(command.payload);
                    if (!response["ok"].boolean()) {
                        std::string error = response["error"].string();
                        if (error.empty())
                            error = "control failed";
                        logger_.warning("native MQTT command " + command.action +
                                        " failed: " + error);
                    }
                    if (command.refreshTelemetry) {
                        MqttTelemetryPublisher *telemetry = &telemetry_;
                        if (!loop_.scheduleAfter(EventLoop::Milliseconds(400),
                                [telemetry]() { telemetry->publishSnapshot(); }))
                            logger_.debug(
                                "native MQTT command: telemetry refresh was not scheduled");
                    }
                }
            } catch (const std::exception &error) {
                logger_.warning(std::string("native MQTT command failed: ") + error.what());
            } catch (...) {
                logger_.warning("native MQTT command failed with an unknown error");
            }

            {
                std::lock_guard<std::mutex> lock(mutex_);
                active_ = false;
            }
            condition_.notify_all();
        }
    }

    EventLoop &loop_;
    HttpFrontDoor &frontDoor_;
    MqttTelemetryPublisher &telemetry_;
    MqttTopics topics_;
    Logger &logger_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<MqttMessage> pending_;
    std::thread worker_;
    bool active_ = false;
    bool stopping_ = false;
};

struct MqttShadowDiscoveryRuntime {
    nativetv::daemon::HaDiscoveryOptions options;
    nativetv::Json probedCapabilities = nativetv::Json::object();
    nativetv::Json soundOutputValues;
    nativetv::Json soundOutputConfigs;
    std::string currentSoundOutput;
    std::string detectedModel;
    unsigned pendingInputs = 0;
    bool lgSettingsReady = false;
    bool capabilitiesReady = false;
    // The picture modes and sound outputs come from the stats read once it has run.
    bool collectorInputs = false;
    bool isOled = false;
    bool published = false;
    std::string publishedSignature;
};

nativetv::Json mqttDiscoveryInstalledApps(const nativetv::Json &response) {
    using namespace nativetv;
    Json raw = response["launchPoints"].isArray()
        ? response["launchPoints"] : response["apps"];
    Json apps = Json::array();
    std::set<std::string> seen;
    std::vector<Json> sorted;
    for (const Json &entry : raw.items()) {
        const std::string id = entry["id"].string();
        if (id.empty() || (entry["visible"].isBoolean() &&
                           !entry["visible"].boolean()) ||
            id.compare(0, std::string("com.webos.app.container").size(),
                       "com.webos.app.container") == 0 ||
            !seen.insert(id).second)
            continue;
        Json app = Json::object();
        app.set("id", Json::text(id));
        const std::string title = entry["title"].string();
        app.set("title", Json::text(title.empty() ? id : title));
        if (entry["iconColor"].isString())
            app.set("iconColor", entry["iconColor"]);
        sorted.push_back(std::move(app));
    }
    const auto lower = [](std::string value) {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return value;
    };
    std::stable_sort(sorted.begin(), sorted.end(), [&lower](const Json &left,
                                                             const Json &right) {
        return lower(left["title"].string()) < lower(right["title"].string());
    });
    for (const Json &app : sorted)
        apps.add(app);
    return apps;
}

nativetv::Json mqttDiscoveryPictureModes(const nativetv::Json &response) {
    using namespace nativetv;
    Json modes = Json::array();
    for (const Json &entry : response["values"]["arrayExt"].items()) {
        if (!entry["visible"].isBoolean() || !entry["visible"].boolean() ||
            (entry["active"].isBoolean() && !entry["active"].boolean()) ||
            !entry["value"].isString() || entry["value"].string().empty())
            continue;
        Json mode = Json::object();
        mode.set("value", entry["value"]);
        mode.set("label", names::pictureMode(entry["value"].string())["display"]);
        modes.add(mode);
    }
    return modes;
}

void publishMqttShadowDiscoveryIfReady(
    const std::shared_ptr<MqttShadowDiscoveryRuntime> &runtime,
    nativetv::daemon::MqttClient *client, nativetv::daemon::Logger *logger) {
    using namespace nativetv::daemon;
    using nativetv::Json;
    if (!runtime || !client || !client->connected())
        return;
    if (runtime->pendingInputs != 0 || !runtime->lgSettingsReady ||
        !runtime->capabilitiesReady) {
        if (logger)
            logger->debug("native MQTT: waiting for " +
                          std::to_string(runtime->pendingInputs) +
                          " discovery input reads and LG settings " +
                          (runtime->lgSettingsReady ? "ready" : "pending") +
                          " and capabilities " +
                          (runtime->capabilitiesReady ? "ready" : "pending"));
        return;
    }
    Json signature = Json::object();
    signature.set("topicPrefix", Json::text(runtime->options.topicPrefix));
    signature.set("discoveryPrefix", Json::text(runtime->options.discoveryPrefix));
    signature.set("deviceId", Json::text(runtime->options.deviceId));
    signature.set("device", runtime->options.device);
    signature.set("capabilities", runtime->options.capabilities);
    signature.set("installedApps", runtime->options.installedApps);
    signature.set("pictureModes", runtime->options.pictureModes);
    signature.set("soundOutputs", runtime->options.soundOutputs);
    signature.set("lgSettings", runtime->options.lgSettings);
    signature.set("allowPower", Json::flag(runtime->options.allowPower));
    signature.set("piccap", Json::flag(runtime->options.piccap));
    const std::string signatureText = signature.dump(false);
    if (runtime->published && runtime->publishedSignature == signatureText)
        return;
    const HaDiscoveryBatch batch = buildHaDiscoveryBatch(runtime->options);
    const bool accepted = publishHaDiscoveryBatch(batch,
        [client](const std::string &topic, const std::string &payload, bool retained) {
            return client->publish(topic, payload, retained);
        });
    runtime->published = true;
    runtime->publishedSignature = signatureText;
    if (logger && accepted)
        logger->info("native MQTT: queued " +
                     std::to_string(batch.publish.size()) +
                     " isolated Home Assistant configs");
    else if (logger)
        logger->warning("native MQTT: discovery batch was only partly queued");
}
} // namespace

int main(int argc, char **argv) {
    using namespace nativetv;
    using namespace nativetv::daemon;

    try {
        ConfigLoadOptions configOptions;
        std::string heartbeatPath = "/var/run/tvweb.beat";
        std::string assetsRoot = "/var/lib/tvweb/assets";
        std::string procRoot = "/proc";
        std::string systemRoot = "/";
        size_t workerCount = 8;
        unsigned short nodePort = 18081;
        bool nodePortGiven = false;
        unsigned cpuWindowMs = 5000;
        bool listen = false;
        bool restoreAdBlock = false;
        bool releaseHome = false;
        bool takeoverHome = false;
        bool restoreHome = false;
        bool restoreNativeTvHome = false;
        bool restoreRemoteKeys = false;
        bool reconcileScreensaver = false;
        bool reconcileAppTiles = false;
        bool openHomeAtBoot = false;
        bool showHomeStartingNotice = false;
        bool blankHomeLayout = false;
        for (int index = 1; index < argc; ++index) {
            const std::string argument = argv[index];
            if (argument == "--help" || argument == "-h") {
                usage(std::cout);
                return 0;
            }
            if ((argument == "--config" || argument == "--heartbeat" || argument == "--workers" ||
                 argument == "--node-port" || argument == "--assets-root" ||
                 argument == "--proc-root" || argument == "--system-root" ||
                 argument == "--cpu-window-ms") &&
                index + 1 >= argc)
                throw std::runtime_error(argument + " requires a value");
            if (argument == "--config")
                configOptions.overridePath = argv[++index];
            else if (argument == "--heartbeat")
                heartbeatPath = argv[++index];
            else if (argument == "--workers")
                workerCount = parseWorkers(argv[++index]);
            else if (argument == "--node-port") {
                nodePort = parsePort(argv[++index]);
                nodePortGiven = true;
            }
            else if (argument == "--assets-root")
                assetsRoot = argv[++index];
            else if (argument == "--proc-root")
                procRoot = argv[++index];
            else if (argument == "--system-root")
                systemRoot = argv[++index];
            else if (argument == "--cpu-window-ms")
                cpuWindowMs = parseCpuWindow(argv[++index]);
            else if (argument == "--listen")
                listen = true;
            else if (argument == "--restore-adblock")
                restoreAdBlock = true;
            else if (argument == "--release-home")
                releaseHome = true;
            else if (argument == "--takeover-home")
                takeoverHome = true;
            else if (argument == "--restore-home-screen")
                restoreHome = true;
            else if (argument == "--restore-nativetv-home")
                restoreNativeTvHome = true;
            else if (argument == "--restore-remote-keys")
                restoreRemoteKeys = true;
            else if (argument == "--reconcile-screensaver")
                reconcileScreensaver = true;
            else if (argument == "--reconcile-app-tiles")
                reconcileAppTiles = true;
            else if (argument == "--open-home-at-boot")
                openHomeAtBoot = true;
            else if (argument == "--show-home-starting-notice")
                showHomeStartingNotice = true;
            else if (argument == "--blank-home-layout")
                blankHomeLayout = true;
            else
                throw std::runtime_error("unknown argument: " + argument);
        }

        if (restoreAdBlock) {
            if (listen || releaseHome || takeoverHome || restoreHome ||
                restoreNativeTvHome ||
                restoreRemoteKeys || reconcileScreensaver || reconcileAppTiles ||
                openHomeAtBoot ||
                showHomeStartingNotice ||
                blankHomeLayout)
                throw std::runtime_error(
                    "--restore-adblock cannot be combined with another operation");
            service::AdBlockStore adBlock(dataDirectory(), systemRoot);
            std::string error;
            if (!adBlock.restoreAtBoot(true, &error))
                throw std::runtime_error("could not restore ad-block hosts table" +
                                         (error.empty() ? std::string() : ": " + error));
            return 0;
        }
        if ((releaseHome || takeoverHome || restoreHome || restoreNativeTvHome ||
             restoreRemoteKeys ||
             reconcileScreensaver || reconcileAppTiles || openHomeAtBoot ||
             showHomeStartingNotice) && listen)
            throw std::runtime_error("one-shot operations cannot start the HTTP front door");
        const unsigned homeOperationCount = static_cast<unsigned>(releaseHome) +
            static_cast<unsigned>(takeoverHome) + static_cast<unsigned>(restoreHome);
        const unsigned oneShotOperationCount = homeOperationCount +
            static_cast<unsigned>(restoreNativeTvHome) +
            static_cast<unsigned>(restoreRemoteKeys) +
            static_cast<unsigned>(reconcileScreensaver) +
            static_cast<unsigned>(reconcileAppTiles) +
            static_cast<unsigned>(openHomeAtBoot) +
            static_cast<unsigned>(showHomeStartingNotice);
        if (oneShotOperationCount > 1)
            throw std::runtime_error("one-shot operations are mutually exclusive");
        if (blankHomeLayout && !takeoverHome)
            throw std::runtime_error("--blank-home-layout requires --takeover-home");

        // The daemon is the whole server unless a Node upstream is named with
        // --node-port (tests of the forwarding) or TVWEB_NODE_FREE=0 says so.
        if (listen && !nodePortGiven && !std::getenv("TVWEB_NODE_FREE"))
            setenv("TVWEB_NODE_FREE", "1", 1);
        if (listen && environmentEnabled("TVWEB_NODE_FREE"))
            applyNodeFreeEnvironment();

        ConfigLoadResult firstLoad = ConfigLoader::load(configOptions);
        // Node's start-up migration is not run once Node is gone: an install that
        // had tile hiding on keeps it allowed.
        if (firstLoad.loaded &&
            migrateTileHidingConfig(firstLoad.path, firstLoad.config.document, dataDirectory()))
            firstLoad = ConfigLoader::load(configOptions);
        if (firstLoad.loaded &&
            migrateLegacyConfig(firstLoad.path, firstLoad.config.document))
            firstLoad = ConfigLoader::load(configOptions);
        // The same for the old piccap.enabled, which became an entity setting.
        if (firstLoad.loaded) {
            const Json piccapChange = service::piccapConfigMigration(firstLoad.config.document);
            if (piccapChange.isObject() && ConfigStore(firstLoad.path).setTopLevel(piccapChange))
                firstLoad = ConfigLoader::load(configOptions);
        }
        const ConfigLoadResult loaded = std::move(firstLoad);
        const char *supervisedValue = std::getenv("TVWEB_SUPERVISED");
        const bool supervised = supervisedValue && std::string(supervisedValue) == "1";
        const pid_t supervisorPid = getppid();
        LoggerOptions loggerOptions;
        loggerOptions.configLevel = configuredLogLevel(loaded.config.document);
        Logger logger(std::move(loggerOptions));
        for (const std::string &warning : loaded.warnings)
            logger.warning(warning);

        const bool nativeMqttShadowRequested =
            environmentEnabled("TVWEB_NATIVE_MQTT_SHADOW");
        const bool nativeMqttProductionRequested =
            environmentEnabled("TVWEB_NATIVE_MQTT_PRODUCTION");
        const bool nativeStateConfigured = environmentEnabled("TVWEB_NATIVE_STATE");
        const bool nativeRemoteInputConfigured =
            environmentEnabled("TVWEB_NATIVE_REMOTE_INPUT");
        const bool mqttEnabled = mqttConfigured(loaded.config.document);
        const bool nativeMqttProduction = nativeMqttProductionRequested &&
            nativeStateConfigured && mqttEnabled;
        const bool nativeMqttShadow = (nativeMqttShadowRequested ||
            nativeMqttProductionRequested) && nativeStateConfigured && mqttEnabled;
        const bool nativeMqttDiscovery = nativeMqttProduction ||
            (environmentEnabled("TVWEB_NATIVE_MQTT_DISCOVERY") && nativeMqttShadow);
        const size_t nativeMqttWorkerFloor = 10;
        if (nativeMqttShadow && nativeStateConfigured && mqttEnabled &&
            workerCount < nativeMqttWorkerFloor) {
            logger.info("native MQTT reserves two blocking workers beside its eight "
                        "long-lived Luna subscriptions");
            workerCount = nativeMqttWorkerFloor;
        }

        // Each live Luna subscription holds a worker for as long as it runs: eight
        // of them with the state groups on, and a one-shot call queued behind a
        // full set waits for a subscription to end, which it never does.
        const size_t stateWorkerFloor = 12;
        if (nativeStateConfigured && workerCount < stateWorkerFloor) {
            logger.info("native state raises the blocking workers to " +
                        std::to_string(stateWorkerFloor) + " for its Luna subscriptions");
            workerCount = stateWorkerFloor;
        }

        if (!installSignalHandlers())
            throw std::runtime_error("could not install SIGTERM/SIGINT handlers");

        EventLoop loop(workerCount, 64, 256, [&logger](std::exception_ptr error) {
            try {
                if (error)
                    std::rethrow_exception(error);
            } catch (const std::exception &exception) {
                logger.error(std::string("event loop callback failed: ") + exception.what());
            } catch (...) {
                logger.error("event loop callback failed with an unknown exception");
            }
        });
        ChildProcessRunner children;
        LunaClient luna(loop, children, &logger);
        CommandPolicyExecutor commandPolicy(luna, &logger);
        Heartbeat heartbeat(loop, heartbeatPath);
        std::unique_ptr<StateNodeBridge> stateBridge;
        std::unique_ptr<StateSubscriptions> stateSubscriptions;
        std::unique_ptr<MqttClient> mqttShadowClient;
        std::unique_ptr<MqttTopics> mqttShadowTopics;
        std::unique_ptr<MqttStatePublisher> mqttShadowPublisher;
        std::unique_ptr<MqttTelemetryPublisher> mqttShadowTelemetryPublisher;
        std::unique_ptr<MqttAvailabilityPublisher> mqttShadowAvailability;
        std::unique_ptr<RemoteButtonsReader> remoteButtonsReader;
        std::unique_ptr<RemoteHomeHoldTimer> remoteHomeHoldTimer;
        std::unique_ptr<RemoteButtonLogWatcher> remoteButtonLogWatcher;
        std::unique_ptr<RemoteKeysReconciler> nativeRemoteKeysReconciler;
        std::unique_ptr<MqttCommandDispatcher> mqttShadowCommandDispatcher;
        const std::shared_ptr<std::atomic<bool>> mqttShadowErrorLogged =
            std::make_shared<std::atomic<bool>>(false);
        std::unique_ptr<service::HistoryRecorder> historyRecorder;
        std::unique_ptr<ScreenTimeEnforcer> screenTimeEnforcer;
        std::unique_ptr<SyslogForwarder> syslogForwarder;
        std::unique_ptr<NtpClient> ntpClient;
        std::unique_ptr<GameFrameRate> gameFrameRate;
        std::unique_ptr<RemoteBattery> remoteBattery;
        std::unique_ptr<AdBlockManager> adBlockManager;
        std::unique_ptr<DevtoolsFirewall> devtoolsFirewall;
        std::unique_ptr<ScreensaverManager> screensaverManager;
        std::unique_ptr<HomeScreenManager> homeScreenManager;
        std::unique_ptr<HomeTakeoverManager> homeTakeoverManager;
        std::unique_ptr<HomeTakeoverLifecycleManager> homeTakeoverLifecycleManager;
        std::unique_ptr<SamRescanManager> samRescanManager;
        std::unique_ptr<AppTileManager> appTileManager;
        std::unique_ptr<TvAppManager> tvAppManager;
        std::unique_ptr<ServicesKeeper> servicesKeeper;
        std::function<Json()> mqttStandingProvider;
        std::unique_ptr<HttpFrontDoor> frontDoor;
        std::unique_ptr<DailyBackupScheduler> dailyBackupScheduler;
        HttpServer httpServer;
        HttpServer phoneHandoffServer;
        HttpServer iconHttpServer;
        if (openHomeAtBoot) {
            HomeBootManager homeBootManager(luna, dataDirectory(), systemRoot, &logger);
            std::thread loopThread([&loop]() { loop.run(); });
            Json result;
            try {
                result = homeBootManager.openAtBoot();
            } catch (...) {
                loop.stop();
                loopThread.join();
                throw;
            }
            loop.stop();
            loopThread.join();
            if (!result["ok"].isBoolean() || !result["ok"].boolean())
                throw std::runtime_error(result["error"].isString()
                    ? result["error"].string()
                    : "could not evaluate Home boot launch policy");
            return 0;
        }
        if (showHomeStartingNotice) {
            HomeBootManager homeBootManager(luna, dataDirectory(), systemRoot, &logger);
            std::thread loopThread([&loop]() { loop.run(); });
            Json result;
            try {
                result = homeBootManager.showStartingNotice();
            } catch (...) {
                loop.stop();
                loopThread.join();
                throw;
            }
            loop.stop();
            loopThread.join();
            if (!result["ok"].isBoolean() || !result["ok"].boolean())
                throw std::runtime_error(result["error"].isString()
                    ? result["error"].string()
                    : "could not show the Home startup notice");
            return 0;
        }
        if (restoreRemoteKeys) {
            std::thread loopThread([&loop]() { loop.run(); });
            CommandPolicyContext context;
            context.allowControl = loaded.config.allowControl;
            context.allowPower = loaded.config.allowPower;
            const Json firmware = loaded.config.document["device"]["sw_version"];
            if (firmware.isString()) context.firmwareVersion = firmware.string();
            std::string error;
            bool restored = false;
            try {
                if (nativetv::service::hasJavaScriptRemoteKeys(systemRoot)) {
                    restored = restoreJavaScriptRemoteKeys(
                        commandPolicy, context, systemRoot, error);
                } else {
                    HttpProxy nativeProxy(HttpProxyOptions{});
                    HttpProxy nativeHandoffProxy(HttpProxyOptions{});
                    frontDoor.reset(new HttpFrontDoor(
                        loaded.config, NATIVETV_SERVER_VERSION, dataDirectory(),
                        loaded.path, std::move(nativeProxy),
                        std::move(nativeHandoffProxy), luna, children, assetsRoot,
                        procRoot, systemRoot, cpuWindowMs, &logger, nullptr,
                        nullptr, &commandPolicy));
                    restored = restoreNativeRemoteKeys(
                        *frontDoor, dataDirectory(), error);
                }
            } catch (...) {
                loop.stop();
                loopThread.join();
                throw;
            }
            loop.stop();
            loopThread.join();
            if (!restored)
                throw std::runtime_error(error.empty()
                    ? "could not restore LG remote-key settings" : error);
            return 0;
        }
        if (reconcileScreensaver) {
            screensaverManager.reset(new ScreensaverManager(
                children, luna, systemRoot, assetsRoot, &logger));
            samRescanManager.reset(new SamRescanManager(
                commandPolicy, children, systemRoot, &logger));
            std::thread loopThread([&loop]() { loop.run(); });
            Json result;
            try {
                CommandPolicyContext context;
                context.allowControl = loaded.config.allowControl;
                context.allowPower = loaded.config.allowPower;
                const Json firmware = loaded.config.document["device"]["sw_version"];
                if (firmware.isString()) context.firmwareVersion = firmware.string();
                const bool allowExperimental =
                    loaded.config.document["allowExperimentalScreensavers"].boolean() ||
                    loaded.config.document["allowOnWebos10"].boolean();
                result = screensaverManager->reconcileBoot(
                    *samRescanManager, context, allowExperimental);
            } catch (...) {
                loop.stop();
                loopThread.join();
                throw;
            }
            loop.stop();
            loopThread.join();
            if (!result["ok"].isBoolean() || !result["ok"].boolean())
                throw std::runtime_error(result["error"].isString()
                    ? result["error"].string()
                    : "could not reconcile the screen saver at boot");
            return 0;
        }
        if (reconcileAppTiles) {
            appTileManager.reset(new AppTileManager(
                children, systemRoot, dataDirectory()));
            samRescanManager.reset(new SamRescanManager(
                commandPolicy, children, systemRoot, &logger));
            std::thread loopThread([&loop]() { loop.run(); });
            Json result;
            try {
                CommandPolicyContext context;
                context.allowControl = loaded.config.allowControl;
                context.allowPower = loaded.config.allowPower;
                const Json firmware = loaded.config.document["device"]["sw_version"];
                if (firmware.isString()) context.firmwareVersion = firmware.string();
                result = appTileManager->restoreHiddenAppsAtBoot(
                    loaded.config.document, *samRescanManager, context);
            } catch (...) {
                loop.stop();
                loopThread.join();
                throw;
            }
            loop.stop();
            loopThread.join();
            if (!result["ok"].isBoolean() || !result["ok"].boolean())
                throw std::runtime_error(result["error"].isString()
                    ? result["error"].string()
                    : "could not reconcile hidden app tiles at boot");
            return 0;
        }
        if (listen || releaseHome || takeoverHome || restoreHome ||
            restoreNativeTvHome) {
            homeScreenManager.reset(new HomeScreenManager(
                children, systemRoot, dataDirectory(), procRoot,
                loaded.config.document["allowHomeLayout"].boolean()));
            homeTakeoverManager.reset(new HomeTakeoverManager(
                children, assetsRoot + "/home-app",
                dataDirectory() + "/home-takeover", systemRoot,
                loaded.config.port));
            samRescanManager.reset(new SamRescanManager(commandPolicy, children,
                                                        systemRoot, &logger));
            homeTakeoverLifecycleManager.reset(new HomeTakeoverLifecycleManager(
                *homeScreenManager, *homeTakeoverManager, *samRescanManager,
                luna, systemRoot));
        }

        if (restoreNativeTvHome) {
            std::thread loopThread([&loop]() { loop.run(); });
            Json result;
            try {
                CommandPolicyContext context;
                context.allowControl = loaded.config.allowControl;
                context.allowPower = loaded.config.allowPower;
                const Json firmware = loaded.config.document["device"]["sw_version"];
                if (firmware.isString()) context.firmwareVersion = firmware.string();
                HomeRestoreManager manager(*homeTakeoverLifecycleManager,
                    commandPolicy, dataDirectory());
                result = manager.restore(context);
            } catch (...) {
                loop.stop();
                loopThread.join();
                throw;
            }
            loop.stop();
            loopThread.join();
            if (!result["ok"].isBoolean() || !result["ok"].boolean())
                throw std::runtime_error(result["error"].isString()
                    ? result["error"].string()
                    : "could not restore Native Tv Home");
            return 0;
        }

        if (releaseHome || takeoverHome || restoreHome) {
            bool blankStartLayout = blankHomeLayout;
            if (takeoverHome) {
                const JsonFileResult homeSettings =
                    JsonFile(dataDirectory() + "/home.json", 256 * 1024).load();
                if (!homeSettings.found || !homeSettings.value.isObject() ||
                    !homeSettings.value["enabled"].boolean()) {
                    logger.info("Home takeover skipped: Native Tv Home is disabled");
                    return 3;
                }
                if (!homeScreenManager->supportsTakeover(
                        loaded.config.document["allowExperimentalHome"].boolean())) {
                    logger.info("Home takeover skipped: firmware is unsupported or experimental Home is disabled");
                    return 4;
                }
                if (!blankStartLayout &&
                    homeSettings.value["hideLgHomeAtStart"].boolean() &&
                    homeScreenManager->isWebos9())
                    blankStartLayout = true;
            }
            std::thread loopThread([&loop]() { loop.run(); });
            Json result;
            Json refusal;
            try {
                CommandPolicyContext context;
                context.allowControl = loaded.config.allowControl;
                context.allowPower = loaded.config.allowPower;
                const Json firmware = loaded.config.document["device"]["sw_version"];
                if (firmware.isString())
                    context.firmwareVersion = firmware.string();
                if (takeoverHome) {
                    // The stub that takes LG's Home's place has no page of its own to
                    // show, so sam has to know the client and it has to be able to draw.
                    HomeProbeGate probeGate(children, homeProbeOptions(loaded.config.document,
                                                                       systemRoot, dataDirectory()),
                                            [&luna]() { return homeClientRegistration(luna); });
                    const Json probe = probeGate.verdict(false);
                    if (!probe["ok"].boolean()) refusal = probe;
                }
                if (refusal.valid()) {
                    // Nothing to take; the result below is not read.
                } else if (restoreHome)
                    result = restoreHomeScreen(*homeScreenManager, commandPolicy, context);
                else if (takeoverHome)
                    result = homeTakeoverLifecycleManager->takeover(blankStartLayout, context);
                else
                    result = homeTakeoverLifecycleManager->release(context);
            } catch (...) {
                loop.stop();
                loopThread.join();
                throw;
            }
            loop.stop();
            loopThread.join();
            if (refusal.valid()) {
                logger.info("Home takeover skipped: the native client is refused, " +
                            refusal["class"].string() + " (exit " +
                            std::to_string(static_cast<int>(refusal["exit"].number(-1))) + ")");
                return 4;
            }
            if (!result["ok"].isBoolean() || !result["ok"].boolean())
                throw std::runtime_error(result["error"].isString()
                    ? result["error"].string()
                    : (restoreHome ? "could not restore LG's Home screen"
                       : (takeoverHome ? "could not take over LG Home"
                                       : "could not release LG Home takeover")));
            return 0;
        }
        if (listen) {
            devtoolsFirewall.reset(new DevtoolsFirewall(children, systemRoot,
                                                         procRoot, &logger));
            screensaverManager.reset(new ScreensaverManager(children, luna, systemRoot,
                                                              assetsRoot, &logger));
            screensaverManager->reconcileStartup();
            appTileManager.reset(new AppTileManager(children, systemRoot, dataDirectory()));
            adBlockManager.reset(new AdBlockManager(loop, luna, dataDirectory(),
                                                     systemRoot, &logger));
            std::string adBlockError;
            if (!adBlockManager->start(&adBlockError))
                throw std::runtime_error("could not restore the ad-block hosts table" +
                                         (adBlockError.empty() ? std::string()
                                                               : ": " + adBlockError));
            if (loaded.config.port == 65535)
                throw std::runtime_error(
                    "the native phone handoff listener needs the public port plus one");
            const bool iconServerEnabled = loaded.config.document["iconServer"].isBoolean() &&
                                           loaded.config.document["iconServer"].boolean();
            if (iconServerEnabled && loaded.config.port > 65533)
                throw std::runtime_error(
                    "the native app icon listener needs the public port plus two");
            const bool noNode = environmentEnabled("TVWEB_NODE_FREE");
            if (!noNode && nodePort == 65535)
                throw std::runtime_error(
                    "the private Node handoff listener needs the Node port plus one");
            const unsigned short publicHandoffPort =
                static_cast<unsigned short>(loaded.config.port + 1);
            const unsigned short nodeHandoffPort = static_cast<unsigned short>(nodePort + 1);
            const unsigned short iconPort = iconServerEnabled
                ? static_cast<unsigned short>(loaded.config.port + 2) : 0;
            if (!noNode &&
                (nodePort == loaded.config.port || nodePort == publicHandoffPort ||
                 nodeHandoffPort == loaded.config.port || nodeHandoffPort == publicHandoffPort ||
                 (iconServerEnabled &&
                  (nodePort == iconPort || nodeHandoffPort == iconPort))))
                throw std::runtime_error(
                    "public and private HTTP listener ports must not overlap");

            const char *secret = std::getenv("TVWEB_PROXY_SECRET");
            // No Node (TVWEB_NODE_FREE=1): its private routes are answered from the
            // daemon through these hooks, filled in once the front door exists.
            const std::shared_ptr<NodeFreeHooks> nodeFreeHooks = environmentEnabled("TVWEB_NODE_FREE")
                ? std::make_shared<NodeFreeHooks>() : nullptr;
            HttpProxyOptions proxyOptions;
            proxyOptions.port = nodePort;
            proxyOptions.sharedSecret = secret ? secret : "";
            HttpProxyOptions stateProxyOptions = proxyOptions;
            stateProxyOptions.responseTimeoutMs = 2000;
            stateProxyOptions.maximumBodyBytes = 16 * 1024;
            // With no Node there is nothing to tell the state to.
            if (!environmentEnabled("TVWEB_NODE_FREE"))
                stateBridge.reset(new StateNodeBridge(loop, HttpProxy(stateProxyOptions), &logger));
            const std::shared_ptr<std::atomic<long long>> mqttLastPublishMs =
                std::make_shared<std::atomic<long long>>(0);
            const std::shared_ptr<std::atomic<bool>> mqttErrorActive =
                std::make_shared<std::atomic<bool>>(false);
            // Where the bridge stands, for /api/settings, when the daemon owns it.
            struct MqttStanding {
                std::mutex mutex;
                std::string state = "disabled";
                std::string detail;
                long long sinceMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
            };
            const std::shared_ptr<MqttStanding> mqttStanding = std::make_shared<MqttStanding>();
            mqttStandingProvider = [mqttStanding, mqttLastPublishMs, &mqttShadowClient]() {
                Json standing = Json::object();
                {
                    std::lock_guard<std::mutex> lock(mqttStanding->mutex);
                    standing.set("state", Json::text(mqttStanding->state));
                    standing.set("detail", Json::text(mqttStanding->detail));
                    standing.set("sinceMs", Json::numeric(static_cast<double>(mqttStanding->sinceMs)));
                }
                standing.set("lastPublishMs", Json::numeric(static_cast<double>(
                    mqttLastPublishMs->load(std::memory_order_acquire))));
                standing.set("latencyMs", Json::numeric(static_cast<double>(
                    mqttShadowClient ? mqttShadowClient->latencyMilliseconds() : -1)));
                return standing;
            };
            const std::function<void(const std::string &, const std::string &)>
                updateNativeMqttStatus = [&stateBridge, &mqttShadowClient,
                    mqttLastPublishMs, nativeMqttProduction, mqttStanding](
                        const std::string &state, const std::string &detail) {
                    if (nativeMqttProduction) {
                        std::lock_guard<std::mutex> lock(mqttStanding->mutex);
                        if (mqttStanding->state != state)
                            mqttStanding->sinceMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch()).count();
                        mqttStanding->state = state;
                        mqttStanding->detail = detail;
                    }
                    if (!nativeMqttProduction || !stateBridge)
                        return;
                    Json status = Json::object();
                    status.set("state", Json::text(state));
                    status.set("detail", Json::text(detail));
                    status.set("lastPublishMs", Json::numeric(static_cast<double>(
                        mqttLastPublishMs->load(std::memory_order_acquire))));
                    const long long latency = mqttShadowClient
                        ? mqttShadowClient->latencyMilliseconds() : -1;
                    status.set("latencyMs", Json::numeric(static_cast<double>(latency)));
                    stateBridge->publishMqttStatus(status);
                };
            const char *nativeSyslogEnabled = std::getenv("TVWEB_NATIVE_SYSLOG");
            if (nativeSyslogEnabled && std::string(nativeSyslogEnabled) == "1") {
                SyslogForwarderOptions syslogOptions;
                syslogOptions.procRoot = procRoot;
                syslogOptions.statusChanged = [&stateBridge](const Json &status) {
                    if (stateBridge)
                        stateBridge->publishSyslogStatus(status);
                };
                syslogForwarder.reset(new SyslogForwarder(
                    loaded.config.document, children, &logger, std::move(syslogOptions)));
                if (syslogForwarder->start()) {
                    const std::string configuredName = loaded.config.document["device"]["name"].string();
                    const std::string configuredSyslogHostname =
                        loaded.config.document["syslog"]["hostname"].string();
                    if (configuredSyslogHostname.empty() &&
                        (configuredName.empty() || configuredName == "LG webOS TV" ||
                         configuredName == "LG OLED B8 TV")) {
                        Json properties = Json::object();
                        Json keys = Json::array();
                        keys.add(Json::text("modelName"));
                        properties.set("keys", keys);
                        (void)luna.call(
                            "com.webos.service.tv.systemproperty/getSystemProperties", properties,
                            [&syslogForwarder, configuredName](const LunaReply &reply) {
                                const std::string model = reply.response["modelName"].string();
                                if (syslogForwarder)
                                    syslogForwarder->setHostname(
                                        model.empty()
                                            ? (configuredName.empty() ? "LG webOS TV" : configuredName)
                                            : "LG " + model);
                            });
                    }
                }
            }
            const char *nativeNtpEnabled = std::getenv("TVWEB_NATIVE_NTP");
            if (nativeNtpEnabled && std::string(nativeNtpEnabled) == "1") {
                ntpClient.reset(new NtpClient(loop, luna, &logger));
                ntpClient->setStatusChanged([&stateBridge](const Json &status) {
                    if (stateBridge)
                        stateBridge->publishNtpStatus(status);
                });
                (void)ntpClient->watchConfig(loaded.path, loaded.config.document);
            }
            stateSubscriptions.reset(new StateSubscriptions(
                loop, luna, state::MappingOptions(), &logger, StateSubscriptions::Invalidate(),
                [&stateBridge](const std::string &group, bool isLive) {
                    if (stateBridge)
                        stateBridge->publishStatus(group, isLive);
                }));
            std::shared_ptr<MqttShadowDiscoveryRuntime> mqttShadowDiscovery;
            std::string mqttShadowSourceTelemetryTopic;
            std::string mqttShadowLegacyTelemetryTopic;
            if (environmentEnabled("TVWEB_NATIVE_MQTT_DISCOVERY") && !nativeMqttShadow)
                logger.warning("native MQTT discovery requires native state and an enabled broker");
            if (nativeMqttShadowRequested && !nativeStateConfigured)
                logger.warning("native MQTT shadow requires TVWEB_NATIVE_STATE=1");
            if (nativeMqttProductionRequested && !nativeStateConfigured)
                logger.warning("native MQTT production requires TVWEB_NATIVE_STATE=1");
            if (nativeMqttProductionRequested && nativeStateConfigured && !mqttEnabled)
                logger.warning("native MQTT production requires an enabled broker with a host");
            if (nativeRemoteInputConfigured && !nativeStateConfigured)
                logger.warning("native remote input bridge requires TVWEB_NATIVE_STATE=1");
            if (nativeMqttShadow && nativeStateConfigured && mqttEnabled) {
                const Json mqttConfig = loaded.config.document["mqtt"];
                std::string sourcePrefix = mqttConfig["topicPrefix"].string();
                if (sourcePrefix.empty()) sourcePrefix = "lgtv";
                mqttShadowSourceTelemetryTopic = sourcePrefix + "/telemetry";
                mqttShadowLegacyTelemetryTopic = sourcePrefix + "_legacy/telemetry";
                const std::string prefix = sourcePrefix +
                    (nativeMqttProduction ? "" : "_native");
                mqttShadowTopics.reset(new MqttTopics(prefix));
                mqttShadowClient.reset(new MqttClient(
                    mqttShadowOptions(loaded.config.document, prefix,
                                      !nativeMqttProduction)));
                mqttShadowAvailability.reset(new MqttAvailabilityPublisher(
                    *mqttShadowTopics,
                    [&mqttShadowClient](const std::string &topic,
                                        const std::string &payload, bool retained) {
                        return mqttShadowClient &&
                               mqttShadowClient->publish(topic, payload, retained);
                    },
                    [&mqttShadowClient](const std::string &payload) {
                        return mqttShadowClient && mqttShadowClient->setWill(payload);
                    }));
                mqttShadowDiscovery = std::make_shared<MqttShadowDiscoveryRuntime>();
                HaDiscoveryOptions &discoveryOptions = mqttShadowDiscovery->options;
                discoveryOptions.topicPrefix = prefix;
                discoveryOptions.discoveryPrefix =
                    mqttConfig["discoveryPrefix"].string();
                if (discoveryOptions.discoveryPrefix.empty())
                    discoveryOptions.discoveryPrefix = "homeassistant";
                if (!nativeMqttProduction)
                    discoveryOptions.discoveryPrefix += "_native";
                std::string deviceId = loaded.config.document["device"]["id"].string();
                if (deviceId.empty()) deviceId = "lg_b8_tv";
                discoveryOptions.deviceId = deviceId +
                    (nativeMqttProduction ? "" : "_native");
                discoveryOptions.allowPower = loaded.config.allowPower;
                // In production the switch waits for PicCap to answer (piccapPresent below).
                discoveryOptions.piccap = !nativeMqttProduction;
                discoveryOptions.applyCapabilities = true;
                mqttShadowDiscovery->isOled = mqttShadowIsOled(
                    loaded.config.document, systemRoot);
                discoveryOptions.installedApps = Json::array();
                discoveryOptions.pictureModes = Json::array();
                const Json configuredDevice = loaded.config.document["device"];
                discoveryOptions.device = Json::object();
                for (const char *key : {"name", "model", "manufacturer", "sw_version"}) {
                    const Json value = configuredDevice[key];
                    if (value.isString() && !value.string().empty())
                        discoveryOptions.device.set(key, value);
                }
                mqttShadowPublisher.reset(new MqttStatePublisher(
                    loop, stateSubscriptions->cache(), *mqttShadowTopics,
                    [&mqttShadowClient](const std::string &topic,
                                       const std::string &payload, bool retained) {
                        return mqttShadowClient &&
                               mqttShadowClient->publish(topic, payload, retained);
                    }));
                MqttTelemetryOptions telemetryOptions;
                telemetryOptions.systemRoot = systemRoot;
                telemetryOptions.dataRoot = dataDirectory();
                double telemetryInterval = mqttConfig["telemetryIntervalMs"].number(30000);
                if (!std::isfinite(telemetryInterval) || telemetryInterval < 1000 ||
                    telemetryInterval > 3600000)
                    telemetryInterval = 30000;
                telemetryOptions.intervalMs = static_cast<unsigned>(telemetryInterval);
                telemetryOptions.onPublished = [mqttLastPublishMs,
                    updateNativeMqttStatus]() {
                    const long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    mqttLastPublishMs->store(nowMs, std::memory_order_release);
                    updateNativeMqttStatus("connected", "");
                };
                telemetryOptions.collectExtra = [&frontDoor]() {
                    return frontDoor ? frontDoor->collectMqttLgSettings() : Json::object();
                };
                // Production publishes what Node's publishTelemetry did: the whole
                // stats read, not the live state and the system snapshot alone.
                // Whether PicCap answered, for the discovery of its switch.
                const std::shared_ptr<std::atomic<bool>> piccapPresent =
                    std::make_shared<std::atomic<bool>>(!nativeMqttProduction);
                if (nativeMqttProduction)
                    telemetryOptions.composeFull = [&frontDoor, &mqttShadowClient, piccapPresent,
                                                    piccapTopic = mqttShadowTopics->state("piccap/power")](
                        const Json &system, const Json &extra) {
                        Json payload = frontDoor ? frontDoor->composeMqttTelemetry(system, extra) : Json();
                        if (!payload.valid())
                            return payload;
                        // The PicCap switch reads its own retained topic: ON or OFF while
                        // PicCap answers, and cleared once it stops.
                        const bool present = payload["piccap"].isObject();
                        const bool was = piccapPresent->exchange(present);
                        if (mqttShadowClient && (present || was))
                            (void)mqttShadowClient->publish(
                                piccapTopic,
                                present ? (payload["piccap"]["power"].boolean() ? "ON" : "OFF") : "",
                                true);
                        return payload;
                    };
                if (nativeMqttDiscovery) {
                    telemetryOptions.onCollected = [mqttShadowDiscovery,
                        &mqttShadowClient, &logger](const Json &, const Json &extra) {
                        if (!mqttShadowDiscovery)
                            return;
                        const Json rows = extra["rows"];
                        mqttShadowDiscovery->options.lgSettings = rows.isArray()
                            ? rows : Json::array();
                        mqttShadowDiscovery->lgSettingsReady = true;
                        logger.debug("native MQTT: read " +
                            std::to_string(mqttShadowDiscovery->options.lgSettings.size()) +
                            " LG settings rows");
                        publishMqttShadowDiscoveryIfReady(
                            mqttShadowDiscovery, mqttShadowClient.get(), &logger);
                    };
                    const Json configuredMqttEntities = mqttConfig["entities"];
                    const std::string dataRoot = dataDirectory();
                    telemetryOptions.onSystemCollected = [piccapPresent, mqttShadowDiscovery,
                        &mqttShadowClient, &logger, &historyRecorder, &frontDoor,
                        configuredMqttEntities,
                        configDocument = loaded.config.document,
                        systemRoot, dataRoot](const Json &system) {
                        if (!mqttShadowDiscovery)
                            return;
                        mqttShadowObserveHdmiFacts(system, dataRoot);
                        const bool watchHistory = historyRecorder &&
                            historyRecorder->status()["running"].boolean();
                        mqttShadowDiscovery->isOled = mqttShadowIsOled(
                            configDocument, systemRoot,
                            mqttShadowDiscovery->detectedModel);
                        // The network address goes into the device's own record, where
                        // Home Assistant shows it and its Wake-on-LAN integration can
                        // take it from: the one way to reach the TV once it is asleep.
                        const Json mac = system["net"]["mac"];
                        if (mac.isString() && !mac.string().empty() &&
                            !mqttShadowDiscovery->options.device.has("connections")) {
                            Json pair = Json::array();
                            pair.add(Json::text("mac"));
                            pair.add(mac);
                            Json connections = Json::array();
                            connections.add(pair);
                            mqttShadowDiscovery->options.device.set("connections", connections);
                        }
                        // What the stats read found, as the server's discovery takes it
                        // from its own: the TV's model and firmware, the picture modes
                        // and sound outputs it offers now. The reads made at start stand
                        // until a collection has run.
                        const Json inputs = frontDoor ? frontDoor->mqttDiscoveryInputs() : Json();
                        if (inputs.isObject()) {
                            for (const char *key : {"model", "sw_version"}) {
                                const Json value = inputs["device"][key];
                                if (value.isString() && !value.string().empty())
                                    mqttShadowDiscovery->options.device.set(key, value);
                            }
                            if (inputs["pictureModes"].size() > 0)
                                mqttShadowDiscovery->options.pictureModes = inputs["pictureModes"];
                            if (inputs["soundOutputs"].size() > 0)
                                mqttShadowDiscovery->options.soundOutputs = inputs["soundOutputs"];
                            mqttShadowDiscovery->collectorInputs = true;
                        }
                        // The PicCap switch exists once the app answers.
                        mqttShadowDiscovery->options.piccap = piccapPresent->load();
                        mqttShadowDiscovery->options.capabilities =
                            mqttShadowDiscoveryCapabilities(
                                configuredMqttEntities, system,
                                mqttShadowDiscovery->probedCapabilities,
                                dataRoot, RemoteButtonsReader::isSupported(),
                                watchHistory, mqttShadowDiscovery->isOled);
                        mqttShadowDiscovery->capabilitiesReady = true;
                        publishMqttShadowDiscoveryIfReady(
                            mqttShadowDiscovery, mqttShadowClient.get(), &logger);
                    };
                }
                mqttShadowTelemetryPublisher.reset(new MqttTelemetryPublisher(
                    loop, stateSubscriptions->cache(), *mqttShadowTopics,
                    [&mqttShadowClient](const std::string &topic,
                                       const std::string &payload, bool retained) {
                        return mqttShadowClient &&
                               mqttShadowClient->publish(topic, payload, retained);
                    }, std::move(telemetryOptions), &logger));
                Json telemetryBase = Json::object();
                telemetryBase.set("tvwebVersion", Json::text(NATIVETV_SERVER_VERSION));
                Json telemetryDevice = Json::object();
                telemetryDevice.set("id", configuredDevice["id"].isString()
                    ? configuredDevice["id"] : Json::text("lg_tv"));
                telemetryDevice.set("name", configuredDevice["name"].isString()
                    ? configuredDevice["name"] : Json::text("LG webOS TV"));
                telemetryDevice.set("model", configuredDevice["model"].isString()
                    ? configuredDevice["model"] : Json::text("webOS TV"));
                telemetryBase.set("device", telemetryDevice);
                mqttShadowTelemetryPublisher->setBase(telemetryBase);
                if (nativeMqttDiscovery && !nativeMqttProduction &&
                    !mqttShadowClient->subscribe(mqttShadowSourceTelemetryTopic, 0))
                    logger.warning("native MQTT: could not subscribe to legacy telemetry");
            }
            if (nativeRemoteInputConfigured ||
                (nativeMqttShadow && nativeStateConfigured && mqttEnabled)) {
                RemoteButtonsReaderOptions remoteOptions;
                const char *inputDirectory = std::getenv("TVWEB_REMOTE_INPUT_DIRECTORY");
                const char *inputDevicesPath = std::getenv("TVWEB_REMOTE_INPUT_DEVICES");
                const char *inputLogPath = std::getenv("TVWEB_REMOTE_INPUT_LOG");
                if (inputDirectory) remoteOptions.inputDirectory = inputDirectory;
                if (inputDevicesPath) remoteOptions.inputDevicesPath = inputDevicesPath;
                if (inputLogPath) remoteOptions.inputLogPath = inputLogPath;
                if (nativeRemoteInputConfigured && stateBridge) {
                    remoteHomeHoldTimer.reset(new RemoteHomeHoldTimer(
                        loop, [&frontDoor, &stateBridge, &loop, &logger]() {
                            if (!frontDoor) {
                                if (stateBridge)
                                    stateBridge->publishRemoteInput(
                                        "home-held", 773, true);
                                return;
                            }
                            if (!loop.submitBlocking([&frontDoor, &stateBridge, &logger]() {
                                    if (!frontDoor) {
                                        if (stateBridge)
                                            stateBridge->publishRemoteInput(
                                                "home-held", 773, true);
                                        return;
                                    }
                                    const Json result =
                                        frontDoor->openLgHomeFromRemoteInput();
                                    if (!result["ok"].boolean()) {
                                        logger.warning("native Home hold: " +
                                            result["error"].string());
                                        if (stateBridge)
                                            stateBridge->publishRemoteInput(
                                                "home-held", 773, true);
                                    }
                                })) {
                                logger.warning("native Home hold: worker queue is full; "
                                               "using Node fallback");
                                if (stateBridge)
                                    stateBridge->publishRemoteInput(
                                        "home-held", 773, true);
                            }
                        }, EventLoop::Milliseconds(1200),
                        [&frontDoor, &stateBridge, &loop, &logger]() {
                            if (!frontDoor) {
                                if (stateBridge)
                                    stateBridge->publishRemoteInput(
                                        "home-short", 773, true);
                                return;
                            }
                            if (!loop.submitBlocking([&frontDoor, &stateBridge, &logger]() {
                                    if (!frontDoor) {
                                        if (stateBridge)
                                            stateBridge->publishRemoteInput(
                                                "home-short", 773, true);
                                        return;
                                    }
                                    const Json result = frontDoor->
                                        returnNativeTvHomeFromRemoteInput();
                                    if (!result["ok"].boolean())
                                        logger.warning("native Home short press: " +
                                            result["error"].string());
                                    if ((!result["ok"].boolean() ||
                                         result["ignored"].boolean() ||
                                         result["fallback"].boolean()) && stateBridge)
                                        stateBridge->publishRemoteInput(
                                            "home-short", 773, true);
                                })) {
                                logger.warning("native Home short press: worker queue is full; "
                                               "using Node fallback");
                                if (stateBridge)
                                    stateBridge->publishRemoteInput(
                                        "home-short", 773, true);
                            }
                        }));
                }
                remoteButtonsReader.reset(new RemoteButtonsReader(
                    [&stateBridge, &mqttShadowClient, &mqttShadowTopics,
                     &remoteHomeHoldTimer, &screensaverManager,
                     nativeRemoteInputConfigured](const RemoteButtonEvent &event) {
                        if (nativeRemoteInputConfigured && event.pressed && screensaverManager)
                            screensaverManager->remoteKeyPressed();
                        if (nativeRemoteInputConfigured && stateBridge) {
                            if (event.type == RemoteButtonEventType::HomeKey &&
                                remoteHomeHoldTimer)
                                remoteHomeHoldTimer->onKeyEvent(event.pressed);
                            const char *type = event.type == RemoteButtonEventType::HomeKey
                                ? "home" : event.type == RemoteButtonEventType::ColorButton
                                    ? "button" : "key";
                            if (event.type != RemoteButtonEventType::Key)
                                stateBridge->publishRemoteInput(type, event.code,
                                                               event.pressed, event.button);
                        }
                        if (event.type != RemoteButtonEventType::ColorButton ||
                            !event.pressed || !mqttShadowClient || !mqttShadowTopics)
                            return;
                        Json payload = Json::object();
                        payload.set("event_type", Json::text(event.button));
                        payload.set("button", Json::text(event.button));
                        (void)mqttShadowClient->publish(
                            mqttShadowTopics->eventsButton(), payload.dump(), false);
                    }, std::move(remoteOptions)));
            }
            if (nativeRemoteInputConfigured &&
                !service::hasJavaScriptRemoteKeys(systemRoot)) {
                RemoteButtonLogWatcherOptions logOptions;
                const char *systemLogPath =
                    std::getenv("TVWEB_REMOTE_KEYS_LOG_PATH");
                if (systemLogPath) logOptions.logPath = systemLogPath;
                logOptions.watchAi = true;
                CommandPolicyContext policyContext;
                policyContext.allowControl = loaded.config.allowControl;
                policyContext.allowPower = loaded.config.allowPower;
                const Json firmware = loaded.config.document["device"]["sw_version"];
                if (firmware.isString())
                    policyContext.firmwareVersion = firmware.string();
                remoteButtonLogWatcher.reset(new RemoteButtonLogWatcher(
                    commandPolicy, policyContext, dataDirectory(), &logger,
                    std::move(logOptions)));
                if (!remoteButtonLogWatcher->start())
                    logger.warning("native remote keys: button log watcher unavailable");
            }
            remoteBattery.reset(new RemoteBattery(loop, luna, dataDirectory(), systemRoot,
                                                  &logger));
            const char *nativeHistoryEnabled = std::getenv("TVWEB_NATIVE_HISTORY");
            if (nativeHistoryEnabled && std::string(nativeHistoryEnabled) == "1") {
                service::HistoryRecorderOptions historyOptions;
                historyOptions.directory = dataDirectory() + "/history";
                historyOptions.enabled = loaded.config.document["history"]["enabled"].boolean();
                historyRecorder.reset(new service::HistoryRecorder(std::move(historyOptions)));
            }
            stateSubscriptions->cache().onChange(
                [&stateBridge, &syslogForwarder, &stateSubscriptions, &historyRecorder,
                 &remoteBattery, &nativeRemoteKeysReconciler, &frontDoor, &loop,
                 &mqttShadowAvailability](
                    const state::Event &event) {
                if (stateBridge)
                    stateBridge->publish(event);
                if (frontDoor) frontDoor->expireStats(event.group);
                // HDMI tile titles on webOS 9's Home follow CEC names, which EIM
                // cannot announce: read again when Home opens, and once CEC
                // devices have had time to announce themselves after the TV
                // switches on. Never in standby.
                const auto refreshHomeTitles = [&frontDoor, &loop, &stateSubscriptions]() {
                    if (!stateSubscriptions) return;
                    const Json systemOn = stateSubscriptions->snapshot()["power"]["systemOn"];
                    if (systemOn.isBoolean() && !systemOn.boolean()) return;
                    (void)loop.submitBlocking([&frontDoor]() {
                        if (frontDoor) frontDoor->refreshHomeInputTitles();
                    });
                };
                // The wake open and auto launch's follow of the screen (home.js).
                if (frontDoor) {
                    if (HomeOrchestrator *orchestrator = frontDoor->homeOrchestrator()) {
                        if (event.group == "application" && event.key == "app_id")
                            orchestrator->onForeground(event.value.isString() ? event.value.string()
                                                                              : std::string());
                        if (event.group == "power" && event.key == "screenOn")
                            orchestrator->onScreen(event.value.isBoolean() && event.value.boolean());
                        if (event.group == "power" && event.key == "state")
                            orchestrator->onPowerState(event.value.isString() ? event.value.string()
                                                                              : std::string());
                    }
                }
                if (event.group == "surface" && event.key == "control_app_id" &&
                    event.value.isString() && event.value.string() == "com.webos.app.home")
                    refreshHomeTitles();
                if (event.group == "power" && event.key == "systemOn" &&
                    event.value.isBoolean() && event.value.boolean()) {
                    (void)loop.scheduleAfter(std::chrono::milliseconds(20000), refreshHomeTitles);
                    (void)loop.submitBlocking([&frontDoor]() {
                        if (frontDoor) frontDoor->resendPreload();
                    });
                }
                if (syslogForwarder && event.group == "power" && event.key == "state")
                    syslogForwarder->flush();
                if (historyRecorder && stateSubscriptions)
                    historyRecorder->observe(event, stateSubscriptions->snapshot());
                if (remoteBattery && event.group == "power" && event.key == "systemOn" &&
                    event.value.isBoolean())
                    remoteBattery->setPowerOn(event.value.boolean());
                if (nativeRemoteKeysReconciler && event.group == "power" &&
                    event.key == "state")
                    nativeRemoteKeysReconciler->powerChanged();
                if (mqttShadowAvailability && event.group == "power" &&
                    event.key == "systemOn" && event.value.isBoolean())
                    mqttShadowAvailability->setSystemOn(event.value.boolean());
            });
            const char *nativeScreenTimeEnabled = std::getenv("TVWEB_NATIVE_SCREENTIME");
            if (nativeScreenTimeEnabled && std::string(nativeScreenTimeEnabled) == "1") {
                const char *nativeStateEnabled = std::getenv("TVWEB_NATIVE_STATE");
                if (!historyRecorder || !stateSubscriptions || !nativeStateEnabled ||
                    std::string(nativeStateEnabled) != "1")
                    throw std::runtime_error(
                        "native screen-time enforcement requires native state and history");
                screenTimeEnforcer.reset(new ScreenTimeEnforcer(
                    loop, stateSubscriptions->cache(), *historyRecorder, luna,
                    dataDirectory(), procRoot, systemRoot,
                    loaded.config.document["screenTime"], loaded.config.allowControl,
                    loaded.config.allowPower, &logger, ScreenTimeTimings(),
                    &commandPolicy));
            }
            HttpProxy proxy(std::move(proxyOptions), nodeFreeHooks);
            HttpProxyOptions handoffProxyOptions;
            handoffProxyOptions.port = nodeHandoffPort;
            handoffProxyOptions.maximumBodyBytes = 16 * 1024;
            handoffProxyOptions.sharedSecret = secret ? secret : "";
            HttpProxy handoffProxy(std::move(handoffProxyOptions), nodeFreeHooks);
            std::string proxyError;
            if (!proxy.valid(proxyError))
                throw std::runtime_error(proxyError +
                                         "; export TVWEB_PROXY_SECRET for both daemon and Node");
            if (!handoffProxy.valid(proxyError))
                throw std::runtime_error(proxyError +
                                         "; export TVWEB_PROXY_SECRET for both daemon and Node");
            gameFrameRate.reset(new GameFrameRate(loop, luna, &logger));
            frontDoor.reset(new HttpFrontDoor(loaded.config, NATIVETV_SERVER_VERSION,
                                              dataDirectory(), loaded.path, std::move(proxy),
                                              std::move(handoffProxy), luna, children, assetsRoot,
                                              procRoot, systemRoot, cpuWindowMs, &logger,
                                              historyRecorder.get(), screenTimeEnforcer.get(),
                                              &commandPolicy, gameFrameRate.get(),
                                              stateSubscriptions.get(), adBlockManager.get(),
                                              devtoolsFirewall.get(),
                                              screensaverManager.get(),
                                              homeScreenManager.get(),
                                              samRescanManager.get(),
                                              appTileManager.get(), {}, {},
                                              homeTakeoverLifecycleManager.get(),
                                              &loop));
            if (nodeFreeHooks) {
                nodeFreeHooks->restart = [&frontDoor](bool checkOnly, int &status, std::string &body,
                                                      std::string &error) {
                    return frontDoor->nodeFreeRestart(checkOnly, status, body, error);
                };
                nodeFreeHooks->setupContext = [&frontDoor](bool demo, Json &context, std::string &error) {
                    return frontDoor->nodeFreeSetupContext(demo, context, error);
                };
                nodeFreeHooks->historyOverlay = [&frontDoor](int64_t fromMs, int64_t toMs, const Json &ids,
                                                             Json &overlay, std::string &error) {
                    return frontDoor->nodeFreeHistoryOverlay(fromMs, toMs, ids, overlay, error);
                };
            }
            if (nativeMqttProduction && mqttStandingProvider)
                frontDoor->setMqttStatusProvider(mqttStandingProvider);
            if (nativeMqttProduction)
                frontDoor->setMqttForget([&mqttShadowClient]() -> size_t {
                    // Empty retained messages remove the entities from Home
                    // Assistant, and clear the state the broker keeps. The client
                    // has no publish acknowledgement, so this waits a moment for
                    // the messages to leave; the clean disconnect then stops the
                    // broker publishing the will after.
                    if (!mqttShadowClient || !mqttShadowClient->connected())
                        return 0;
                    const std::vector<std::string> topics = mqttShadowClient->retainedTopics();
                    for (const std::string &topic : topics)
                        (void)mqttShadowClient->publish(topic, "", true);
                    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
                    mqttShadowClient->stop();
                    return topics.size();
                });
            tvAppManager.reset(new TvAppManager(children, assetsRoot));
            frontDoor->setTvAppManager(tvAppManager.get());
            // Only a forwarder that exists has a status to report, and the
            // Server tab's NTP save waits only on a client that is running.
            frontDoor->setStatusProviders(
                ntpClient ? std::function<Json()>([&ntpClient]() { return ntpClient->status(); })
                          : std::function<Json()>(),
                syslogForwarder
                    ? std::function<Json()>([&syslogForwarder]() { return syslogForwarder->status(); })
                    : std::function<Json()>());

            if (nativeRemoteInputConfigured && nativeStateConfigured &&
                !service::hasJavaScriptRemoteKeys(systemRoot)) {
                nativeRemoteKeysReconciler.reset(new RemoteKeysReconciler(
                    loop, luna,
                    [&frontDoor](bool &wrote, std::string &error) {
                        if (!frontDoor) {
                            error = "native HTTP front door is unavailable";
                            return false;
                        }
                        return frontDoor->reconcileNativeRemoteKeys(wrote, error);
                    }, &logger));
            }

            if (nativeMqttDiscovery && mqttShadowClient && mqttShadowTopics &&
                mqttShadowTelemetryPublisher) {
                mqttShadowCommandDispatcher.reset(new MqttCommandDispatcher(
                    loop, *frontDoor, *mqttShadowTelemetryPublisher,
                    *mqttShadowTopics, logger));
                if (!mqttShadowClient->subscribe(
                        mqttShadowTopics->commandPrefix() + "#", 0))
                    logger.warning("native MQTT: could not subscribe to command topics");
            }

            HttpServerOptions httpOptions;
            httpOptions.host = loaded.config.host;
            httpOptions.port = static_cast<unsigned short>(loaded.config.port);
            httpOptions.workerCount = 32;
            if (!httpServer.start(
                    httpOptions,
                    [&frontDoor](HttpExchange &exchange) { frontDoor->handle(exchange); },
                    proxyError))
                throw std::runtime_error("could not start HTTP front door: " + proxyError);
            HttpServerOptions handoffOptions;
            handoffOptions.host = loaded.config.host;
            handoffOptions.port = publicHandoffPort;
            handoffOptions.workerCount = 8;
            if (!phoneHandoffServer.start(
                    handoffOptions,
                    [&frontDoor](HttpExchange &exchange) {
                        frontDoor->handlePhoneHandoff(exchange);
                    },
                    proxyError))
                throw std::runtime_error("could not start phone setup listener: " + proxyError);
            if (iconServerEnabled) {
                HttpServerOptions iconOptions;
                iconOptions.host = loaded.config.host == "0.0.0.0" ? "0.0.0.0" : "::";
                iconOptions.port = iconPort;
                iconOptions.workerCount = 4;
                if (!iconHttpServer.start(
                        iconOptions,
                        [&frontDoor](HttpExchange &exchange) {
                            frontDoor->handleLanIcon(exchange);
                        },
                        proxyError))
                    throw std::runtime_error("could not start app icon listener: " + proxyError);
            }
            std::string clientConfigError;
            if (!writeClientServerPort(
                    clientServerConfigPathForDataRoot(dataDirectory()),
                    static_cast<unsigned>(loaded.config.port), &clientConfigError))
                logger.warning("native client port config could not be published: " +
                               clientConfigError);
            dailyBackupScheduler.reset(new DailyBackupScheduler(
                loop,
                [&frontDoor](std::string &error) {
                    return frontDoor && frontDoor->createDailyBackupIfDue(error);
                },
                &logger));
            if (!dailyBackupScheduler->start())
                throw std::runtime_error("could not schedule native daily backups");
            if (httpServer.usingIpv4Fallback())
                logger.warning("HTTP dual-stack listener unavailable; using IPv4-only fallback");

            // The server directory holds assets/ and lib/; files an earlier
            // release installed and this one drops are removed at every start.
            const size_t retiredGone = removeRetiredFiles(
                assetsRoot.substr(0, assetsRoot.find_last_of('/')));
            if (retiredGone)
                logger.info("server: removed " + std::to_string(retiredGone) +
                            " file(s) an earlier release installed");
            if (removeRetiredApps((systemRoot == "/" ? std::string() : systemRoot) +
                                  "/media/developer/apps/usr/palm"))
                logger.info("apps: removed the developer client com.tvweb.native an earlier release staged");

            // The launcher apps carry the dashboard port in their page; an
            // update or a changed "port" leaves it out of step, so it is
            // rewritten at every start (as apps.js syncLauncherPort did).
            for (const std::string &id : syncLauncherPort(
                     loaded.config.port,
                     (systemRoot == "/" ? std::string() : systemRoot) +
                         "/media/developer/apps/usr/palm/applications"))
                logger.info("apps: " + id + " now opens the dashboard on port " +
                            std::to_string(loaded.config.port));

            // The Homebrew Channel app is the tile, so the one deploy.sh added
            // goes; boot is the moment, as nothing is open and it is never
            // removed while open. Otherwise the tile's packaged copy is
            // brought up to date with this release; a removed app stays removed.
            // A saver an earlier run launched may still be showing; only this
            // process can close it.
            (void)loop.submitBlocking([&frontDoor]() {
                if (frontDoor) frontDoor->runStartupTasks();
            });
            // What home.js did at start: what earlier builds left, taking LG's
            // Home's place, the starting notice. Off until validated on a TV (Node
            // does it otherwise).
            (void)loop.submitBlocking([&frontDoor]() {
                HomeOrchestrator *orchestrator = frontDoor ? frontDoor->homeOrchestrator() : nullptr;
                if (orchestrator) orchestrator->start();
            });
            // What services.js does besides the toggle: retire old entries, bind
            // the stubs of the services switched off, and stop one that came
            // back. Off until validated on a TV (Node does it otherwise).
            const char *nativeServices = std::getenv("TVWEB_NATIVE_SERVICES");
            if (nativeServices && std::string(nativeServices) == "1") {
                ServicesKeeperDeps keeperDeps;
                keeperDeps.systemRoot = systemRoot;
                keeperDeps.dataRoot = dataDirectory();
                keeperDeps.procRoot = procRoot;
                keeperDeps.run = [&children](const std::string &program,
                                             const std::vector<std::string> &arguments) {
                    ChildOptions options;
                    options.arguments.push_back(program);
                    options.arguments.insert(options.arguments.end(), arguments.begin(),
                                             arguments.end());
                    options.timeout = std::chrono::milliseconds(12000);
                    options.maximumOutputBytes = 16 * 1024;
                    return children.run(options).succeeded();
                };
                keeperDeps.bindStubs = [&frontDoor](const service::BackgroundServiceInfo &info,
                                                    const std::string &id) {
                    std::string error;
                    return frontDoor && frontDoor->setServiceStubs(info, id, true, error);
                };
                keeperDeps.terminate = [](int pid) { (void)kill(pid, SIGTERM); };
                keeperDeps.later = [&loop](std::chrono::milliseconds delay,
                                           std::function<void()> task) {
                    (void)loop.scheduleAfter(delay, std::move(task));
                };
                keeperDeps.log = [&logger](const std::string &message) { logger.info(message); };
                servicesKeeper.reset(new ServicesKeeper(std::move(keeperDeps)));
                (void)loop.submitBlocking([&servicesKeeper]() { (void)servicesKeeper->start(); });
                const auto enforce = [&loop, &servicesKeeper]() {
                    (void)loop.submitBlocking([&servicesKeeper]() { (void)servicesKeeper->enforce(); });
                };
                (void)loop.scheduleAfter(std::chrono::milliseconds(30000), enforce);
                (void)loop.scheduleEvery(std::chrono::minutes(5), enforce);
            }
            (void)loop.submitBlocking([&tvAppManager, &logger, fromHbc = access(
                (dataDirectory() + "/.from-homebrew-channel").c_str(), F_OK) == 0]() {
                if (!tvAppManager) return;
                const Json result = fromHbc ? tvAppManager->run("retire")
                                            : tvAppManager->refreshInstalled();
                if (fromHbc && result["retired"].boolean())
                    logger.info("tvapp: old tile removed; Homebrew Channel replaces it");
                else if (!fromHbc && result["refreshed"].boolean() && result["ok"].boolean())
                    logger.info("tvapp: refreshed to current release");
            });

            const char *nativeStateEnabled = std::getenv("TVWEB_NATIVE_STATE");
            if (nativeStateEnabled && std::string(nativeStateEnabled) == "1") {
                if (!loop.post([&loop, &luna, &stateSubscriptions, &historyRecorder, &remoteBattery,
                                &screenTimeEnforcer, &mqttShadowClient,
                                &mqttShadowAvailability,
                                &nativeRemoteKeysReconciler,
                                &mqttShadowTopics, &mqttShadowPublisher,
                                &mqttShadowTelemetryPublisher,
                                &remoteButtonsReader, &mqttShadowCommandDispatcher,
                                &mqttShadowErrorLogged,
                                mqttLastPublishMs, mqttErrorActive,
                                updateNativeMqttStatus,
                                &logger, mqttEnabled, nativeMqttDiscovery,
                                mqttShadowDiscovery,
                                mqttShadowSourceTelemetryTopic,
                                mqttShadowLegacyTelemetryTopic]() {
                        std::unique_ptr<StateSubscriptions> *subscriptionsRef =
                            &stateSubscriptions;
                        std::unique_ptr<RemoteBattery> *batteryRef = &remoteBattery;
                        std::unique_ptr<service::HistoryRecorder> *historyRef =
                            &historyRecorder;
                        std::unique_ptr<ScreenTimeEnforcer> *screenTimeRef =
                            &screenTimeEnforcer;
                        std::unique_ptr<RemoteKeysReconciler> *remoteKeysRef =
                            &nativeRemoteKeysReconciler;
                        Logger *loggerRef = &logger;
                        const std::shared_ptr<bool> stateGroupsStarted =
                            std::make_shared<bool>(false);
                        const std::shared_ptr<std::function<void()>> startStateGroups =
                            std::make_shared<std::function<void()>>();
                        *startStateGroups = [subscriptionsRef, batteryRef, historyRef,
                                             screenTimeRef, remoteKeysRef, loggerRef, stateGroupsStarted,
                                             mqttEnabled]() {
                            if (*stateGroupsStarted)
                                return;
                            *stateGroupsStarted = true;
                            if (!*subscriptionsRef || !(*subscriptionsRef)->start())
                                return;
                            if (*batteryRef && !(*batteryRef)->start())
                                loggerRef->warning(
                                    "remote battery: could not start native poller");
                            if (*historyRef)
                                (*historyRef)->start((*subscriptionsRef)->snapshot());
                            if (*screenTimeRef)
                                (void)(*screenTimeRef)->start();
                            if (*remoteKeysRef && !(*remoteKeysRef)->start())
                                loggerRef->warning(
                                    "remotekeys: native reapplication manager could not start");
                            if (mqttEnabled) {
                                (void)(*subscriptionsRef)->startGroup("surface");
                                (void)(*subscriptionsRef)->startGroup("focus");
                            }
                        };
                        if (nativeMqttDiscovery && mqttShadowDiscovery) {
                            mqttShadowDiscovery->pendingInputs = 10;
                            const auto finishInput = [mqttShadowDiscovery,
                                                      &mqttShadowClient, &logger,
                                                      startStateGroups](
                                                          const std::string &label) {
                                if (mqttShadowDiscovery->pendingInputs > 0)
                                    --mqttShadowDiscovery->pendingInputs;
                                logger.debug("native MQTT: read " + label);
                                if (mqttShadowDiscovery->pendingInputs == 0) {
                                    if (!mqttShadowDiscovery->collectorInputs) {
                                        mqttShadowDiscovery->options.soundOutputs =
                                            service::offeredSoundOutputs(
                                                mqttShadowDiscovery->soundOutputValues,
                                                mqttShadowDiscovery->soundOutputConfigs,
                                                mqttShadowDiscovery->currentSoundOutput);
                                        logger.debug("native MQTT: read " +
                                            std::to_string(mqttShadowDiscovery->options.soundOutputs.size()) +
                                            " available sound outputs");
                                    }
                                    (*startStateGroups)();
                                }
                                publishMqttShadowDiscoveryIfReady(
                                    mqttShadowDiscovery, mqttShadowClient.get(), &logger);
                            };
                            const auto completeApps = [mqttShadowDiscovery,
                                                       finishInput](const Json &apps) {
                                mqttShadowDiscovery->options.installedApps = apps;
                                finishInput("" + std::to_string(apps.size()) +
                                            " installed apps");
                            };
                            const bool appsQueued = luna.call(
                                "com.webos.applicationManager/listApps", Json::object(),
                                [&luna, completeApps](const LunaReply &reply) {
                                    const Json apps = mqttDiscoveryInstalledApps(reply.response);
                                    if (apps.size() == 0) {
                                        if (luna.call(
                                                "com.webos.applicationManager/listLaunchPoints",
                                                Json::object(),
                                                [completeApps](const LunaReply &fallback) {
                                                    completeApps(mqttDiscoveryInstalledApps(
                                                        fallback.response));
                                                }))
                                            return;
                                    }
                                    completeApps(apps);
                                });
                            if (!appsQueued) {
                                logger.warning(
                                    "native MQTT: installed-app read was not queued");
                                completeApps(Json::array());
                            }

                            Json pictureRequest = Json::object();
                            pictureRequest.set("category", Json::text("picture"));
                            pictureRequest.set("key", Json::text("pictureMode"));
                            const bool pictureQueued = luna.call(
                                "com.webos.service.settings/getSystemSettingValues",
                                pictureRequest,
                                [mqttShadowDiscovery, finishInput](const LunaReply &reply) {
                                    if (!mqttShadowDiscovery->collectorInputs)
                                        mqttShadowDiscovery->options.pictureModes =
                                            mqttDiscoveryPictureModes(reply.response);
                                    finishInput("" + std::to_string(
                                        mqttShadowDiscovery->options.pictureModes.size()) +
                                        " picture modes");
                                });
                            if (!pictureQueued) {
                                logger.warning(
                                    "native MQTT: picture-mode read was not queued");
                                mqttShadowDiscovery->options.pictureModes = Json::array();
                                finishInput("picture modes unavailable");
                            }

                            Json soundValuesRequest = Json::object();
                            soundValuesRequest.set("category", Json::text("sound"));
                            soundValuesRequest.set("key", Json::text("soundOutput"));
                            const bool soundValuesQueued = luna.call(
                                "com.webos.service.settings/getSystemSettingValues",
                                soundValuesRequest,
                                [mqttShadowDiscovery, finishInput](const LunaReply &reply) {
                                    const Json values = reply.response["values"]["arrayExt"];
                                    mqttShadowDiscovery->soundOutputValues = values.isArray()
                                        ? values : Json::array();
                                    finishInput("sound output choices");
                                });
                            if (!soundValuesQueued) {
                                logger.warning(
                                    "native MQTT: sound output choices were not queued");
                                mqttShadowDiscovery->soundOutputValues = Json::array();
                                finishInput("sound output choices unavailable");
                            }

                            Json soundConfigRequest = Json::object();
                            Json soundConfigNames = Json::array();
                            for (const std::string &name : service::soundOutputConfigNames())
                                soundConfigNames.add(Json::text(name));
                            soundConfigRequest.set("configNames", soundConfigNames);
                            const bool soundConfigQueued = luna.call(
                                "com.webos.service.config/getConfigs", soundConfigRequest,
                                [mqttShadowDiscovery, finishInput](const LunaReply &reply) {
                                    const Json configs = reply.response["configs"];
                                    mqttShadowDiscovery->soundOutputConfigs =
                                        configs.isObject() ? configs : Json::object();
                                    finishInput("sound output model capabilities");
                                });
                            if (!soundConfigQueued) {
                                logger.warning(
                                    "native MQTT: sound output model read was not queued");
                                mqttShadowDiscovery->soundOutputConfigs = Json::object();
                                finishInput("sound output model unavailable");
                            }

                            Json currentSoundRequest = Json::object();
                            currentSoundRequest.set("category", Json::text("sound"));
                            const bool currentSoundQueued = luna.call(
                                "com.webos.service.settings/getSystemSettings",
                                currentSoundRequest,
                                [mqttShadowDiscovery, finishInput](const LunaReply &reply) {
                                    mqttShadowDiscovery->currentSoundOutput =
                                        reply.response["settings"]["soundOutput"].string();
                                    finishInput("current sound output");
                                });
                            if (!currentSoundQueued) {
                                logger.warning(
                                    "native MQTT: current sound output read was not queued");
                                mqttShadowDiscovery->currentSoundOutput.clear();
                                finishInput("current sound output unavailable");
                            }
                            Json propertiesRequest = Json::object();
                            Json propertyKeys = Json::array();
                            propertyKeys.add(Json::text("tv.model.logoLight"));
                            propertyKeys.add(Json::text("modelName"));
                            propertiesRequest.set("keys", propertyKeys);
                            const bool propertiesQueued = luna.call(
                                "com.webos.service.tv.systemproperty/getSystemProperties",
                                propertiesRequest,
                                [mqttShadowDiscovery, finishInput](const LunaReply &reply) {
                                    const Json properties = reply.response;
                                    const Json logo = properties["tv.model.logoLight"];
                                    mqttShadowDiscovery->probedCapabilities.set(
                                        "hasLogoLight",
                                        Json::flag(logo.isBoolean() && logo.boolean()));
                                    if (properties["modelName"].isString())
                                        mqttShadowDiscovery->detectedModel =
                                            properties["modelName"].string();
                                    finishInput("logo-light and model capabilities");
                                });
                            if (!propertiesQueued) {
                                mqttShadowDiscovery->probedCapabilities.set(
                                    "hasLogoLight", Json::flag(false));
                                finishInput("logo-light and model capabilities unavailable");
                            }

                            const bool cellQueued = luna.call(
                                "com.webos.service.panelcontroller/getOledCellInfo",
                                Json::object(),
                                [mqttShadowDiscovery, finishInput](const LunaReply &reply) {
                                    mqttShadowDiscovery->probedCapabilities.set(
                                        "hasCell", Json::flag(
                                            reply.response["cellInfo"].valid()));
                                    finishInput("OLED cell capability");
                                });
                            if (!cellQueued) {
                                mqttShadowDiscovery->probedCapabilities.set(
                                    "hasCell", Json::flag(false));
                                finishInput("OLED cell capability unavailable");
                            }

                            Json timerRequest = Json::object();
                            timerRequest.set("category", Json::text("time"));
                            Json timerKeys = Json::array();
                            for (const char *key : {"onTimerEnable", "onTimerHour",
                                                    "offTimerEnable", "offTimerHour"})
                                timerKeys.add(Json::text(key));
                            timerRequest.set("keys", timerKeys);
                            const bool timerQueued = luna.call(
                                "com.webos.service.settings/getSystemSettings",
                                timerRequest,
                                [mqttShadowDiscovery, finishInput](const LunaReply &reply) {
                                    const Json settings = reply.response["settings"];
                                    if (reply.response["returnValue"].boolean()) {
                                        mqttShadowDiscovery->probedCapabilities.set(
                                            "hasOnTimer", Json::flag(
                                                settings.has("onTimerEnable") &&
                                                settings.has("onTimerHour")));
                                        mqttShadowDiscovery->probedCapabilities.set(
                                            "hasOffTimer", Json::flag(
                                                settings.has("offTimerEnable") &&
                                                settings.has("offTimerHour")));
                                    }
                                    finishInput("power-timer capabilities");
                                });
                            if (!timerQueued)
                                finishInput("power-timer capabilities unavailable");

                            const bool lightSensorQueued = luna.call(
                                "com.webos.service.tv.display/getLightSensorData",
                                Json::object(),
                                [mqttShadowDiscovery, finishInput](const LunaReply &reply) {
                                    bool present = false;
                                    for (const Json &sensor :
                                         reply.response["sensorData"].items()) {
                                        const std::string property =
                                            sensor["property"].string();
                                        const Json value = sensor["value"];
                                        if ((property == "visibleLuminance" ||
                                             property == "luminance") && value.valid() &&
                                            !(value.isNumber() && value.number() == 65535)) {
                                            present = true;
                                            break;
                                        }
                                    }
                                    mqttShadowDiscovery->probedCapabilities.set(
                                        "hasLightSensor", Json::flag(present));
                                    finishInput("ambient-light capability");
                                });
                            if (!lightSensorQueued) {
                                mqttShadowDiscovery->probedCapabilities.set(
                                    "hasLightSensor", Json::flag(false));
                                finishInput("ambient-light capability unavailable");
                            }

                            const bool soundFormatQueued = luna.call(
                                "com.webos.audio/tv/getAudioInputProperty",
                                Json::object(),
                                [mqttShadowDiscovery, finishInput](const LunaReply &reply) {
                                    const Json returned = reply.response["returnValue"];
                                    const std::string error =
                                        reply.response["errorText"].string();
                                    if (returned.isBoolean() && returned.boolean())
                                        mqttShadowDiscovery->probedCapabilities.set(
                                            "hasSoundFormat", Json::flag(true));
                                    else if (returned.isBoolean() && !returned.boolean() &&
                                             error.find("Unknown method") !=
                                                 std::string::npos)
                                        mqttShadowDiscovery->probedCapabilities.set(
                                            "hasSoundFormat", Json::flag(false));
                                    finishInput("sound-format capability");
                                });
                            if (!soundFormatQueued)
                                finishInput("sound-format capability unavailable");
                        } else {
                            (*startStateGroups)();
                        }
                        if (mqttShadowPublisher && mqttShadowClient && mqttShadowTopics) {
                            if (!mqttShadowPublisher->start()) {
                                logger.warning("native MQTT: state publisher could not start");
                            } else {
                                if (mqttShadowTelemetryPublisher &&
                                    !mqttShadowTelemetryPublisher->start())
                                    logger.warning(
                                        "native MQTT: telemetry publisher could not start");
                                MqttClient::MessageHandler shadowMessageHandler;
                                if (nativeMqttDiscovery) {
                                    shadowMessageHandler = [&mqttShadowClient,
                                        &mqttShadowCommandDispatcher,
                                        &mqttShadowTopics, &logger,
                                        mqttShadowLegacyTelemetryTopic,
                                        mqttShadowSourceTelemetryTopic](
                                            const MqttMessage &message) {
                                        if (!mqttShadowClient)
                                            return;
                                        if (message.topic == mqttShadowSourceTelemetryTopic) {
                                            (void)mqttShadowClient->publish(
                                                mqttShadowLegacyTelemetryTopic,
                                                message.payload, true);
                                            return;
                                        }
                                        if (mqttShadowCommandDispatcher && mqttShadowTopics &&
                                            message.topic.compare(
                                                0, mqttShadowTopics->commandPrefix().size(),
                                                mqttShadowTopics->commandPrefix()) == 0 &&
                                            !mqttShadowCommandDispatcher->enqueue(message))
                                            logger.warning(
                                                "native MQTT: command queue is full or the message is invalid");
                                    };
                                }
                                mqttShadowClient->setHandlers(
                                    std::move(shadowMessageHandler),
                                    [&loop, &mqttShadowClient, &mqttShadowTopics,
                                     &mqttShadowPublisher, &mqttShadowTelemetryPublisher,
                                     &mqttShadowAvailability,
                                     &mqttShadowErrorLogged,
                                     mqttErrorActive,
                                     updateNativeMqttStatus,
                                     &logger, nativeMqttDiscovery,
                                     mqttShadowDiscovery]() {
                                        mqttErrorActive->store(false,
                                            std::memory_order_release);
                                        mqttShadowErrorLogged->store(false,
                                            std::memory_order_release);
                                        if (!loop.post([&mqttShadowClient, &mqttShadowTopics,
                                                        &mqttShadowPublisher,
                                                        &mqttShadowTelemetryPublisher, &logger,
                                                        &mqttShadowAvailability,
                                                        updateNativeMqttStatus,
                                                        nativeMqttDiscovery,
                                                        mqttShadowDiscovery]() {
                                                if (!mqttShadowClient || !mqttShadowTopics ||
                                                    !mqttShadowPublisher ||
                                                    !mqttShadowTelemetryPublisher ||
                                                    !mqttShadowAvailability)
                                                    return;
                                                if (mqttShadowDiscovery)
                                                    mqttShadowDiscovery->published = false;
                                                mqttShadowAvailability->onConnected();
                                                updateNativeMqttStatus("connected", "");
                                                mqttShadowPublisher->publishSnapshot();
                                                mqttShadowTelemetryPublisher->setConnected(true);
                                                if (nativeMqttDiscovery)
                                                    publishMqttShadowDiscoveryIfReady(
                                                        mqttShadowDiscovery,
                                                        mqttShadowClient.get(), &logger);
                                            }))
                                            logger.warning("native MQTT: could not queue connect snapshot");
                                    },
                                    [&mqttShadowTelemetryPublisher, mqttErrorActive,
                                     updateNativeMqttStatus]() {
                                        if (mqttShadowTelemetryPublisher)
                                            mqttShadowTelemetryPublisher->setConnected(false);
                                        if (!mqttErrorActive->load(std::memory_order_acquire))
                                            updateNativeMqttStatus(
                                                "connecting", "connection dropped, retrying");
                                    },
                                    [&logger, &mqttShadowErrorLogged, mqttErrorActive,
                                     updateNativeMqttStatus](const std::string &error) {
                                        mqttErrorActive->store(true, std::memory_order_release);
                                        updateNativeMqttStatus("error", error);
                                        if (!mqttShadowErrorLogged->exchange(
                                                true, std::memory_order_acq_rel))
                                            logger.warning("native MQTT: " + error);
                                    });
                                updateNativeMqttStatus("connecting", "");
                                if (!mqttShadowClient->start()) {
                                    mqttErrorActive->store(true, std::memory_order_release);
                                    updateNativeMqttStatus("error", "client could not start");
                                    logger.warning("native MQTT: client could not start");
                                } else if (remoteButtonsReader &&
                                         !remoteButtonsReader->start())
                                    logger.warning("native MQTT: remote button reader unavailable");
                            }
                        }
                    }))
                    throw std::runtime_error("could not schedule native state subscriptions");
            }
            if (nativeRemoteInputConfigured && nativeStateConfigured &&
                remoteButtonsReader && !remoteButtonsReader->start())
                logger.warning("native remote input: remote button reader unavailable");
        }
        bool shutdownStarted = false;
        const EventLoop::TimerId signalPoll =
            loop.scheduleEvery(EventLoop::Milliseconds(250),
            [&loop, &stateSubscriptions, &historyRecorder, &screenTimeEnforcer,
             &remoteBattery, &gameFrameRate, &adBlockManager,
             &dailyBackupScheduler, &mqttShadowClient, &mqttShadowPublisher,
             &mqttShadowTelemetryPublisher, &remoteButtonsReader,
             &remoteHomeHoldTimer,
             &remoteButtonLogWatcher,
             &nativeRemoteKeysReconciler,
             &mqttShadowCommandDispatcher, &shutdownStarted, &logger,
             supervised, supervisorPid]() {
                if (shutdownRequested || (supervised && getppid() != supervisorPid)) {
                    if (!shutdownStarted) {
                        shutdownStarted = true;
                        if (remoteButtonsReader)
                            remoteButtonsReader->stop();
                        if (remoteHomeHoldTimer)
                            remoteHomeHoldTimer->stop();
                        if (remoteButtonLogWatcher)
                            remoteButtonLogWatcher->stop();
                        if (nativeRemoteKeysReconciler)
                            nativeRemoteKeysReconciler->stop();
                        if (mqttShadowClient)
                            mqttShadowClient->stop();
                        if (mqttShadowCommandDispatcher)
                            mqttShadowCommandDispatcher->stop();
                    }
                    // A current command can be waiting for a Luna completion
                    // on this loop. Let it finish before the event loop drains.
                    if (mqttShadowCommandDispatcher &&
                        !mqttShadowCommandDispatcher->idle())
                        return;
                    // Cancel Luna children on the loop thread before stopping
                    // the loop; its worker pool drains those children as part
                    // of orderly shutdown.
                    if (screenTimeEnforcer)
                        screenTimeEnforcer->stop();
                    if (remoteBattery)
                        remoteBattery->stop();
                    if (gameFrameRate)
                        gameFrameRate->stop();
                    if (adBlockManager)
                        adBlockManager->stop();
                    if (dailyBackupScheduler)
                        dailyBackupScheduler->stop();
                    if (mqttShadowPublisher)
                        mqttShadowPublisher->stop();
                    if (mqttShadowTelemetryPublisher)
                        mqttShadowTelemetryPublisher->stop();
                    if (stateSubscriptions)
                        stateSubscriptions->stop();
                    if (historyRecorder) {
                        std::string error;
                        historyRecorder->stop(&error);
                        if (!error.empty()) logger.error("history: " + error);
                    }
                    loop.stop();
                }
            });
        const EventLoop::TimerId historyFlush = historyRecorder
            ? loop.scheduleEvery(EventLoop::Milliseconds(600000), [&historyRecorder, &logger]() {
                  if (!historyRecorder) return;
                  std::string error;
                  if (!historyRecorder->flush(&error) && !error.empty())
                      logger.error("history: " + error);
              })
            : 0;
        if (!signalPoll || (historyRecorder && !historyFlush) || !heartbeat.start())
            throw std::runtime_error("could not initialize daemon timers");

        logger.info(std::string("starting resident runtime; config=") + loaded.path +
                    ", port=" + std::to_string(loaded.config.port) +
                    ", workers=" + std::to_string(workerCount) +
                    (listen ? ", HTTP front door=active" : ", HTTP front door=disabled") +
                    (supervised ? ", parent supervision=active" : "") +
                    (listen ? ", phone handoff port=" +
                                  std::to_string(phoneHandoffServer.boundPort())
                            : "") +
                    ", HTTP engine=CivetWeb/" + HttpServer::libraryVersion());
        loop.run();
        if (mqttShadowCommandDispatcher)
            mqttShadowCommandDispatcher->join();
        if (remoteButtonsReader)
            remoteButtonsReader->stop();
        if (remoteHomeHoldTimer)
            remoteHomeHoldTimer->stop();
        if (remoteButtonLogWatcher)
            remoteButtonLogWatcher->stop();
        if (mqttShadowClient)
            mqttShadowClient->stop();
        if (mqttShadowPublisher)
            mqttShadowPublisher->stop();
        if (mqttShadowTelemetryPublisher)
            mqttShadowTelemetryPublisher->stop();
        if (ntpClient)
            ntpClient->stop();
        if (syslogForwarder)
            syslogForwarder->stop();
        iconHttpServer.stop();
        phoneHandoffServer.stop();
        httpServer.stop();
        heartbeat.stop();
        if (heartbeat.failed()) {
            logger.error("heartbeat write failed; stopping daemon");
            return 1;
        }
        logger.info("resident runtime stopped");
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "Native Tv daemon: %s\n", error.what());
        return 2;
    }
}
