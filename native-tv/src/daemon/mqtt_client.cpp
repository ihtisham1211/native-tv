#include "daemon/mqtt_client.hpp"
#include <curl/curl.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cerrno>
#include <memory>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <unistd.h>

namespace nativetv {
namespace daemon {
namespace {
const size_t kMaximumPacketBytes = 1024 * 1024;
const size_t kMaximumPendingPublishes = 1024;

void initializeCurl() {
    static std::once_flag once;
    static CURLcode result = CURLE_FAILED_INIT;
    std::call_once(once, [] { result = curl_global_init(CURL_GLOBAL_DEFAULT); });
    if (result != CURLE_OK)
        throw std::runtime_error("libcurl global initialization failed");
}

bool validMqttString(const std::string &value) {
    return value.size() <= 65535 && value.find('\0') == std::string::npos;
}

bool validTopicName(const std::string &topic) {
    return !topic.empty() && validMqttString(topic) &&
           topic.find_first_of("+#") == std::string::npos;
}

bool validTopicFilter(const std::string &filter) {
    if (filter.empty() || !validMqttString(filter))
        return false;
    size_t start = 0;
    while (start <= filter.size()) {
        const size_t end = filter.find('/', start);
        const size_t length = (end == std::string::npos ? filter.size() : end) - start;
        const std::string level = filter.substr(start, length);
        if (level.find('#') != std::string::npos && level != "#")
            return false;
        if (level.find('+') != std::string::npos && level != "+")
            return false;
        if (level == "#" && end != std::string::npos)
            return false;
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return true;
}

void appendU16(std::vector<unsigned char> &out, size_t value) {
    out.push_back(static_cast<unsigned char>((value >> 8) & 0xff));
    out.push_back(static_cast<unsigned char>(value & 0xff));
}

void appendString(std::vector<unsigned char> &out, const std::string &value) {
    appendU16(out, value.size());
    out.insert(out.end(), value.begin(), value.end());
}

bool appendRemainingLength(std::vector<unsigned char> &out, size_t length) {
    if (length > 268435455)
        return false;
    do {
        unsigned char digit = static_cast<unsigned char>(length % 128);
        length /= 128;
        if (length)
            digit |= 0x80;
        out.push_back(digit);
    } while (length);
    return true;
}

bool makeConnectPacket(const MqttClientOptions &options,
                       std::vector<unsigned char> &packet, std::string &error) {
    std::string clientId = options.clientId;
    if (clientId.empty()) {
        std::ostringstream generated;
        generated << "nativetv-" << static_cast<long>(getpid());
        clientId = generated.str();
    }
    if (!validMqttString(clientId) || !validMqttString(options.username) ||
        !validMqttString(options.password) || !validMqttString(options.willTopic) ||
        !validMqttString(options.willPayload)) {
        error = "MQTT strings must be NUL-free and no longer than 65535 bytes";
        return false;
    }
    if (options.keepAliveSeconds == 0) {
        error = "MQTT keep-alive must be between 1 and 65535 seconds";
        return false;
    }

    std::vector<unsigned char> body;
    body.insert(body.end(), {0x00, 0x04, 'M', 'Q', 'T', 'T', 0x04});
    unsigned char flags = 0x02; // Clean session.
    if (!options.willTopic.empty()) {
        flags |= 0x04;
        if (options.willRetain)
            flags |= 0x20;
    }
    if (!options.username.empty())
        flags |= 0x80;
    if (!options.password.empty())
        flags |= 0x40;
    body.push_back(flags);
    appendU16(body, options.keepAliveSeconds);
    appendString(body, clientId);
    if (!options.willTopic.empty()) {
        appendString(body, options.willTopic);
        appendString(body, options.willPayload);
    }
    if (!options.username.empty())
        appendString(body, options.username);
    if (!options.password.empty())
        appendString(body, options.password);

    packet.clear();
    packet.push_back(0x10);
    if (!appendRemainingLength(packet, body.size())) {
        error = "MQTT CONNECT packet exceeds the protocol limit";
        return false;
    }
    packet.insert(packet.end(), body.begin(), body.end());
    return true;
}

bool makePublishPacket(const std::string &topic, const std::string &payload,
                       bool retained, std::vector<unsigned char> &packet) {
    if (!validTopicName(topic))
        return false;
    const size_t bodyLength = 2 + topic.size() + payload.size();
    if (bodyLength > kMaximumPacketBytes)
        return false;
    packet.clear();
    packet.push_back(static_cast<unsigned char>(0x30 | (retained ? 0x01 : 0x00)));
    if (!appendRemainingLength(packet, bodyLength))
        return false;
    appendString(packet, topic);
    packet.insert(packet.end(), payload.begin(), payload.end());
    return true;
}

bool makeSubscribePacket(const std::string &topic, unsigned qos, unsigned short id,
                         std::vector<unsigned char> &packet) {
    if (!validTopicFilter(topic) || qos > 2 || id == 0)
        return false;
    std::vector<unsigned char> body;
    appendU16(body, id);
    appendString(body, topic);
    body.push_back(static_cast<unsigned char>(qos));
    packet.clear();
    packet.push_back(0x82);
    if (!appendRemainingLength(packet, body.size()))
        return false;
    packet.insert(packet.end(), body.begin(), body.end());
    return true;
}

bool makeControlPacket(unsigned char type, unsigned short packetId,
                       std::vector<unsigned char> &packet) {
    packet = {type, 0x02, static_cast<unsigned char>((packetId >> 8) & 0xff),
              static_cast<unsigned char>(packetId & 0xff)};
    return packetId != 0;
}

bool parsePacketSize(const std::vector<unsigned char> &buffer, size_t &headerSize,
                     size_t &bodySize) {
    if (buffer.size() < 2)
        return false;
    size_t multiplier = 1;
    bodySize = 0;
    size_t index = 1;
    unsigned digit = 0;
    do {
        if (index >= buffer.size() || index > 4)
            return false;
        digit = buffer[index++];
        bodySize += (digit & 0x7f) * multiplier;
        multiplier *= 128;
    } while ((digit & 0x80) != 0);
    if (bodySize > kMaximumPacketBytes)
        return false;
    headerSize = index;
    return true;
}

std::string urlHost(const std::string &host) {
    if (host.empty() || host.find_first_of("/?#@ \t\r\n") != std::string::npos)
        return std::string();
    if (host[0] == '[' && host[host.size() - 1] == ']')
        return host;
    if (host.find(':') != std::string::npos)
        return "[" + host + "]";
    return host;
}

bool waitSocket(curl_socket_t socket, short events, unsigned timeoutMs) {
    pollfd descriptor;
    descriptor.fd = static_cast<int>(socket);
    descriptor.events = events;
    descriptor.revents = 0;
    int result;
    do {
        result = poll(&descriptor, 1, static_cast<int>(timeoutMs));
    } while (result < 0 && errno == EINTR);
    return result > 0 && (descriptor.revents & (events | POLLERR | POLLHUP | POLLNVAL));
}

bool sendAll(CURL *curl, curl_socket_t socket,
             const std::vector<unsigned char> &packet, unsigned timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs);
    size_t sent = 0;
    while (sent < packet.size() && std::chrono::steady_clock::now() < deadline) {
        size_t count = 0;
        const CURLcode result = curl_easy_send(curl, packet.data() + sent,
                                              packet.size() - sent, &count);
        if (result == CURLE_AGAIN) {
            (void)waitSocket(socket, POLLOUT, 50);
            continue;
        }
        if (result != CURLE_OK || count == 0)
            return false;
        sent += count;
    }
    return sent == packet.size();
}

void invoke(const MqttClient::StateHandler &handler) {
    if (!handler)
        return;
    try { handler(); } catch (...) {}
}
} // namespace

MqttClient::MqttClient(MqttClientOptions options) : options_(std::move(options)) {
    if (!options_.port)
        options_.port = options_.tls ? 8883 : 1883;
}

MqttClient::~MqttClient() { stop(); }

void MqttClient::setHandlers(MessageHandler message, StateHandler connected,
                             StateHandler disconnected, ErrorHandler error) {
    std::lock_guard<std::mutex> lock(mutex_);
    messageHandler_ = std::move(message);
    connectedHandler_ = std::move(connected);
    disconnectedHandler_ = std::move(disconnected);
    errorHandler_ = std::move(error);
}

bool MqttClient::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_)
        return true;
    if (thread_.joinable())
        return false;
    if (options_.host.empty() || options_.port == 0 || options_.keepAliveSeconds == 0)
        return false;
    running_ = true;
    try {
        thread_ = std::thread(&MqttClient::run, this);
    } catch (...) {
        running_ = false;
        return false;
    }
    return true;
}

