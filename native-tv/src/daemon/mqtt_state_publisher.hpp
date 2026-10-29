#pragma once

#include "daemon/event_loop.hpp"
#include "daemon/mqtt_topics.hpp"
#include "domain/live_state.hpp"
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace nativetv {
namespace daemon {

// Publishes the live state cache to retained MQTT topics. The callback gate
// makes the cache's non-removable listener safe across publisher shutdown.
class MqttStatePublisher {
  public:
    using Publish = std::function<bool(const std::string &, const std::string &, bool)>;

    MqttStatePublisher(EventLoop &loop, state::Cache &cache, MqttTopics topics,
                       Publish publish);
    ~MqttStatePublisher();
    MqttStatePublisher(const MqttStatePublisher &) = delete;
    MqttStatePublisher &operator=(const MqttStatePublisher &) = delete;

    bool start();
    void stop();
    void publishSnapshot();

  private:
    struct CallbackGate {
        std::mutex mutex;
        MqttStatePublisher *owner = nullptr;
    };

    void onChange(const state::Event &event);
    void publishValue(const std::string &group, const std::string &key,
                      const Json &value);
    void publishRemote(bool force, bool settled);
    static std::string scalar(const Json &value);
    static bool truthy(const Json &value);

    EventLoop &loop_;
    state::Cache &cache_;
    MqttTopics topics_;
    Publish publish_;
    std::shared_ptr<CallbackGate> gate_;
    EventLoop::TimerId emptyRemoteTimer_ = 0;
    std::string lastRemoteBody_;
    bool remoteSnapshotPending_ = false;
    bool started_ = false;
    bool stopped_ = false;
};

} // namespace daemon
} // namespace nativetv
