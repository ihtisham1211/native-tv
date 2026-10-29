#include "daemon/remote_battery.hpp"
#include "daemon/json_file.hpp"
#include "daemon/logger.hpp"
#include "daemon/luna.hpp"
#include "service/kernel_files.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <regex>
#include <utility>

namespace nativetv {
namespace daemon {
namespace {
const char *kMrcu = "com.webos.service.mrcu/";
const char *kBluetooth = "com.webos.service.bluetooth2/";
const char *kBatteryService = "0000180f-0000-1000-8000-00805f9b34fb";
const char *kBatteryLevel = "00002a19-0000-1000-8000-00805f9b34fb";
const char *kInvalidAddress = "00:00:00:00:00:00";

std::string rooted(const std::string &root, const std::string &path) {
    return root == "/" ? path : root + path;
}

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::string webosVersion(const std::string &root) {
    std::string issue;
    service::readKernelFile(rooted(root, "/etc/issue"), issue);
    if (issue.empty())
        service::readKernelFile(rooted(root, "/etc/issue.net"), issue);
    std::smatch match;
    if (std::regex_search(issue, match, std::regex("webOS(?: TV)?\\s+([0-9]+(?:\\.[0-9]+)*)",
                                                   std::regex::icase)))
        return match[1].str();
    std::string starfish;
    service::readKernelFile(rooted(root, "/etc/starfish-release"), starfish);
    if (std::regex_search(starfish, match, std::regex("release\\s+([0-9]+(?:\\.[0-9]+)*)",
                                                       std::regex::icase)))
        return match[1].str();
    return "";
}

bool validLevel(const Json &value, int &level) {
    if (!value.isNumber() || value.number() < 0 || value.number() > 100 ||
        static_cast<int>(value.number()) != value.number())
        return false;
    level = static_cast<int>(value.number());
    return true;
}

std::string stringValue(const Json &value) {
    if (value.isString())
        return value.string();
    if (value.isNumber())
        return value.dump();
    return "";
}

std::string remoteAddress(const Json &devices) {
    for (const Json &device : devices.items()) {
        const std::string name = device["name"].string();
        if (device["paired"].boolean() && device["typeOfDevice"].string() == "ble" &&
            name.size() >= 6 && lowercase(name.substr(0, 6)) == "lge mr") {
            const std::string address = device["address"].string();
            if (!address.empty() && address != kInvalidAddress)
                return address;
        }
    }
    return "";
}

long long wallTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
} // namespace

RemoteBattery::RemoteBattery(EventLoop &loop, LunaClient &luna, std::string dataRoot,
                             std::string systemRoot, Logger *logger,
                             RemoteBatteryTimings timings)
    : loop_(loop), luna_(luna), dataRoot_(std::move(dataRoot)),
      systemRoot_(std::move(systemRoot)), logger_(logger), timings_(timings),
      file_(new JsonFile(dataRoot_ + "/remote-battery.json")) {
    const std::string version = webosVersion(systemRoot_);
    gattSupported_ = std::regex_search(version, std::regex("^(9|10)\\."));
    const JsonFileResult saved = file_->load();
    if (!saved.found || !saved.value.isObject())
        return;
    hasLast_ = validLevel(saved.value["level"], lastLevel_);
    lastAt_ = static_cast<long long>(saved.value["at"].number());
    lastMac_ = saved.value["mac"].string();
    if (saved.value["lowStrength"].isBoolean()) {
        hasLowStrength_ = true;
        lowStrength_ = saved.value["lowStrength"].boolean();
    }
    firmware_ = saved.value["firmware"].string();
    model_ = saved.value["model"].string();
}

RemoteBattery::~RemoteBattery() { stop(); }

bool RemoteBattery::start() {
    if (!loop_.isLoopThread())
        return false;
    if (started_)
        return true;
    started_ = true;
    note("remote battery: native poller started");
    poweredOn_ = false;
    connected_ = false;
    isBle_ = false;
    due_ = true;
    busy_ = false;
    persist();

    LunaSubscriptionHandlers handlers;
    handlers.message = [this](const Json &status) { onConnection(status); };
    handlers.error = [this](const std::string &error) {
        if (started_)
            note("remote battery: connection subscription: " + error);
    };
    handlers.close = [this](int code, int signal) {
        if (started_ && (code || signal))
            note("remote battery: connection subscription closed (" +
                 std::to_string(code) + ", " + std::to_string(signal) + ")");
    };
    Json subscribe = Json::object();
    subscribe.set("subscribe", Json::flag(true));
    connectionSubscription_ = luna_.subscribe(
        std::string(kMrcu) + "getConnectionStatus", subscribe, "", std::move(handlers));
    if (!connectionSubscription_ || !connectionSubscription_->start())
        note("remote battery: could not start connection subscription");

    checkTimer_ = loop_.scheduleEvery(timings_.check, [this]() {
        if (started_ && poweredOn_ && due_)
            readOnLoop();
    });
    if (!checkTimer_)
        note("remote battery: could not schedule periodic check");

    Json payload = Json::object();
    payload.set("subscribe", Json::flag(false));
    payload.set("deviceName", Json::text("MagicRemote_1"));
    if (!luna_.call(std::string(kMrcu) + "getDeviceInfo", payload,
                    [this](const LunaReply &reply) {
                        if (started_)
                            onDeviceInfo(reply.response, true);
                    }))
        note("remote battery: device info request could not be queued");
    return true;
}

void RemoteBattery::stop() {
    if (!started_)
        return;
    started_ = false;
    if (checkTimer_) { (void)loop_.cancelTimer(checkTimer_); checkTimer_ = 0; }
    if (settleTimer_) { (void)loop_.cancelTimer(settleTimer_); settleTimer_ = 0; }
    if (discoverTimer_) { (void)loop_.cancelTimer(discoverTimer_); discoverTimer_ = 0; }
    if (dueTimer_) { (void)loop_.cancelTimer(dueTimer_); dueTimer_ = 0; }
    if (connectionSubscription_) {
        connectionSubscription_->stop();
        connectionSubscription_.reset();
    }
    if (!clientId_.empty()) {
        Json payload = Json::object();
        payload.set("clientId", Json::text(clientId_));
        (void)luna_.call(std::string(kBluetooth) + "gatt/disconnect", payload,
                         [](const LunaReply &) {});
        clientId_.clear();
    }
    busy_ = false;
}

void RemoteBattery::setPowerOn(bool on) {
    if (!loop_.isLoopThread() || !started_)
        return;
    const bool changed = poweredOn_ != on;
    poweredOn_ = on;
    if (changed && on) {
        due_ = true;
        if (dueTimer_) { (void)loop_.cancelTimer(dueTimer_); dueTimer_ = 0; }
    }
    if (on && connected_ && isBle_ && due_ && !busy_) {
        if (settleTimer_)
            (void)loop_.cancelTimer(settleTimer_);
        settleTimer_ = loop_.scheduleAfter(timings_.settle, [this]() {
            settleTimer_ = 0;
            if (started_ && poweredOn_ && due_)
                readOnLoop();
        });
    }
}

void RemoteBattery::onConnection(const Json &status) {
    if (!started_)
        return;
    if (status["connected"].isBoolean())
        connected_ = status["connected"].boolean();
    if (status["isBleRemocon"].isBoolean())
        isBle_ = status["isBleRemocon"].boolean();
    persist();
    if (connected_ && isBle_ && poweredOn_ && due_ && !busy_) {
        if (settleTimer_)
            (void)loop_.cancelTimer(settleTimer_);
        settleTimer_ = loop_.scheduleAfter(timings_.settle, [this]() {
            settleTimer_ = 0;
            if (started_ && poweredOn_ && due_)
                readOnLoop();
        });
    } else if (!connected_ && settleTimer_) {
        (void)loop_.cancelTimer(settleTimer_);
        settleTimer_ = 0;
    }
}

void RemoteBattery::onDeviceInfo(const Json &info, bool seedBattery) {
    if (info["lowStrength"].isBoolean()) {
        hasLowStrength_ = true;
        lowStrength_ = info["lowStrength"].boolean();
    }
    const std::string firmware = info["fwVer"].string();
    if (!firmware.empty() && firmware != "00.00.00.00.00.00.00.00.00.00.00")
        firmware_ = firmware;
    const std::string model = info["deviceName"].string();
    if (!model.empty())
        model_ = model;
    if (seedBattery && !hasLast_ && info["returnValue"].boolean()) {
        int level = 0;
        if (validLevel(info["batteryLevel"], level)) {
            const std::string address = info["bdAddress"].string();
            if (!address.empty() && address != kInvalidAddress)
                lastMac_ = address;
            hasLast_ = true;
            lastLevel_ = level;
            lastAt_ = wallTimeMs();
        }
    }
    persist();
}

void RemoteBattery::readOnLoop() {
    if (!loop_.isLoopThread() || !started_ || busy_ || !poweredOn_ || !due_)
        return;
    busy_ = true;
    readTried_ = false;
    address_.clear();
    clientId_.clear();
    Json payload = Json::object();
    if (!luna_.call(std::string(kMrcu) + "getConnectionStatus", payload,
                    [this](const LunaReply &reply) {
                        if (!started_)
                            return;
                        const Json status = reply.response;
                        if (status["connected"].isBoolean())
                            connected_ = status["connected"].boolean();
                        // A one-off response can omit the BLE flag even while
                        // the subscription has already confirmed it.
                        if (status["isBleRemocon"].boolean())
                            isBle_ = true;
                        persist();
                        if (!status["connected"].boolean() || !isBle_) {
                            finishRead(false, false, 0);
                            return;
                        }
                        Json infoPayload = Json::object();
                        infoPayload.set("subscribe", Json::flag(false));
                        infoPayload.set("deviceName", Json::text("MagicRemote_1"));
                        if (!luna_.call(std::string(kMrcu) + "getDeviceInfo", infoPayload,
                                        [this](const LunaReply &infoReply) {
                                            if (!started_)
                                                return;
                                            onDeviceInfo(infoReply.response, false);
                                            int level = 0;
                                            if (infoReply.response["returnValue"].boolean() &&
                                                validLevel(infoReply.response["batteryLevel"], level)) {
                                                readTried_ = true;
                                                const std::string address =
                                                    infoReply.response["bdAddress"].string();
                                                if (address_.empty() && !address.empty() &&
                                                    address != kInvalidAddress)
                                                    address_ = address;
                                                finishRead(true, true, level);
                                            } else if (!gattSupported_) {
                                                finishRead(true, false, 0);
                                            } else {
                                                readGatt();
                                            }
                                        }))
                            finishRead(false, false, 0);
                    })) {
        finishRead(false, false, 0);
    }
}

void RemoteBattery::readGatt() {
    if (!started_ || !busy_)
        return;
    if (!luna_.call(std::string(kBluetooth) + "device/getStatus", Json::object(),
                    [this](const LunaReply &reply) {
                        if (!started_)
                            return;
                        address_ = remoteAddress(reply.response["devices"]);
                        if (address_.empty()) {
                            finishRead(false, false, 0);
                            return;
                        }
                        readTried_ = true;
                        Json payload = Json::object();
                        payload.set("address", Json::text(address_));
                        if (!luna_.call(std::string(kBluetooth) + "gatt/connect", payload,
                                        [this](const LunaReply &connectReply) {
                                            if (!started_)
                                                return;
                                            if (!connectReply.response["returnValue"].boolean()) {
                                                finishRead(true, false, 0);
                                                return;
                                            }
                                            clientId_ = stringValue(connectReply.response["clientId"]);
                                            if (clientId_.empty()) {
                                                finishRead(true, false, 0);
                                                return;
                                            }
                                            Json discover = Json::object();
                                            discover.set("address", Json::text(address_));
                                            if (!luna_.call(
                                                    std::string(kBluetooth) + "gatt/discoverServices",
                                                    discover, [this](const LunaReply &discReply) {
                                                        if (!started_)
                                                            return;
                                                        if (discReply.response["returnValue"].isBoolean() &&
                                                            !discReply.response["returnValue"].boolean()) {
                                                            finishRead(true, false, 0);
                                                            return;
                                                        }
                                                        discoverTimer_ = loop_.scheduleAfter(
                                                            timings_.discover, [this]() {
                                                                discoverTimer_ = 0;
                                                                if (!started_)
                                                                    return;
                                                                Json read = Json::object();
                                                                read.set("clientId", Json::text(clientId_));
                                                                read.set("service", Json::text(kBatteryService));
                                                                read.set("characteristic", Json::text(kBatteryLevel));
                                                                if (!luna_.call(
                                                                        std::string(kBluetooth) +
                                                                            "gatt/readCharacteristicValue",
                                                                        read, [this](const LunaReply &readReply) {
                                                                            if (!started_)
                                                                                return;
                                                                            const Json bytes =
                                                                                readReply.response["value"]["value"]["bytes"];
                                                                            int level = 0;
                                                                            if (readReply.response["returnValue"].boolean() &&
                                                                                bytes.isArray() && bytes.size() &&
                                                                                validLevel(bytes[0], level))
                                                                                finishRead(true, true, level);
                                                                            else
                                                                                finishRead(true, false, 0);
                                                                        }))
                                                                    finishRead(true, false, 0);
                                                            });
                                                        if (!discoverTimer_)
                                                            finishRead(true, false, 0);
                                                    }))
                                                finishRead(true, false, 0);
                                        }))
                            finishRead(true, false, 0);
                    }))
        finishRead(false, false, 0);
}

void RemoteBattery::finishRead(bool tried, bool hasLevel, int level) {
    readTried_ = readTried_ || tried;
    hasLevel_ = hasLevel_ || hasLevel;
    if (hasLevel)
        level_ = level;
    if (discoverTimer_) { (void)loop_.cancelTimer(discoverTimer_); discoverTimer_ = 0; }
    if (!clientId_.empty()) {
        const std::string clientId = clientId_;
        clientId_.clear();
        Json payload = Json::object();
        payload.set("clientId", Json::text(clientId));
        if (luna_.call(std::string(kBluetooth) + "gatt/disconnect", payload,
                       [this](const LunaReply &) {
                           if (started_)
                               completeRead(readTried_, hasLevel_, level_);
                       }))
            return;
    }
    completeRead(readTried_, hasLevel_, level_);
}

void RemoteBattery::completeRead(bool tried, bool hasLevel, int level) {
    if (!started_)
        return;
    busy_ = false;
    if (tried)
        scheduleDue(hasLevel ? timings_.success : timings_.retry);
    if (hasLevel) {
        const bool changed = !hasLast_ || lastLevel_ != level;
        hasLast_ = true;
        lastLevel_ = level;
        lastAt_ = wallTimeMs();
        if (!address_.empty())
            lastMac_ = address_;
        if (changed)
            note("remote battery: level " + std::to_string(level) + "%");
    }
    persist();
    readTried_ = false;
    hasLevel_ = false;
    level_ = 0;
}

void RemoteBattery::scheduleDue(EventLoop::Milliseconds delay) {
    if (dueTimer_)
        (void)loop_.cancelTimer(dueTimer_);
    due_ = false;
    dueTimer_ = loop_.scheduleAfter(delay, [this]() {
        dueTimer_ = 0;
        due_ = true;
    });
    if (!dueTimer_) {
        due_ = true;
        note("remote battery: could not schedule next read");
    }
}

void RemoteBattery::persist() {
    if (!file_)
        return;
    Json state = Json::object();
    state.set("level", hasLast_ ? Json::numeric(lastLevel_) : Json());
    state.set("at", lastAt_ > 0 ? Json::numeric(static_cast<double>(lastAt_)) : Json());
    state.set("mac", lastMac_.empty() ? Json() : Json::text(lastMac_));
    state.set("lowStrength", hasLowStrength_ ? Json::flag(lowStrength_) : Json());
    state.set("firmware", firmware_.empty() ? Json() : Json::text(firmware_));
    state.set("model", model_.empty() ? Json() : Json::text(model_));
    state.set("connected", Json::flag(connected_));
    state.set("isBle", Json::flag(isBle_));
    std::string error;
    if (!file_->save(state, &error) && !error.empty())
        note("remote battery: persist: " + error);
}

void RemoteBattery::note(const std::string &message) {
    if (logger_)
        logger_->info(message);
}
} // namespace daemon
} // namespace nativetv