void MqttClient::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
    }
    condition_.notify_all();
    if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id())
        thread_.join();
}

bool MqttClient::publish(const std::string &topic, const std::string &payload,
                         bool retained) {
    if (!validTopicName(topic) ||
        topic.size() + payload.size() > kMaximumPacketBytes)
        return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_ || !connected_.load(std::memory_order_acquire) ||
        outbound_.size() >= kMaximumPendingPublishes)
        return false;
    Outbound item;
    item.topic = topic;
    item.payload = payload;
    item.retained = retained;
    if (retained) {
        if (payload.empty()) retained_.erase(topic);
        else retained_.insert(topic);
    }
    outbound_.push_back(std::move(item));
    condition_.notify_one();
    return true;
}

std::vector<std::string> MqttClient::retainedTopics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::vector<std::string>(retained_.begin(), retained_.end());
}

bool MqttClient::subscribe(const std::string &topic, unsigned qos) {
    if (!validTopicFilter(topic) || qos > 2)
        return false;
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = subscriptions_.find(topic);
    if (found != subscriptions_.end() && found->second == qos)
        return true;
    if (running_ && connected_.load(std::memory_order_acquire) &&
        outbound_.size() >= kMaximumPendingPublishes)
        return false;
    subscriptions_[topic] = qos;
    if (running_ && connected_.load(std::memory_order_acquire)) {
        Outbound item;
        item.topic = topic;
        item.qos = qos;
        item.subscription = true;
        outbound_.push_back(std::move(item));
    }
    condition_.notify_one();
    return true;
}

