#include "daemon/mqtt_client.hpp"
#include <arpa/inet.h>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <functional>
#include <mutex>
#include <netinet/in.h>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
using namespace nativetv::daemon;

void readAll(int fd, unsigned char *data, size_t size) {
    size_t offset = 0;
    while (offset < size) {
        const ssize_t count = recv(fd, data + offset, size - offset, 0);
        if (count <= 0)
            throw std::runtime_error("broker socket closed while reading packet");
        offset += static_cast<size_t>(count);
    }
}

void writeAll(int fd, const unsigned char *data, size_t size) {
    size_t offset = 0;
    while (offset < size) {
        const ssize_t count = send(fd, data + offset, size - offset, 0);
        if (count <= 0)
            throw std::runtime_error("broker socket closed while writing packet");
        offset += static_cast<size_t>(count);
    }
}

std::vector<unsigned char> readPacket(int fd) {
    unsigned char first[1];
    readAll(fd, first, sizeof(first));
    std::vector<unsigned char> packet(first, first + 1);
    size_t remaining = 0;
    size_t multiplier = 1;
    unsigned char digit = 0;
    unsigned count = 0;
    do {
        readAll(fd, &digit, 1);
        packet.push_back(digit);
        remaining += (digit & 0x7f) * multiplier;
        multiplier *= 128;
        if (++count > 4)
            throw std::runtime_error("invalid MQTT remaining length");
    } while (digit & 0x80);
    if (remaining > 1024 * 1024)
        throw std::runtime_error("oversized MQTT test packet");
    const size_t header = packet.size();
    packet.resize(header + remaining);
    if (remaining)
        readAll(fd, packet.data() + header, remaining);
    return packet;
}

unsigned packetType(const std::vector<unsigned char> &packet) {
    return packet.empty() ? 0 : packet[0] >> 4;
}

std::vector<unsigned char> inboundQosOne() {
    const std::string topic = "lgtv/command/power";
    const std::string payload = "ON";
    std::vector<unsigned char> packet{0x33,
        static_cast<unsigned char>(2 + topic.size() + 2 + payload.size()),
        0, static_cast<unsigned char>(topic.size())};
    packet.insert(packet.end(), topic.begin(), topic.end());
    packet.push_back(0);
    packet.push_back(9);
    packet.insert(packet.end(), payload.begin(), payload.end());
    return packet;
}

class LocalBroker {
  public:
    LocalBroker() {
        listener_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listener_ < 0)
            throw std::runtime_error("could not create local MQTT test broker");
        int reuse = 1;
        (void)setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (bind(listener_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0 ||
            listen(listener_, 2) < 0) {
            close(listener_);
            throw std::runtime_error("could not bind local MQTT test broker");
        }
        socklen_t length = sizeof(address);
        if (getsockname(listener_, reinterpret_cast<sockaddr *>(&address), &length) < 0) {
            close(listener_);
            throw std::runtime_error("could not read local MQTT test port");
        }
        port_ = ntohs(address.sin_port);
    }

    ~LocalBroker() {
        if (listener_ >= 0)
            close(listener_);
    }

    unsigned short port() const { return port_; }

    void run() {
        worker_ = std::thread([this] {
            try {
                runBroker();
            } catch (...) {
                std::lock_guard<std::mutex> lock(mutex_);
                error_ = std::current_exception();
            }
        });
    }

    void finish() {
        if (worker_.joinable())
            worker_.join();
        if (error_)
            std::rethrow_exception(error_);
    }

  private:
    int acceptClient() {
        const int client = accept(listener_, nullptr, nullptr);
        if (client < 0)
            throw std::runtime_error("local MQTT broker accept failed");
        timeval timeout{};
        timeout.tv_sec = 6;
        (void)setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        return client;
    }

    void sendConnack(int fd) {
        const unsigned char bytes[] = {0x20, 0x02, 0x00, 0x00};
        writeAll(fd, bytes, 2);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        writeAll(fd, bytes + 2, 2);
    }

