#include "daemon/mqtt_client.hpp"
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstdio>
#include <mutex>
#include <string>

int main(int argc, char **argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: mqtt-tls-probe https://HOST:PORT/ CA_FILE\n");
        return 2;
    }
    const std::string url(argv[1]);
    const std::string scheme = "https://";
    if (url.compare(0, scheme.size(), scheme) != 0) {
        std::fprintf(stderr, "TLS connection failed: expected an https URL\n");
        return 1;
    }
    const size_t hostStart = scheme.size();
    const size_t colon = url.find(':', hostStart);
    const size_t slash = url.find('/', hostStart);
    if (colon == std::string::npos ||
        (slash != std::string::npos && colon > slash)) {
        std::fprintf(stderr, "TLS connection failed: invalid broker URL\n");
        return 1;
    }
    const size_t hostEnd = url.find('/', hostStart);
    const std::string hostPort = url.substr(hostStart,
        (hostEnd == std::string::npos ? url.size() : hostEnd) - hostStart);
    const size_t hostColon = hostPort.rfind(':');
    if (hostColon == std::string::npos) {
        std::fprintf(stderr, "TLS connection failed: missing broker port\n");
        return 1;
    }
    const std::string host = hostPort.substr(0, hostColon);
    const std::string portText = hostPort.substr(hostColon + 1);
    char *end = nullptr;
    const unsigned long port = std::strtoul(portText.c_str(), &end, 10);
    if (host.empty() || !end || *end || port == 0 || port > 65535) {
        std::fprintf(stderr, "TLS connection failed: invalid broker host or port\n");
        return 1;
    }

    nativetv::daemon::MqttClientOptions options;
    options.host = host;
    options.port = static_cast<unsigned short>(port);
    options.tls = true;
    options.caFile = argv[2];
    options.clientId = "glass";
    options.connectTimeoutMs = 2000;
    options.reconnectDelayMs = 100;
    nativetv::daemon::MqttClient client(options);
    std::mutex mutex;
    std::condition_variable condition;
    bool connected = false;
    std::string error;
    client.setHandlers({}, [&] {
        std::lock_guard<std::mutex> lock(mutex);
        connected = true;
        condition.notify_all();
    }, {}, [&](const std::string &message) {
        std::lock_guard<std::mutex> lock(mutex);
        if (error.empty())
            error = message;
        condition.notify_all();
    });
    if (!client.start()) {
        std::fprintf(stderr, "TLS connection failed: could not start MQTT client\n");
        return 1;
    }
    {
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait_for(lock, std::chrono::seconds(5), [&] {
            return connected || !error.empty();
        });
    }
    client.stop();
    if (!connected) {
        std::fprintf(stderr, "TLS connection failed: %s\n",
                     error.empty() ? "connection timed out" : error.c_str());
        return 1;
    }
    return 0;
}
