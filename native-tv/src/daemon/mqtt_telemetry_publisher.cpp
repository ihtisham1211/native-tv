#include "daemon/mqtt_telemetry_publisher.hpp"
#include "domain/names.hpp"
#include "service/system_metrics.hpp"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <limits>
#include <utility>

namespace nativetv {
namespace daemon {
namespace {
Json aggregateTelemetry(const Json &base, const Json &snapshot) {
    Json result = base.isObject() ? base : Json::object();
    const Json application = snapshot["application"];
    for (const char *key : {"app_id", "app", "display_title"}) {
        const Json value = application[key];
        if (value.valid())
            result.set(key, value);
    }

    const Json power = snapshot["power"];
    const Json rawPower = power["state"];
    if (rawPower.valid()) {
        const Json mapped = names::powerState(rawPower);
        Json powerState = result["powerState"].isObject()
            ? result["powerState"] : Json::object();
        powerState.set("raw", rawPower);
        powerState.set("label", mapped["display"]);
        for (const char *key : {"systemOn", "screenOn"}) {
            const Json value = power[key].isBoolean() ? power[key] : mapped[key];
            if (value.isBoolean())
                powerState.set(key, value);
        }
        result.set("powerState", powerState);
        if (powerState["systemOn"].isBoolean())
            result.set("tvOff", Json::flag(!powerState["systemOn"].boolean()));
    }

    Json picture = result["picture"].isObject() ? result["picture"] : Json::object();
    const Json pictureState = snapshot["picture"];
    const Json rawMode = pictureState["pictureMode"];
    if (rawMode.isString()) {
        const Json mode = names::pictureMode(rawMode.string());
        picture.set("mode_raw", rawMode);
        picture.set("mode", mode["display"]);
    }
    for (const char *key : {"backlight", "energySaving", "screenShift",
                            "logoLuminanceAdjust"}) {
        Json value = pictureState[key];
        if (std::string(key) == "backlight" && value.isString()) {
            const std::string rawBacklight = value.string();
            errno = 0;
            char *end = nullptr;
            const long parsed = std::strtol(rawBacklight.c_str(), &end, 10);
            if (end != rawBacklight.c_str() && *end == '\0' && errno == 0)
                value = Json::numeric(static_cast<double>(parsed));
        }
        if (value.valid())
            picture.set(key, value);
    }
    if (picture.keys().size() != 0)
        result.set("picture", picture);

    const Json audio = snapshot["audio"];
    if (audio["volume"].isNumber())
        result.set("volume", audio["volume"]);
    if (audio["muted"].isBoolean())
        result.set("muted", audio["muted"]);
    if (audio["output"].isString()) {
        result.set("audio_output", audio["output"]);
        Json sound = result["sound"].isObject() ? result["sound"] : Json::object();
        sound.set("output", audio["output"]);
        result.set("sound", sound);
    }

    const Json encodedMedia = snapshot["media"]["snapshot"];
    if (encodedMedia.isString()) {
        const Json media = Json::parse(encodedMedia.string());
        if (media.isObject())
            result.set("media", media);
    }
    return result;
}

int64_t monotonicMilliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

int64_t wallMilliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::vector<service::CpuCoreTicks> cpuTicks(const Json &cpu) {
    std::vector<service::CpuCoreTicks> result;
    const Json cpus = cpu["times"]["cpus"];
    for (const std::string &id : cpus.keys()) {
        const Json modes = cpus[id];
        uint64_t total = 0;
        uint64_t idle = 0;
        bool any = false;
        for (const std::string &mode : modes.keys()) {
            const Json value = modes[mode];
            const double raw = value.number(-1);
            if (!value.isNumber() || !std::isfinite(raw) || raw < 0 ||
                raw >= static_cast<double>(std::numeric_limits<uint64_t>::max()))
                continue;
            const uint64_t ticks = static_cast<uint64_t>(raw);
            if (ticks > std::numeric_limits<uint64_t>::max() - total ||
                ((mode == "idle" || mode == "iowait") &&
                 ticks > std::numeric_limits<uint64_t>::max() - idle))
                continue;
            total += ticks;
            if (mode == "idle" || mode == "iowait")
                idle += ticks;
            any = true;
        }
        if (any)
            result.push_back({id, total, idle});
    }
    return result;
}

Json cpuCoreLoads(const Json &cpu, const Json &sample) {
    const Json measured = sample["cores"];
    std::vector<std::string> ids = measured.keys();
    if (!ids.empty()) {
        const auto numericId = [](const std::string &id) {
            char *end = nullptr;
            const unsigned long value = std::strtoul(id.c_str(), &end, 10);
            return end != id.c_str() && *end == '\0' ? value
                : std::numeric_limits<unsigned long>::max();
        };
        std::sort(ids.begin(), ids.end(), [&numericId](const std::string &left,
                                                       const std::string &right) {
            return numericId(left) < numericId(right);
        });
        Json result = Json::array();
        for (const std::string &id : ids)
            result.add(measured[id]);
        return result;
    }
    const Json fallback = cpu["coreLoads"];
    return fallback.isArray() ? fallback : Json::array();
}

void setIfValid(Json &target, const char *key, const Json &source,
                const char *sourceKey = nullptr) {
    const Json value = source[sourceKey ? sourceKey : key];
    if (value.valid())
        target.set(key, value);
}
} // namespace

MqttTelemetryPublisher::MqttTelemetryPublisher(EventLoop &loop, state::Cache &cache,
                                               MqttTopics topics, Publish publish,
                                               MqttTelemetryOptions options,
                                               Logger *logger)
    : loop_(loop), cache_(cache), topics_(std::move(topics)),
      publish_(std::move(publish)), options_(std::move(options)),
      logger_(logger), gate_(std::make_shared<CallbackGate>()) {
    gate_->owner = this;
}

MqttTelemetryPublisher::~MqttTelemetryPublisher() {
    stop();
    std::lock_guard<std::mutex> lock(gate_->mutex);
    gate_->owner = nullptr;
}

bool MqttTelemetryPublisher::start() {
    std::lock_guard<std::mutex> lock(gate_->mutex);
    if (started_)
        return !stopped_;
    if (stopped_)
        return false;
    started_ = true;
    const std::weak_ptr<CallbackGate> weakGate(gate_);
    cache_.onChange([weakGate](const state::Event &event) {
        const std::shared_ptr<CallbackGate> gate = weakGate.lock();
        if (!gate)
            return;
        std::lock_guard<std::mutex> lock(gate->mutex);
        if (gate->owner)
            gate->owner->onChange(event);
    });
    if (!options_.systemRoot.empty() && !options_.dataRoot.empty()) {
        try {
            systemWorker_ = std::thread(&MqttTelemetryPublisher::systemWorkerLoop, this);
        } catch (...) {
            stopped_ = true;
            if (logger_)
                logger_->error("native MQTT telemetry: system worker could not start");
            return false;
        }
    }
    return true;
}

void MqttTelemetryPublisher::stop() {
    {
        std::lock_guard<std::mutex> lock(gate_->mutex);
        if (stopped_)
            return;
        stopped_ = true;
        if (timer_)
            loop_.cancelTimer(timer_);
        if (systemTimer_)
            loop_.cancelTimer(systemTimer_);
        timer_ = 0;
        systemTimer_ = 0;
        systemTimerIntervalMs_ = 0;
    }
    {
        std::lock_guard<std::mutex> lock(systemWorkerMutex_);
        systemWorkerStop_ = true;
        systemWorkerPending_ = false;
    }
    systemWorkerCondition_.notify_all();
    if (systemWorker_.joinable() && systemWorker_.get_id() != std::this_thread::get_id())
        systemWorker_.join();
}

void MqttTelemetryPublisher::setBase(const Json &base) {
    std::lock_guard<std::mutex> lock(gate_->mutex);
    base_ = base.isObject() ? base : Json::object();
    if (started_ && !stopped_)
        schedulePublish();
}

void MqttTelemetryPublisher::setConnected(bool connected) {
    std::lock_guard<std::mutex> lock(gate_->mutex);
    const bool changed = connected_ != connected;
    connected_ = connected;
    if (!changed || !started_ || stopped_)
        return;
    if (!connected) {
        if (systemTimer_)
            (void)loop_.cancelTimer(systemTimer_);
        systemTimer_ = 0;
        systemTimerIntervalMs_ = 0;
        return;
    }
    if (!options_.composeFull)
        publishNow();
    collectSystem();
    scheduleSystemTimer();
}

void MqttTelemetryPublisher::setSystemSnapshot(const Json &system) {
    std::lock_guard<std::mutex> lock(gate_->mutex);
    if (stopped_ || !system.isObject() || !system["ok"].boolean())
        return;
    applySystemSnapshot(system);
    if (connected_)
        publishNow();
}

void MqttTelemetryPublisher::publishSnapshot() {
    std::lock_guard<std::mutex> lock(gate_->mutex);
    if (!started_ || stopped_ || !publish_)
        return;
    publishNow();
}

void MqttTelemetryPublisher::onChange(const state::Event &event) {
    if (!started_ || stopped_)
        return;
    if (event.group == "power" && event.key == "systemOn") {
        if (!options_.composeFull)
            publishNow();
        collectSystem();
        scheduleSystemTimer();
    } else {
        schedulePublish();
    }
}

void MqttTelemetryPublisher::schedulePublish() {
    if (!started_ || stopped_ || !publish_ || timer_)
        return;
    const auto now = std::chrono::steady_clock::now();
    auto delay = std::chrono::milliseconds(300);
    if (lastPublish_ != std::chrono::steady_clock::time_point()) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            lastPublish_ + std::chrono::milliseconds(1500) - now);
        if (remaining > delay)
            delay = remaining;
    }
    const std::weak_ptr<CallbackGate> weakGate(gate_);
    timer_ = loop_.scheduleAfter(delay, [weakGate] {
        const std::shared_ptr<CallbackGate> gate = weakGate.lock();
        if (!gate)
            return;
        std::lock_guard<std::mutex> lock(gate->mutex);
        if (!gate->owner)
            return;
        gate->owner->timer_ = 0;
        if (gate->owner->started_ && !gate->owner->stopped_)
            gate->owner->publishFresh();
    });
    if (!timer_)
        publishFresh();
}