    void runBroker() {
        int first = acceptClient();
        const std::vector<unsigned char> connect = readPacket(first);
        const std::string connectBytes(connect.begin(), connect.end());
        if (packetType(connect) != 1 || connect.size() < 12 ||
            (connect[9] & 0xa4) != 0xa4 ||
            connectBytes.find("nativetv-test") == std::string::npos ||
            connectBytes.find("user") == std::string::npos ||
            connectBytes.find("pass") == std::string::npos)
            throw std::runtime_error("MQTT CONNECT flags or credentials were not encoded");
        sendConnack(first);

        bool sawPublish = false;
        bool sawSubscribe = false;
        while (!sawPublish || !sawSubscribe) {
            const std::vector<unsigned char> packet = readPacket(first);
            if (packetType(packet) == 3) {
                if ((packet[0] & 0x0f) != 1)
                    throw std::runtime_error("retained MQTT publish flag was missing");
                if (packet.size() < 5)
                    throw std::runtime_error("truncated MQTT retained publish");
                const size_t topicSize = (static_cast<size_t>(packet[2]) << 8) | packet[3];
                if (topicSize > packet.size() - 4)
                    throw std::runtime_error("invalid MQTT publish topic length");
                const std::string publishedTopic(packet.begin() + 4,
                                                  packet.begin() + 4 + topicSize);
                const std::string publishedPayload(packet.begin() + 4 + topicSize,
                                                    packet.end());
                if (publishedTopic != "lgtv/status" || publishedPayload != "online")
                    throw std::runtime_error("retained MQTT publish payload did not match");
                sawPublish = true;
            } else if (packetType(packet) == 8) {
                if (packet.size() < 7)
                    throw std::runtime_error("truncated MQTT subscription");
                const size_t filterSize = (static_cast<size_t>(packet[4]) << 8) | packet[5];
                if (filterSize != packet.size() - 7)
                    throw std::runtime_error("invalid MQTT subscription filter length");
                const std::string filter(packet.begin() + 6, packet.end() - 1);
                if (filter != "lgtv/command/#")
                    throw std::runtime_error("MQTT command subscription filter changed");
                sawSubscribe = true;
                const unsigned char ack[] = {0x90, 0x03, packet[2], packet[3], 0x00};
                writeAll(first, ack, sizeof(ack));
            } else {
                throw std::runtime_error("unexpected MQTT packet from client");
            }
        }
        const std::vector<unsigned char> inbound = inboundQosOne();
        writeAll(first, inbound.data(), inbound.size());
        const std::vector<unsigned char> puback = readPacket(first);
        if (puback.size() != 4 || puback[0] != 0x40 || puback[2] != 0 || puback[3] != 9)
            throw std::runtime_error("client did not acknowledge inbound QoS 1 publish");
        const std::vector<unsigned char> disconnect = readPacket(first);
        if (disconnect.size() != 2 || disconnect[0] != 0xe0)
            throw std::runtime_error("updated will did not cleanly disconnect the client");
        close(first);

        int second = acceptClient();
        const std::vector<unsigned char> reconnect = readPacket(second);
        const std::string reconnectBytes(reconnect.begin(), reconnect.end());
        if (packetType(reconnect) != 1 || reconnectBytes.find("sleep") == std::string::npos)
            throw std::runtime_error("updated MQTT last will was not sent on reconnect");
        sendConnack(second);
        bool restoredSubscription = false;
        bool cleanDisconnect = false;
        while (!cleanDisconnect) {
            const std::vector<unsigned char> packet = readPacket(second);
            if (packetType(packet) == 8) {
                restoredSubscription = true;
            } else if (packet.size() == 2 && packet[0] == 0xe0) {
                cleanDisconnect = true;
            } else {
                throw std::runtime_error("unexpected packet after MQTT reconnect");
            }
        }
        if (!restoredSubscription)
            throw std::runtime_error("client did not restore subscription after reconnect");
        close(second);
    }

    int listener_ = -1;
    unsigned short port_ = 0;
    std::thread worker_;
    std::mutex mutex_;
    std::exception_ptr error_;
};

bool waitFor(std::condition_variable &condition, std::mutex &mutex,
             const std::function<bool()> &predicate) {
    std::unique_lock<std::mutex> lock(mutex);
    return condition.wait_for(lock, std::chrono::seconds(5), predicate);
}

// test-mqtt.js: a broker that does what each case needs, one thread per connection.
class ScriptedBroker {
  public:
    using Handler = std::function<void(int fd, unsigned connection)>;

