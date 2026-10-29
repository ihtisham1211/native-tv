#include "daemon/child_process.hpp"
#include "daemon/syslog_forwarder.hpp"
#include <arpa/inet.h>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <mutex>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace nativetv;
using namespace nativetv::daemon;

namespace {
void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}

void writeFile(const std::string &path, const std::string &value, bool append = false) {
    std::ofstream output(path.c_str(), append ? std::ios::app : std::ios::trunc);
    if (!output)
        throw std::runtime_error("could not write syslog fixture");
    output << value;
}

uint16_t startReceiver(int &descriptor) {
    descriptor = socket(AF_INET, SOCK_DGRAM, 0);
    if (descriptor < 0)
        throw std::runtime_error("could not create syslog receiver socket");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(descriptor, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0)
        throw std::runtime_error("could not bind syslog receiver socket");
    socklen_t length = sizeof(address);
    if (getsockname(descriptor, reinterpret_cast<sockaddr *>(&address), &length) != 0)
        throw std::runtime_error("could not read syslog receiver port");
    return ntohs(address.sin_port);
}

// An IPv6 loopback receiver, or -1 where the host has none.
int startReceiver6(uint16_t &port) {
    const int descriptor = socket(AF_INET6, SOCK_DGRAM, 0);
    if (descriptor < 0)
        return -1;
    sockaddr_in6 address{};
    address.sin6_family = AF_INET6;
    address.sin6_addr = in6addr_loopback;
    socklen_t length = sizeof(address);
    if (bind(descriptor, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
        getsockname(descriptor, reinterpret_cast<sockaddr *>(&address), &length) != 0) {
        close(descriptor);
        return -1;
    }
    port = ntohs(address.sin6_port);
    return descriptor;
}

bool waitFor(const std::function<bool()> &condition, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return condition();
}
} // namespace

