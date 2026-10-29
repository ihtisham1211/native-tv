#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace nativetv {
namespace daemon {

struct MqttClientOptions {
    std::string host;
    unsigned short port = 1883;
    bool tls = false;
    std::string caFile;
    bool tlsRejectUnauthorized = true;
    std::string username;
    std::string password;
    std::string clientId;
    unsigned short keepAliveSeconds = 60;
    std::string willTopic;
    std::string willPayload;
    bool willRetain = true;
    unsigned connectTimeoutMs = 5000;
    unsigned reconnectDelayMs = 5000;
};

struct MqttMessage {
    std::string topic;
    std::string payload;
    unsigned qos = 0;
    bool retained = false;
};

// MQTT 3.1.1 QoS 0 publisher with QoS 0-2 subscription acknowledgements.
// Its private network thread owns the libcurl connect-only socket; callers
// may publish and subscribe from any thread. Handlers run on that thread.
class MqttClient {
  public:
    using MessageHandler = std::function<void(const MqttMessage &)>;
    using StateHandler = std::function<void()>;
    using ErrorHandler = std::function<void(const std::string &)>;

    explicit MqttClient(MqttClientOptions options);
    ~MqttClient();
    MqttClient(const MqttClient &) = delete;
    MqttClient &operator=(const MqttClient &) = delete;

    void setHandlers(MessageHandler message, StateHandler connected,
                     StateHandler disconnected, ErrorHandler error);
    bool start();
    void stop();
    bool publish(const std::string &topic, const std::string &payload,
                 bool retained = false);
    bool subscribe(const std::string &topic, unsigned qos = 0);
    bool setWill(const std::string &payload);
    // Every retained topic published and not since cleared, for removing this
    // TV from Home Assistant and the broker on uninstall.
    std::vector<std::string> retainedTopics() const;
    bool connected() const;
    long long latencyMilliseconds() const;

  private:
    struct Outbound {
        std::string topic;
        std::string payload;
        bool retained = false;
        bool subscription = false;
        unsigned qos = 0;
    };

    void run();
    bool connectAndRun();
    void notifyError(const std::string &message) const;

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    MqttClientOptions options_;
    std::map<std::string, unsigned> subscriptions_;
    std::set<std::string> retained_;
    std::vector<Outbound> outbound_;
    MessageHandler messageHandler_;
    StateHandler connectedHandler_;
    StateHandler disconnectedHandler_;
    ErrorHandler errorHandler_;
    std::thread thread_;
    std::atomic<bool> connected_{false};
    std::atomic<long long> latencyMs_{-1};
    bool running_ = false;
    bool reconnectNow_ = false;
};

} // namespace daemon
} // namespace nativetv