    explicit ScriptedBroker(Handler handler) : handler_(std::move(handler)) {
        listener_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listener_ < 0)
            throw std::runtime_error("could not create scripted MQTT broker");
        int reuse = 1;
        (void)setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t length = sizeof(address);
        if (bind(listener_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0 ||
            listen(listener_, 4) < 0 ||
            getsockname(listener_, reinterpret_cast<sockaddr *>(&address), &length) < 0) {
            close(listener_);
            throw std::runtime_error("could not bind scripted MQTT broker");
        }
        port_ = ntohs(address.sin_port);
        acceptor_ = std::thread([this] { acceptLoop(); });
    }
    ~ScriptedBroker() {
        stopping_ = true;
        if (acceptor_.joinable()) acceptor_.join();
        for (std::thread &handler : handlers_)
            if (handler.joinable()) handler.join();
        close(listener_);
    }
    unsigned short port() const { return port_; }
    unsigned connections() const { return connections_.load(); }
    // Joins the handlers, then throws what any of them did.
    void finish() {
        stopping_ = true;
        if (acceptor_.joinable()) acceptor_.join();
        for (std::thread &handler : handlers_)
            if (handler.joinable()) handler.join();
        if (error_) std::rethrow_exception(error_);
    }

  private:
    void acceptLoop() {
        while (!stopping_) {
            pollfd waiting{listener_, POLLIN, 0};
            if (poll(&waiting, 1, 50) <= 0) continue;
            const int client = accept(listener_, nullptr, nullptr);
            if (client < 0) continue;
            timeval timeout{};
            timeout.tv_sec = 6;
            (void)setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            const unsigned index = connections_.fetch_add(1);
            handlers_.emplace_back([this, client, index] {
                try {
                    handler_(client, index);
                } catch (...) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (!error_) error_ = std::current_exception();
                }
                close(client);
            });
        }
    }

    Handler handler_;
    int listener_ = -1;
    unsigned short port_ = 0;
    std::thread acceptor_;
    std::vector<std::thread> handlers_;
    std::atomic<unsigned> connections_{0};
    std::atomic<bool> stopping_{false};
    std::mutex mutex_;
    std::exception_ptr error_;
};

void sendConnack(int fd, unsigned char code = 0) {
    const unsigned char reply[] = {0x20, 0x02, 0x00, code};
    writeAll(fd, reply, sizeof(reply));
}

// Reads until the client goes, which ends the connection's script.
void drain(int fd) {
    try {
        for (;;) (void)readPacket(fd);
    } catch (const std::exception &) {
    }
}

bool eventually(const std::function<bool()> &predicate, int milliseconds) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return predicate();
}

MqttClientOptions scriptedOptions(unsigned short port, const std::string &clientId) {
    MqttClientOptions options;
    options.host = "127.0.0.1";
    options.port = port;
    options.clientId = clientId;
    options.reconnectDelayMs = 10;
    return options;
}

// CONNACK 4 and 5 are reported in words, not as a code, and the client is not connected.
void testRefusedConnectionsAreDescribed() {
    const struct { unsigned char code; const char *message; } refusals[] = {
        {4, "bad username or password"}, {5, "not authorized"}};
    for (const auto &refusal : refusals) {
        const unsigned char code = refusal.code;
        ScriptedBroker broker([code](int fd, unsigned) {
            (void)readPacket(fd);
            sendConnack(fd, code);
        });
        MqttClientOptions options = scriptedOptions(broker.port(), "badauth");
        options.reconnectDelayMs = 5000;  // one attempt: the next waits past the stop
        MqttClient client(options);
        std::mutex mutex;
        std::string error;
        bool connected = false;
        client.setHandlers({}, [&] {
            std::lock_guard<std::mutex> lock(mutex);
            connected = true;
        }, {}, [&](const std::string &message) {
            std::lock_guard<std::mutex> lock(mutex);
            if (error.empty()) error = message;
        });
        assert(client.start());
        assert(eventually([&] { std::lock_guard<std::mutex> lock(mutex); return !error.empty(); }, 3000));
        client.stop();
        broker.finish();
        std::lock_guard<std::mutex> lock(mutex);
        assert(error == refusal.message);
        assert(!connected && !client.connected());
    }
}

// A PINGRESP replaces the connection time as the latency.
void testPingResponseRecordsLatency() {
    ScriptedBroker broker([](int fd, unsigned) {
        (void)readPacket(fd);
        sendConnack(fd);
        const std::vector<unsigned char> ping = readPacket(fd);
        if (packetType(ping) != 12)
            throw std::runtime_error("the client's first packet after CONNACK was not a PINGREQ");
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        const unsigned char pong[] = {0xd0, 0x00};
        writeAll(fd, pong, sizeof(pong));
        drain(fd);
    });
    MqttClientOptions options = scriptedOptions(broker.port(), "t_lat");
    options.keepAliveSeconds = 2;  // pings every second
    MqttClient client(options);
    client.setHandlers({}, {}, {}, {});
    assert(client.start());
    assert(eventually([&] { return client.connected() && client.latencyMilliseconds() >= 0; }, 3000));
    assert(client.latencyMilliseconds() < 80);
    assert(eventually([&] { return client.latencyMilliseconds() >= 80; }, 4000));
    client.stop();
    broker.finish();
}

