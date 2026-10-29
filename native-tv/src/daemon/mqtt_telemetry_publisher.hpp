#pragma once

#include "daemon/event_loop.hpp"
#include "daemon/logger.hpp"
#include "daemon/mqtt_topics.hpp"
#include "domain/live_state.hpp"
#include "service/cpu_windows.hpp"
#include "service/network_rates.hpp"
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace nativetv {
namespace daemon {

struct MqttTelemetryOptions {
    std::string systemRoot;
    std::string dataRoot;
    unsigned intervalMs = 30000;
    std::function<Json()> collectExtra;
    std::function<void()> onPublished;
    std::function<void(const Json &, const Json &)> onCollected;
    std::function<void(const Json &)> onSystemCollected;
    // The whole telemetry payload from the system snapshot and the extra
    // reads (LG's settings), made on the worker as it may take seconds. When
    // set, it replaces the payload built here from the live state and the
    // snapshot alone, which lacks most of what Home Assistant's entities read,
    // and each change asks for a fresh one instead of republishing the last.
    std::function<Json(const Json &system, const Json &extra)> composeFull;
};

// Builds the retained aggregate telemetry payload from native state and
// publishes it independently from the per-state MQTT topics.
class MqttTelemetryPublisher {
  public:
    using Publish = std::function<bool(const std::string &, const std::string &, bool)>;

    MqttTelemetryPublisher(EventLoop &loop, state::Cache &cache, MqttTopics topics,
                           Publish publish,
                           MqttTelemetryOptions options = MqttTelemetryOptions(),
                           Logger *logger = nullptr);
    ~MqttTelemetryPublisher();
    MqttTelemetryPublisher(const MqttTelemetryPublisher &) = delete;
    MqttTelemetryPublisher &operator=(const MqttTelemetryPublisher &) = delete;

    bool start();
    void stop();
    void setBase(const Json &base);
    void setConnected(bool connected);
    void setSystemSnapshot(const Json &system);
    void publishSnapshot();

  private:
    struct CallbackGate {
        std::mutex mutex;
        MqttTelemetryPublisher *owner = nullptr;
    };

    void onChange(const state::Event &event);
    void schedulePublish();
    void scheduleSystemTimer();
    void publishNow();
    void publishFresh();
    void collectSystem();
    void systemWorkerLoop();
    void applySystemSnapshot(const Json &system);

    EventLoop &loop_;
    state::Cache &cache_;
    MqttTopics topics_;
    Publish publish_;
    MqttTelemetryOptions options_;
    Logger *logger_;
    Json base_ = Json::object();
    std::shared_ptr<CallbackGate> gate_;
    EventLoop::TimerId timer_ = 0;
    EventLoop::TimerId systemTimer_ = 0;
    unsigned systemTimerIntervalMs_ = 0;
    service::SystemCpuWindows cpuWindows_;
    service::SystemNetworkRates networkRates_;
    std::vector<Json> tempHistory_;
    std::chrono::steady_clock::time_point lastPublish_;
    std::mutex systemWorkerMutex_;
    std::condition_variable systemWorkerCondition_;
    std::thread systemWorker_;
    Json cachedExtra_;
    Json fullPayload_;
    bool collectAgain_ = false;
    std::chrono::steady_clock::time_point extraCollectedAt_;
    bool systemWorkerPending_ = false;
    bool systemWorkerStop_ = false;
    bool systemReadInFlight_ = false;
    bool extraCallbackDelivered_ = false;
    bool connected_ = false;
    bool started_ = false;
    bool stopped_ = false;
};

} // namespace daemon
} // namespace nativetv