void MqttTelemetryPublisher::scheduleSystemTimer() {
    if (!started_ || stopped_ || !connected_ || options_.systemRoot.empty() ||
        options_.dataRoot.empty())
        return;
    const Json power = cache_.snapshot()["power"];
    const Json systemOn = power["systemOn"];
    const unsigned interval = systemOn.isBoolean() && !systemOn.boolean()
        ? 60000U : std::max(1000U, options_.intervalMs);
    if (systemTimer_ && systemTimerIntervalMs_ == interval)
        return;
    if (systemTimer_)
        (void)loop_.cancelTimer(systemTimer_);
    systemTimer_ = 0;
    systemTimerIntervalMs_ = interval;
    const std::weak_ptr<CallbackGate> weakGate(gate_);
    systemTimer_ = loop_.scheduleEvery(EventLoop::Milliseconds(interval), [weakGate] {
        const std::shared_ptr<CallbackGate> gate = weakGate.lock();
        if (!gate)
            return;
        std::lock_guard<std::mutex> lock(gate->mutex);
        if (!gate->owner)
            return;
        if (gate->owner->logger_)
            gate->owner->logger_->debug("native MQTT telemetry: system refresh timer fired");
        gate->owner->collectSystem();
    });
    if (!systemTimer_) {
        systemTimerIntervalMs_ = 0;
        if (logger_)
            logger_->warning("native MQTT telemetry: system refresh timer could not be scheduled");
    }
}