bool MqttClient::setWill(const std::string &payload) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!validMqttString(payload))
        return false;
    if (options_.willTopic.empty())
        return false;
    if (options_.willPayload == payload)
        return true;
    options_.willPayload = payload;
    if (connected_.load(std::memory_order_acquire))
        reconnectNow_ = true;
    condition_.notify_one();
    return true;
}

bool MqttClient::connected() const {
    return connected_.load(std::memory_order_acquire);
}

long long MqttClient::latencyMilliseconds() const {
    return latencyMs_.load(std::memory_order_acquire);
}

void MqttClient::notifyError(const std::string &message) const {
    ErrorHandler handler;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        handler = errorHandler_;
    }
    if (handler) {
        try { handler(message); } catch (...) {}
    }
}

void MqttClient::run() {
    while (true) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_)
                break;
        }
        try {
            (void)connectAndRun();
        } catch (const std::exception &exception) {
            notifyError(exception.what());
        } catch (...) {
            notifyError("MQTT client failed with an unknown error");
        }
        const bool wasConnected = connected_.exchange(false, std::memory_order_acq_rel);
        latencyMs_.store(-1, std::memory_order_release);
        if (wasConnected) {
            StateHandler handler;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                handler = disconnectedHandler_;
            }
            invoke(handler);
        }
        std::unique_lock<std::mutex> lock(mutex_);
        if (!running_)
            break;
        const unsigned delay = reconnectNow_ ? 0 : options_.reconnectDelayMs;
        reconnectNow_ = false;
        condition_.wait_for(lock, std::chrono::milliseconds(delay), [this] {
            return !running_ || reconnectNow_;
        });
        if (!running_)
            break;
        reconnectNow_ = false;
    }
}

