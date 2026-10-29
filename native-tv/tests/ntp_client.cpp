#include "daemon/child_process.hpp"
#include "daemon/event_loop.hpp"
#include "daemon/logger.hpp"
#include "daemon/luna.hpp"
#include "daemon/ntp_client.hpp"
#include <arpa/inet.h>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <sys/stat.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef NATIVETV_FAKE_LUNA_SEND
#error NATIVETV_FAKE_LUNA_SEND must name the fake luna-send executable
#endif

using namespace nativetv;
using namespace nativetv::daemon;

namespace {
void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}

uint32_t read32(const uint8_t *packet, size_t offset) {
    return (static_cast<uint32_t>(packet[offset]) << 24) |
           (static_cast<uint32_t>(packet[offset + 1]) << 16) |
           (static_cast<uint32_t>(packet[offset + 2]) << 8) |
           static_cast<uint32_t>(packet[offset + 3]);
}

void write32(uint8_t *packet, size_t offset, uint32_t value) {
    packet[offset] = static_cast<uint8_t>(value >> 24);
    packet[offset + 1] = static_cast<uint8_t>(value >> 16);
    packet[offset + 2] = static_cast<uint8_t>(value >> 8);
    packet[offset + 3] = static_cast<uint8_t>(value);
}

void writeTimestamp(uint8_t *packet, size_t offset, double milliseconds) {
    const double seconds = std::floor(milliseconds / 1000.0);
    const double fraction = std::floor((milliseconds - seconds * 1000.0) / 1000.0 *
                                       4294967296.0);
    const uint64_t ntpSeconds = static_cast<uint64_t>(static_cast<int64_t>(seconds) +
                                                       2208988800LL);
    write32(packet, offset, static_cast<uint32_t>(ntpSeconds));
    write32(packet, offset + 4, static_cast<uint32_t>(fraction));
}

uint16_t startNtpServer(int &descriptor) {
    descriptor = socket(AF_INET, SOCK_DGRAM, 0);
    if (descriptor < 0)
        throw std::runtime_error("could not create NTP client fixture socket");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(descriptor, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0)
        throw std::runtime_error("could not bind NTP client fixture socket");
    socklen_t length = sizeof(address);
    if (getsockname(descriptor, reinterpret_cast<sockaddr *>(&address), &length) != 0)
        throw std::runtime_error("could not read NTP client fixture port");
    return ntohs(address.sin_port);
}
} // namespace

int main() {
    try {
        int server = -1;
        const uint16_t port = startNtpServer(server);
        std::thread responder([server] {
            uint8_t request[512]{};
            sockaddr_in client{};
            socklen_t clientLength = sizeof(client);
            const ssize_t count = recvfrom(server, request, sizeof(request), 0,
                                           reinterpret_cast<sockaddr *>(&client), &clientLength);
            if (count < 48)
                return;
            uint8_t reply[48]{};
            reply[0] = static_cast<uint8_t>((4 << 3) | 4);
            reply[1] = 2;
            std::memcpy(reply + 24, request + 40, 8);
            const uint32_t seconds = read32(request + 40, 0);
            const uint32_t fraction = read32(request + 40, 4);
            double unixMs = (static_cast<double>(seconds) - 2208988800.0) * 1000.0 +
                            static_cast<double>(fraction) / 4294967296.0 * 1000.0;
            unixMs += 40.0;
            writeTimestamp(reply, 32, unixMs);
            writeTimestamp(reply, 40, unixMs);
            (void)sendto(server, reply, sizeof(reply), 0,
                         reinterpret_cast<sockaddr *>(&client), clientLength);
        });

        EventLoop loop(4, 32, 64);
        ChildRunnerOptions childOptions;
        childOptions.startupGap = std::chrono::milliseconds(0);
        childOptions.regularGap = std::chrono::milliseconds(0);
        childOptions.startupWindow = std::chrono::milliseconds(0);
        ChildProcessRunner children(childOptions);
        LoggerOptions loggerOptions;
        loggerOptions.environmentLevel = "debug";
        loggerOptions.sink = [](LogLevel, const std::string &, bool) {};
        Logger logger(loggerOptions);
        LunaClientOptions lunaOptions;
        lunaOptions.executable = NATIVETV_FAKE_LUNA_SEND;
        LunaClient luna(loop, children, &logger, lunaOptions);
        NtpClient client(loop, luna, &logger);

        Json config = Json::object();
        Json ntp = Json::object();
        ntp.set("server", Json::text("127.0.0.1"));
        ntp.set("port", Json::numeric(port));
        config.set("ntp", ntp);
        char configDirectory[] = "/tmp/nativetv-ntp-client-XXXXXX";
        const char *created = mkdtemp(configDirectory);
        if (!created)
            throw std::runtime_error("could not create NTP config fixture");
        const std::string configPath = std::string(created) + "/config.json";
        {
            std::ofstream output(configPath.c_str());
            output << config.dump();
        }
        check(client.watchConfig(configPath, config), "configured native NTP watcher should start");

        std::thread loopThread([&loop] { loop.run(); });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        Json status;
        do {
            status = client.status();
            if (status["sets"].number() == 1)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } while (std::chrono::steady_clock::now() < deadline);

        check(status["sets"].number() == 1 && status["errors"].number() == 0 &&
                  status["server"].string() == "127.0.0.1" &&
                  status["port"].number() == port && status["last"]["stratum"].number() == 2 &&
                  status["error"].isObject() == false && !status["error"].valid(),
              "native client sets the clock through Luna and exposes status");
        ntp.set("enabled", Json::flag(false));
        config.set("ntp", ntp);
        {
            std::ofstream output(configPath.c_str(), std::ios::trunc);
            output << config.dump();
        }
        const auto disabledDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (client.status().valid() && std::chrono::steady_clock::now() < disabledDeadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        check(!client.status().valid(), "saved NTP disable setting should stop the native client");
        client.stop();
        loop.stop();
        loopThread.join();
        responder.join();
        close(server);

        check(!client.status().valid(), "stopped NTP client has no status");
        unlink(configPath.c_str());
        rmdir(created);
        std::printf("native NTP client runtime check passed\n");
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