void MqttTelemetryPublisher::publishNow() {
    if (!started_ || stopped_ || !publish_)
        return;
    if (timer_) {
        (void)loop_.cancelTimer(timer_);
        timer_ = 0;
    }
    lastPublish_ = std::chrono::steady_clock::now();
    if (options_.composeFull && !fullPayload_.valid())
        return;
    const Json payload = options_.composeFull
        ? fullPayload_ : aggregateTelemetry(base_, cache_.snapshot());
    if (publish_(topics_.telemetry(), payload.dump(), true) && options_.onPublished)
        options_.onPublished();
}

// A change the TV reports: a fresh read when the payload is the whole of what
// the TV reports, otherwise the live state as it is now.
void MqttTelemetryPublisher::publishFresh() {
    if (options_.composeFull) {
        lastPublish_ = std::chrono::steady_clock::now();
        collectSystem();
    } else {
        publishNow();
    }
}

void MqttTelemetryPublisher::collectSystem() {
    if (!connected_ || stopped_ || options_.systemRoot.empty() || options_.dataRoot.empty())
        return;
    if (systemReadInFlight_) {
        // A change during a read goes out after it, as that read may have
        // seen the value from before.
        collectAgain_ = options_.composeFull != nullptr;
        if (logger_)
            logger_->debug("native MQTT telemetry: system refresh skipped; read still in flight");
        return;
    }
    systemReadInFlight_ = true;
    {
        std::lock_guard<std::mutex> lock(systemWorkerMutex_);
        if (systemWorkerStop_) {
            systemReadInFlight_ = false;
            return;
        }
        systemWorkerPending_ = true;
    }
    if (logger_)
        logger_->debug("native MQTT telemetry: system refresh queued");
    systemWorkerCondition_.notify_one();
}