int main() {
    int receiver = -1;
    std::string root;
    try {
        const uint16_t port = startReceiver(receiver);
        char temporary[] = "/tmp/nativetv-syslog-forwarder-XXXXXX";
        const char *created = mkdtemp(temporary);
        if (!created)
            throw std::runtime_error("could not create syslog fixture root");
        root = created;
        if (mkdir((root + "/run").c_str(), 0700) != 0)
            throw std::runtime_error("could not create syslog runtime fixture");
        const std::string systemPath = root + "/messages";
        const std::string nativeTvPath = root + "/tvweb.log";
        const std::string markerPath = root + "/run/boot-sent";
        writeFile(systemPath,
                  "2026-10-26T10:00:00.000Z [1.000000] user.info app [] connected to 192.168.1.50\n");
        writeFile(nativeTvPath, "");

        Json config = Json::object();
        Json device = Json::object();
        device.set("name", Json::text("Living Room"));
        config.set("device", device);
        Json syslog = Json::object();
        syslog.set("server", Json::text("127.0.0.1"));
        syslog.set("port", Json::numeric(port));
        Json sources = Json::array();
        sources.add(Json::text("system"));
        sources.add(Json::text("kernel"));
        syslog.set("sources", sources);
        syslog.set("redact", Json::flag(true));
        config.set("syslog", syslog);

        std::mutex statusMutex;
        Json latestStatus;
        unsigned kernelCalls = 0;
        SyslogForwarderOptions options;
        options.systemLogPath = systemPath;
        options.nativeTvLogPath = nativeTvPath;
        options.bootMarkerPath = markerPath;
        options.pollIntervalMs = 60;
        options.kernelPollIntervalMs = 120;
        options.wallClockMs = [] { return 1793000000000.0; };
        options.uptimeSeconds = [] { return 100.0; };
        options.kernelReader = [&kernelCalls](std::string &error) {
            ++kernelCalls;
            if (kernelCalls == 1) {
                error = "fixture dmesg failure";
                return std::string();
            }
            return std::string("<6>[ 1.500000 ] gpu: initialized\n");
        };
        options.statusChanged = [&statusMutex, &latestStatus](const Json &status) {
            std::lock_guard<std::mutex> lock(statusMutex);
            latestStatus = status;
        };
        ChildProcessRunner children;
        SyslogForwarder forwarder(config, children, nullptr, options);
        check(forwarder.start(), "configured native syslog forwarder should start");

        std::vector<std::string> packets;
        auto collectPackets = [&]() {
            for (;;) {
                pollfd item{};
                item.fd = receiver;
                item.events = POLLIN;
                if (poll(&item, 1, 0) <= 0)
                    break;
                char buffer[4096];
                const ssize_t count = recv(receiver, buffer, sizeof(buffer), 0);
                if (count <= 0)
                    break;
                packets.emplace_back(buffer, static_cast<size_t>(count));
            }
        };
        check(waitFor([&]() {
                  collectPackets();
                  const Json counters = forwarder.counters();
                  return counters["messages"]["system"].number() == 1 &&
                         counters["messages"]["kernel"].number() == 1 &&
                         access(markerPath.c_str(), F_OK) == 0;
              }, std::chrono::seconds(4)),
              "system and kernel entries should be sent and the boot marker written after recovery");
        check(kernelCalls >= 2, "a failed kernel read should be retried");
        // A counter moves before its datagram is readable.
        (void)waitFor([&]() { collectPackets(); return packets.size() >= 2; },
                      std::chrono::seconds(1));
        check(packets.size() == 2, "boot system and kernel entries should each send once");
        check(packets[0].find("<14>1 2026-10-26T10:00:00.000Z Living-Room app - system -") == 0 &&
                  packets[0].find("192.168.1.50") == std::string::npos &&
                  packets[0].find("<IP>") != std::string::npos,
              "system datagrams preserve RFC 5424 fields and redact sensitive addresses");
        check(packets[1].find("<6>1 ") == 0 &&
                  packets[1].find(" Living-Room kernel - kernel - gpu: initialized") !=
                      std::string::npos,
              "kernel dmesg entries use their native facility and process name");

        writeFile(systemPath,
                  "2026-10-26T10:00:01.000Z [2.000000] daemon.warn app [] second line\n", true);
        forwarder.flush();
        check(waitFor([&]() {
                  collectPackets();
                  return forwarder.counters()["messages"]["system"].number() == 2 &&
                         packets.size() >= 3;
              }, std::chrono::seconds(2)),
              "flush should deliver newly appended system log entries promptly");
        check(packets.size() == 3 && packets[2].find("<28>1 ") == 0 &&
                  packets[2].find("second line") != std::string::npos,
              "incremental system reads keep facility and severity priority");
        {
            std::lock_guard<std::mutex> lock(statusMutex);
            check(latestStatus["server"].string() == "127.0.0.1" &&
                      latestStatus["port"].number() == port &&
                      latestStatus["messages"]["system"].number() == 2 &&
                      latestStatus["error"].valid() == false,
                  "status callback reports destination, counters and recovered error state");
        }

        forwarder.stop();

        // test-syslog.js 7: an address the lookup gives as IPv6 is sent to over udp6.
        uint16_t port6 = 0;
        const int receiver6 = startReceiver6(port6);
        if (receiver6 >= 0) {
            const std::string messages6 = root + "/messages6";
            writeFile(messages6, "");
            Json config6 = Json::object();
            Json device6 = Json::object();
            device6.set("name", Json::text("bedroom tv"));
            config6.set("device", device6);
            Json syslog6 = Json::object();
            syslog6.set("server", Json::text("::1"));
            syslog6.set("port", Json::numeric(port6));
            Json sources6 = Json::array();
            sources6.add(Json::text("system"));
            syslog6.set("sources", sources6);
            config6.set("syslog", syslog6);
            SyslogForwarderOptions options6;
            options6.systemLogPath = messages6;
            options6.nativeTvLogPath = nativeTvPath;
            options6.bootMarkerPath = root + "/run/boot-sent6";
            options6.pollIntervalMs = 60;
            options6.wallClockMs = [] { return 1793000000000.0; };
            options6.uptimeSeconds = [] { return 4000.0; };
            ChildProcessRunner children6;
            SyslogForwarder forwarder6(config6, children6, nullptr, options6);
            check(forwarder6.start(), "an IPv6 syslog destination should start");
            writeFile(messages6,
                      "2026-10-08T12:00:03.000000Z [3.000000] user.info WebAppMgr [] over IPv6\n", true);
            forwarder6.flush();
            std::string datagram;
            check(waitFor([&]() {
                      pollfd item{};
                      item.fd = receiver6;
                      item.events = POLLIN;
                      if (poll(&item, 1, 0) <= 0)
                          return false;
                      char buffer[4096];
                      const ssize_t count = recv(receiver6, buffer, sizeof(buffer), 0);
                      if (count > 0)
                          datagram.assign(buffer, static_cast<size_t>(count));
                      return count > 0 && datagram.find("over IPv6") != std::string::npos;
                  }, std::chrono::seconds(4)),
                  "an IPv6 address should be sent to over udp6");
            check(datagram.find(" bedroom-tv WebAppMgr - system - over IPv6") != std::string::npos,
                  "the IPv6 datagram carries the configured hostname");
            forwarder6.stop();
            close(receiver6);
            unlink(messages6.c_str());
            unlink((root + "/run/boot-sent6").c_str());
        } else {
            std::printf("no IPv6 loopback here; the udp6 send is not tested\n");
        }

        close(receiver);
        receiver = -1;
        unlink(systemPath.c_str());
        unlink(nativeTvPath.c_str());
        unlink(markerPath.c_str());
        rmdir((root + "/run").c_str());
        rmdir(root.c_str());
        std::printf("native syslog forwarder runtime check passed\n");
        return 0;
    } catch (const std::exception &error) {
        if (receiver >= 0)
            close(receiver);
        if (!root.empty()) {
            unlink((root + "/messages").c_str());
            unlink((root + "/tvweb.log").c_str());
            unlink((root + "/run/boot-sent").c_str());
            rmdir((root + "/run").c_str());
            rmdir(root.c_str());
        }
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