bool MqttClient::connectAndRun() {
    const auto connectionStart = std::chrono::steady_clock::now();
    MqttClientOptions options;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        options = options_;
    }
    const std::string host = urlHost(options.host);
    if (host.empty()) {
        notifyError("invalid MQTT broker host");
        return false;
    }
    if (!validMqttString(options.username) || !validMqttString(options.password) ||
        !validMqttString(options.clientId) || !validMqttString(options.willTopic) ||
        !validMqttString(options.willPayload) || !options.keepAliveSeconds) {
        notifyError("invalid MQTT connection settings");
        return false;
    }

    std::vector<unsigned char> connectPacket;
    std::string packetError;
    if (!makeConnectPacket(options, connectPacket, packetError)) {
        notifyError(packetError);
        return false;
    }
    initializeCurl();
    using CurlHandle = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;
    CurlHandle curl(curl_easy_init(), &curl_easy_cleanup);
    if (!curl) {
        notifyError("could not create an MQTT network connection");
        return false;
    }
    const std::string url = std::string(options.tls ? "https://" : "http://") + host + ":" +
                            std::to_string(options.port) + "/";
    char errorBuffer[CURL_ERROR_SIZE] = {};
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_CONNECT_ONLY, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS,
                     static_cast<long>(options.connectTimeoutMs));
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS,
                     static_cast<long>(options.connectTimeoutMs));
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_PROXY, "");
    curl_easy_setopt(curl.get(), CURLOPT_ERRORBUFFER, errorBuffer);
    if (options.tls) {
        if (!options.caFile.empty())
            curl_easy_setopt(curl.get(), CURLOPT_CAINFO, options.caFile.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER,
                         options.tlsRejectUnauthorized ? 1L : 0L);
        curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST,
                         options.tlsRejectUnauthorized ? 2L : 0L);
    }
    CURLcode result = curl_easy_perform(curl.get());
    if (result != CURLE_OK) {
        notifyError(errorBuffer[0] ? errorBuffer : curl_easy_strerror(result));
        return false;
    }
    curl_socket_t socket = CURL_SOCKET_BAD;
    if (curl_easy_getinfo(curl.get(), CURLINFO_ACTIVESOCKET, &socket) != CURLE_OK ||
        socket == CURL_SOCKET_BAD) {
        notifyError("MQTT broker connection has no active socket");
        return false;
    }
    if (!sendAll(curl.get(), socket, connectPacket, options.connectTimeoutMs)) {
        notifyError("could not send MQTT CONNECT packet");
        return false;
    }

    std::vector<unsigned char> incoming;
    const auto connectDeadline = std::chrono::steady_clock::now() +
                                std::chrono::milliseconds(options.connectTimeoutMs);
    bool gotConnack = false;
    while (std::chrono::steady_clock::now() < connectDeadline) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_)
                return false;
        }
        if (waitSocket(socket, POLLIN, 100)) {
            unsigned char bytes[4096];
            size_t count = 0;
            result = curl_easy_recv(curl.get(), bytes, sizeof(bytes), &count);
            if (result == CURLE_AGAIN)
                continue;
            if (result != CURLE_OK || count == 0) {
                notifyError("MQTT broker closed during connection setup");
                return false;
            }
            incoming.insert(incoming.end(), bytes, bytes + count);
            size_t header = 0, body = 0;
            if (!parsePacketSize(incoming, header, body)) {
                if (incoming.size() < 5)
                    continue;
                notifyError("invalid MQTT packet from broker");
                return false;
            }
            if (incoming.size() < header + body)
                continue;
            if (incoming[0] != 0x20 || body != 2) {
                notifyError("broker did not send a valid MQTT CONNACK");
                return false;
            }
            const unsigned code = incoming[header + 1];
            if (code != 0) {
                static const char *errors[] = {"", "unacceptable protocol version",
                    "identifier rejected", "server unavailable", "bad username or password",
                    "not authorized"};
                notifyError(code < sizeof(errors) / sizeof(errors[0])
                                ? errors[code]
                                : "MQTT connection refused by broker");
                return false;
            }
            incoming.erase(incoming.begin(), incoming.begin() + header + body);
            gotConnack = true;
            break;
        }
    }
    if (!gotConnack) {
        notifyError("MQTT connection timed out waiting for CONNACK");
        return false;
    }

    const auto connectedAt = std::chrono::steady_clock::now();
    connected_.store(true, std::memory_order_release);
    latencyMs_.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                         connectedAt - connectionStart).count(),
                     std::memory_order_release);
    StateHandler connectedHandler;
    std::map<std::string, unsigned> subscriptions;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        connectedHandler = connectedHandler_;
        subscriptions = subscriptions_;
    }
    invoke(connectedHandler);

    unsigned short nextPacketId = 1;
    for (const auto &subscription : subscriptions) {
        std::vector<unsigned char> packet;
        if (!makeSubscribePacket(subscription.first, subscription.second,
                                 nextPacketId++, packet) ||
            !sendAll(curl.get(), socket, packet, 2000)) {
            notifyError("could not restore MQTT subscription");
            return false;
        }
        if (nextPacketId == 0)
            nextPacketId = 1;
    }

    auto lastReceived = std::chrono::steady_clock::now();
    auto lastPing = lastReceived;
    auto pingSent = lastReceived;
    auto pingInterval = std::chrono::seconds(std::max<unsigned>(1, options.keepAliveSeconds / 2));
    auto pingDeadline = std::chrono::steady_clock::time_point();
    bool running = true;
    while (running) {
        bool forceReconnect = false;
        std::vector<Outbound> outbound;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (!running_)
                running = false;
            forceReconnect = reconnectNow_;
            if (forceReconnect)
                reconnectNow_ = false;
            outbound.swap(outbound_);
            if (outbound.empty() && running && !forceReconnect)
                condition_.wait_for(lock, std::chrono::milliseconds(50), [this] {
                    return !running_ || reconnectNow_ || !outbound_.empty();
                });
            if (!running_)
                running = false;
            forceReconnect = forceReconnect || reconnectNow_;
            if (forceReconnect)
                reconnectNow_ = false;
            outbound.insert(outbound.end(), outbound_.begin(), outbound_.end());
            outbound_.clear();
        }
        if (!running)
            break;
        if (forceReconnect) {
            const std::vector<unsigned char> disconnect{0xe0, 0x00};
            (void)sendAll(curl.get(), socket, disconnect, 500);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                reconnectNow_ = true;
            }
            return true;
        }
        for (const Outbound &item : outbound) {
            std::vector<unsigned char> packet;
            const bool made = item.subscription
                ? makeSubscribePacket(item.topic, item.qos, nextPacketId++, packet)
                : makePublishPacket(item.topic, item.payload, item.retained, packet);
            if (nextPacketId == 0)
                nextPacketId = 1;
            if (!made || !sendAll(curl.get(), socket, packet, 2000)) {
                notifyError("could not send MQTT message");
                return false;
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - lastReceived > std::chrono::seconds(options.keepAliveSeconds) * 3 / 2) {
            notifyError("MQTT broker stopped responding to keep-alive");
            return false;
        }
        if (now - lastPing >= pingInterval) {
            const std::vector<unsigned char> ping{0xc0, 0x00};
            if (!sendAll(curl.get(), socket, ping, 1000)) {
                notifyError("could not send MQTT keep-alive");
                return false;
            }
            pingSent = now;
            pingDeadline = now + std::chrono::seconds(options.keepAliveSeconds);
            lastPing = now;
        }
        if (pingDeadline.time_since_epoch().count() && now > pingDeadline) {
            notifyError("MQTT broker did not answer keep-alive");
            return false;
        }
        if (!waitSocket(socket, POLLIN, 100))
            continue;

        unsigned char bytes[16384];
        size_t count = 0;
        result = curl_easy_recv(curl.get(), bytes, sizeof(bytes), &count);
        if (result == CURLE_AGAIN)
            continue;
        if (result != CURLE_OK || count == 0) {
            notifyError("MQTT broker connection closed");
            return false;
        }
        if (incoming.size() + count > kMaximumPacketBytes + 5) {
            notifyError("MQTT broker packet exceeds the size limit");
            return false;
        }
        incoming.insert(incoming.end(), bytes, bytes + count);
        lastReceived = std::chrono::steady_clock::now();
        while (!incoming.empty()) {
            size_t header = 0, body = 0;
            if (!parsePacketSize(incoming, header, body)) {
                if (incoming.size() < 5)
                    break;
                notifyError("invalid MQTT packet from broker");
                return false;
            }
            if (incoming.size() < header + body)
                break;
            const unsigned char first = incoming[0];
            const unsigned type = first >> 4;
            const unsigned flags = first & 0x0f;
            const unsigned char *packet = incoming.data() + header;
            if (type == 13 && body == 0) { // PINGRESP
                if (pingSent.time_since_epoch().count()) {
                    latencyMs_.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now() - pingSent).count(),
                                     std::memory_order_release);
                    pingDeadline = std::chrono::steady_clock::time_point();
                }
            } else if (type == 3 && body >= 2) { // PUBLISH
                const size_t topicSize = (static_cast<size_t>(packet[0]) << 8) | packet[1];
                const unsigned qos = (flags >> 1) & 0x03;
                size_t payloadOffset = 2 + topicSize;
                if (topicSize == 0 || payloadOffset > body || qos == 3) {
                    notifyError("invalid MQTT PUBLISH packet from broker");
                    return false;
                }
                unsigned short id = 0;
                if (qos > 0) {
                    if (payloadOffset + 2 > body) {
                        notifyError("truncated MQTT PUBLISH packet from broker");
                        return false;
                    }
                    id = static_cast<unsigned short>((packet[payloadOffset] << 8) |
                                                     packet[payloadOffset + 1]);
                    if (!id) {
                        notifyError("invalid MQTT packet identifier from broker");
                        return false;
                    }
                    payloadOffset += 2;
                    if (qos == 1) {
                        std::vector<unsigned char> ack;
                        (void)makeControlPacket(0x40, id, ack);
                        if (!sendAll(curl.get(), socket, ack, 1000))
                            return false;
                    } else {
                        std::vector<unsigned char> ack;
                        (void)makeControlPacket(0x50, id, ack); // PUBREC
                        if (!sendAll(curl.get(), socket, ack, 1000))
                            return false;
                    }
                }
                MqttMessage message;
                message.topic.assign(reinterpret_cast<const char *>(packet + 2), topicSize);
                message.payload.assign(reinterpret_cast<const char *>(packet + payloadOffset),
                                       body - payloadOffset);
                message.qos = qos;
                message.retained = (flags & 0x01) != 0;
                MessageHandler handler;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    handler = messageHandler_;
                }
                if (handler) {
                    try { handler(message); } catch (...) {}
                }
            } else if (type == 6 && body == 2) { // PUBREL (QoS 2)
                const unsigned short id = static_cast<unsigned short>((packet[0] << 8) | packet[1]);
                std::vector<unsigned char> ack;
                (void)makeControlPacket(0x70, id, ack); // PUBCOMP
                if (!sendAll(curl.get(), socket, ack, 1000))
                    return false;
            }
            incoming.erase(incoming.begin(), incoming.begin() + header + body);
        }
    }

    if (connected_.load(std::memory_order_acquire)) {
        const std::vector<unsigned char> disconnect{0xe0, 0x00};
        (void)sendAll(curl.get(), socket, disconnect, 500);
    }
    return true;
}

} // namespace daemon
} // namespace nativetv