void MqttTelemetryPublisher::systemWorkerLoop() {
    const std::string systemRoot = options_.systemRoot;
    const std::string dataRoot = options_.dataRoot;
    const std::weak_ptr<CallbackGate> weakGate(gate_);
    while (true) {
        {
            std::unique_lock<std::mutex> lock(systemWorkerMutex_);
            systemWorkerCondition_.wait(lock, [this] {
                return systemWorkerStop_ || systemWorkerPending_;
            });
            if (systemWorkerStop_)
                return;
            systemWorkerPending_ = false;
        }

        const auto started = std::chrono::steady_clock::now();
        std::shared_ptr<Json> snapshot = std::make_shared<Json>();
        std::shared_ptr<Json> extra = std::make_shared<Json>();
        bool extraFresh = false;
        std::exception_ptr failure;
        try {
            if (logger_)
                logger_->debug("native MQTT telemetry: system read started");
            *snapshot = service::collectSystem(systemRoot, dataRoot);
        } catch (...) {
            failure = std::current_exception();
        }
        if (options_.collectExtra) {
            const auto now = std::chrono::steady_clock::now();
            const bool expired = extraCollectedAt_ == std::chrono::steady_clock::time_point() ||
                now - extraCollectedAt_ >= std::chrono::seconds(120);
            if (expired) {
                extraFresh = true;
                extraCollectedAt_ = now;
            }
        }
        if (options_.collectExtra && extraFresh) {
            try {
                cachedExtra_ = options_.collectExtra();
            } catch (const std::exception &error) {
                cachedExtra_ = Json::object();
                if (logger_)
                    logger_->warning("native MQTT telemetry: extra collection failed: " +
                                     std::string(error.what()));
            } catch (...) {
                cachedExtra_ = Json::object();
                if (logger_)
                    logger_->warning("native MQTT telemetry: extra collection failed");
            }
        }
        if (options_.collectExtra)
            *extra = cachedExtra_;
        std::shared_ptr<Json> full = std::make_shared<Json>();
        if (options_.composeFull && !failure && (*snapshot)["ok"].boolean()) {
            try {
                *full = options_.composeFull(*snapshot, *extra);
            } catch (const std::exception &error) {
                if (logger_)
                    logger_->warning("native MQTT telemetry: payload failed: " +
                                     std::string(error.what()));
            } catch (...) {
                if (logger_)
                    logger_->warning("native MQTT telemetry: payload failed");
            }
        }
        if (logger_) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count();
            logger_->debug("native MQTT telemetry: system read finished in " +
                           std::to_string(elapsed) + "ms");
        }

        const bool posted = loop_.post([weakGate, snapshot, extra, full, extraFresh, failure] {
            const std::shared_ptr<CallbackGate> gate = weakGate.lock();
            if (!gate)
                return;
            std::lock_guard<std::mutex> lock(gate->mutex);
            if (!gate->owner)
                return;
            MqttTelemetryPublisher *owner = gate->owner;
            owner->systemReadInFlight_ = false;
            const bool again = owner->collectAgain_;
            owner->collectAgain_ = false;
            if (owner->connected_ && !owner->stopped_) {
                bool updated = false;
                if (full->valid()) {
                    owner->fullPayload_ = *full;
                    updated = true;
                }
                if (!failure && (*snapshot)["ok"].boolean()) {
                    if (owner->logger_)
                        owner->logger_->debug(
                            "native MQTT telemetry: system refresh completed");
                    owner->applySystemSnapshot(*snapshot);
                    updated = true;
                } else if (owner->logger_) {
                    owner->logger_->warning(
                        "native MQTT telemetry: system refresh failed or became stale");
                }
                if (!failure && (*snapshot)["ok"].boolean() &&
                    owner->options_.onSystemCollected) {
                    try {
                        owner->options_.onSystemCollected(*snapshot);
                    } catch (...) {
                        if (owner->logger_)
                            owner->logger_->warning(
                                "native MQTT telemetry: system callback failed");
                    }
                }
                const Json rows = (*extra)["rows"];
                if (rows.isArray()) {
                    Json values = Json::object();
                    for (const Json &row : rows.items()) {
                        const std::string id = row["id"].string();
                        if (id.empty())
                            continue;
                        if (row["type"].string() == "switch" && row["on"].isBoolean())
                            values.set(id, row["on"]);
                        else if (row["value"].valid())
                            values.set(id, row["value"]);
                    }
                    owner->base_.set("lgs", values);
                    updated = true;
                }
                if (owner->options_.onCollected &&
                    (extraFresh || !owner->extraCallbackDelivered_)) {
                    try {
                        owner->options_.onCollected(*snapshot, *extra);
                        owner->extraCallbackDelivered_ = true;
                    } catch (...) {
                        if (owner->logger_)
                            owner->logger_->warning(
                                "native MQTT telemetry: collection callback failed");
                    }
                }
                if (updated)
                    owner->publishNow();
                if (again)
                    owner->collectSystem();
            } else if (owner->logger_ && !owner->stopped_) {
                owner->logger_->debug(
                    "native MQTT telemetry: collection finished while disconnected");
            }
        });
        if (!posted) {
            const std::shared_ptr<CallbackGate> gate = weakGate.lock();
            if (gate) {
                std::lock_guard<std::mutex> lock(gate->mutex);
                if (gate->owner) {
                    gate->owner->systemReadInFlight_ = false;
                    if (gate->owner->logger_ && !gate->owner->stopped_)
                        gate->owner->logger_->warning(
                            "native MQTT telemetry: system refresh completion could not be queued");
                }
            }
        }
    }
}