// A broker that never answers a ping is dropped and the client connects again.
void testSilentBrokerIsReconnected() {
    ScriptedBroker broker([](int fd, unsigned) {
        (void)readPacket(fd);
        sendConnack(fd);
        drain(fd);
    });
    MqttClientOptions options = scriptedOptions(broker.port(), "t_silent");
    options.keepAliveSeconds = 1;
    MqttClient client(options);
    std::mutex mutex;
    std::string error;
    client.setHandlers({}, {}, {}, [&](const std::string &message) {
        std::lock_guard<std::mutex> lock(mutex);
        if (error.empty()) error = message;
    });
    assert(client.start());
    assert(eventually([&] { return broker.connections() >= 2; }, 8000));
    client.stop();
    broker.finish();
    std::lock_guard<std::mutex> lock(mutex);
    assert(error.find("keep-alive") != std::string::npos);
}

// A broker that never closes its end after DISCONNECT does not hold the client up.
void testUnacknowledgedCloseIsForced() {
    ScriptedBroker broker([](int fd, unsigned connection) {
        (void)readPacket(fd);
        sendConnack(fd);
        if (connection == 0) {
            const std::vector<unsigned char> disconnect = readPacket(fd);
            if (disconnect.size() != 2 || disconnect[0] != 0xe0)
                throw std::runtime_error("the updated will did not disconnect the client");
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));  // socket left open
        } else {
            drain(fd);
        }
    });
    MqttClientOptions options = scriptedOptions(broker.port(), "t_stuck");
    options.willTopic = "lgtv/status";
    options.willPayload = "offline";
    MqttClient client(options);
    client.setHandlers({}, {}, {}, {});
    assert(client.start());
    assert(eventually([&] { return client.connected(); }, 3000));
    assert(client.setWill("asleep"));
    assert(eventually([&] { return broker.connections() >= 2; }, 1500));
    assert(eventually([&] { return client.connected(); }, 1500));
    client.stop();
    broker.finish();
}
} // namespace

int main() {
    LocalBroker broker;
    broker.run();
    MqttClientOptions options;
    options.host = "127.0.0.1";
    options.port = broker.port();
    options.clientId = "nativetv-test";
    options.username = "user";
    options.password = "pass";
    options.willTopic = "lgtv/status";
    options.willPayload = "offline";
    options.willRetain = true;
    options.reconnectDelayMs = 10;
    MqttClient client(options);
    std::mutex mutex;
    std::condition_variable condition;
    unsigned connects = 0;
    unsigned disconnects = 0;
    bool received = false;
    std::string error;
    client.setHandlers(
        [&](const MqttMessage &message) {
            std::lock_guard<std::mutex> lock(mutex);
            received = message.topic == "lgtv/command/power" && message.payload == "ON" &&
                       message.qos == 1 && message.retained;
            condition.notify_all();
        },
        [&] {
            std::lock_guard<std::mutex> lock(mutex);
            ++connects;
            condition.notify_all();
        },
        [&] {
            std::lock_guard<std::mutex> lock(mutex);
            ++disconnects;
            condition.notify_all();
        },
        [&](const std::string &message) {
            std::lock_guard<std::mutex> lock(mutex);
            error = message;
            condition.notify_all();
        });

    assert(client.subscribe("lgtv/command/#"));
    assert(!client.subscribe("lgtv/#/invalid"));
    assert(!client.subscribe("lgtv/command+"));
    assert(client.start());
    assert(waitFor(condition, mutex, [&] { return connects == 1 || !error.empty(); }));
    assert(error.empty());
    assert(client.connected());
    assert(client.latencyMilliseconds() >= 0);
    assert(!client.publish("lgtv/command/#", "invalid", false));
    assert(client.publish("lgtv/status", "online", true));
    assert(waitFor(condition, mutex, [&] { return received || !error.empty(); }));
    assert(error.empty());
    assert(received);
    assert(client.setWill("sleep"));
    assert(waitFor(condition, mutex, [&] { return connects == 2 || !error.empty(); }));
    assert(error.empty());
    client.stop();
    broker.finish();
    assert(connects == 2);
    assert(disconnects >= 1);
    testRefusedConnectionsAreDescribed();
    testPingResponseRecordsLatency();
    testSilentBrokerIsReconnected();
    testUnacknowledgedCloseIsForced();
    return 0;
}