void MqttTelemetryPublisher::applySystemSnapshot(const Json &system) {
    base_.set("time", Json::numeric(static_cast<double>(wallMilliseconds())));
    for (const char *key : {"temp", "tempMillidegrees", "mem", "swap", "uptime",
                            "loadavg", "wifi", "remote", "emmc", "gpuMhz", "gpuHz",
                            "cpuHz", "mhz", "appStorage"})
        setIfValid(base_, key, system);

    const Json network = system["net"];
    if (network.isObject() && network["iface"].isString() &&
        network["rx"].isNumber() && network["tx"].isNumber()) {
        Json totals = Json::object();
        totals.set("rx", network["rx"]);
        totals.set("tx", network["tx"]);
        totals.set("iface", network["iface"]);
        base_.set("netTotal", totals);
        setIfValid(base_, "mac", network, "mac");
        const service::NetworkCounters counters{
            network["iface"].string(),
            static_cast<uint64_t>(std::max(0.0, network["rx"].number())),
            static_cast<uint64_t>(std::max(0.0, network["tx"].number()))};
        const Json rate = networkRates_.sample(monotonicMilliseconds(), "mqtt", 0, counters);
        if (rate.isObject())
            base_.set("net", rate);
    }

    const Json cpu = system["cpu"];
    const Json sample = cpuWindows_.sample(
        static_cast<unsigned>(std::max(0.0, cpu["coresTotal"].number())),
        monotonicMilliseconds(), "mqtt", 0, cpuTicks(cpu));
    const Json loads = cpuCoreLoads(cpu, sample);
    base_.set("cores", loads);
    base_.set("coresTotal", Json::numeric(cpu["coresTotal"].number()));
    if (sample["overall"].isNumber()) {
        base_.set("load", sample["overall"]);
    } else if (cpu["currentLoad"].isNumber()) {
        base_.set("load", cpu["currentLoad"]);
    } else if (loads.isArray() && loads.size() > 0) {
        double sum = 0;
        double peak = 0;
        for (const Json &load : loads.items()) {
            sum += load.number();
            peak = std::max(peak, load.number());
        }
        base_.set("load", Json::numeric(std::floor(sum / loads.size() + 0.5)));
        base_.set("loadPeak", Json::numeric(peak));
    }
    if (sample["overall"].isNumber() || cpu["currentLoad"].isNumber()) {
        double peak = 0;
        for (const Json &load : loads.items())
            peak = std::max(peak, load.number());
        if (loads.size() > 0)
            base_.set("loadPeak", Json::numeric(peak));
        else if (cpu["currentLoad"].isNumber())
            base_.set("loadPeak", cpu["currentLoad"]);
    }
    const Json boot = cpu["times"]["btime"];
    if (boot.isNumber()) {
        base_.set("btime", boot);
        const std::time_t seconds = static_cast<std::time_t>(boot.number());
        std::tm utc{};
        char iso[32] = {};
        if (gmtime_r(&seconds, &utc) && std::strftime(iso, sizeof(iso),
                                                       "%Y-%m-%dT%H:%M:%SZ", &utc))
            base_.set("bootTime", Json::text(iso));
    }
    const Json pictureEngine = system["pictureEngine"];
    if (pictureEngine.isObject()) {
        base_.set("picture_engine", pictureEngine);
        setIfValid(base_, "colorimetry", pictureEngine, "colorimetry");
    }

    const Json temperature = system["temp"];
    if (temperature.isNumber() && temperature.number() > 0) {
        tempHistory_.push_back(temperature);
        if (tempHistory_.size() > 120)
            tempHistory_.erase(tempHistory_.begin());
    }
    Json temps = Json::array();
    for (const Json &value : tempHistory_)
        temps.add(value);
    base_.set("temps", temps);
}

} // namespace daemon
} // namespace nativetv
