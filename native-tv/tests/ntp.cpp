#include "service/ntp.hpp"
#include <arpa/inet.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace nativetv;
using namespace nativetv::service;

namespace {
unsigned checks = 0;

void check(bool value, const char *message) {
    ++checks;
    if (!value)
        throw std::runtime_error(message);
}

uint16_t bindLoopback(int &descriptor) {
    descriptor = socket(AF_INET, SOCK_DGRAM, 0);
    if (descriptor < 0)
        throw std::runtime_error("could not create UDP fixture socket");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(descriptor, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0)
        throw std::runtime_error("could not bind UDP fixture socket");
    socklen_t length = sizeof(address);
    if (getsockname(descriptor, reinterpret_cast<sockaddr *>(&address), &length) != 0)
        throw std::runtime_error("could not read UDP fixture port");
    return ntohs(address.sin_port);
}

std::array<uint8_t, 48> replyFor(const std::array<uint8_t, 48> &request,
                                 double serverMs, unsigned leap = 0,
                                 unsigned mode = 4, unsigned stratum = 2) {
    std::array<uint8_t, 48> reply{};
    reply[0] = static_cast<uint8_t>((leap << 6) | (4 << 3) | mode);
    reply[1] = static_cast<uint8_t>(stratum);
    std::memcpy(reply.data() + 24, request.data() + 40, 8);
    writeNtpTimestamp(reply.data(), 32, serverMs);
    writeNtpTimestamp(reply.data(), 40, serverMs);
    return reply;
}
} // namespace

int main() {
    try {
        const double now = 1791576000250.0;
        std::array<uint8_t, 48> timestamp{};
        writeNtpTimestamp(timestamp.data(), 0, now);
        check(std::abs(readNtpTimestamp(timestamp.data(), 0) - now) < 0.001,
              "NTP timestamp round trip");
        const double eraOne = 2085978500000.0;
        writeNtpTimestamp(timestamp.data(), 0, eraOne);
        check(timestamp[0] == 0 && timestamp[1] == 0 && timestamp[2] == 0 &&
                  timestamp[3] == 4 && readNtpTimestamp(timestamp.data(), 0) == eraOne,
              "NTP era one timestamps");

        const auto request = buildNtpRequest(now);
        check(request.size() == 48 && request[0] == 0x23 &&
                  std::abs(readNtpTimestamp(request.data(), 40) - now) < 0.02,
              "NTP client request fields and transmit time");
        bool randomBitsDiffer = false;
        for (unsigned index = 0; index < 8; ++index) {
            const auto another = buildNtpRequest(now);
            if (another[46] != request[46] || another[47] != request[47])
                randomBitsDiffer = true;
        }
        check(randomBitsDiffer, "NTP request nonce bits vary");

        const auto goodReply = replyFor(request, now + 10020.0);
        check(ntpRequestAnswered(goodReply.data(), goodReply.size(), request) &&
                  !ntpRequestAnswered(goodReply.data(), goodReply.size() - 1, request),
              "NTP reply must echo the request transmit timestamp");
        const NtpResult parsed = parseNtpReply(goodReply.data(), goodReply.size(),
                                               now, now + 40, 5000);
        check(parsed.ok && std::abs(parsed.answer.offsetMs - 10000) < 0.001 &&
                  std::abs(parsed.answer.roundTripMs - 40) < 0.001 &&
                  std::abs(parsed.answer.atMs - (now + 10040)) < 0.001 &&
                  parsed.answer.atMonoMs == 5000 && parsed.answer.stratum == 2,
              "NTP offset, round trip and server-at-receive time");

        const auto modeError = replyFor(request, now, 0, 3, 2);
        const auto lowStratum = replyFor(request, now, 0, 4, 0);
        const auto highStratum = replyFor(request, now, 0, 4, 16);
        const auto unsynchronised = replyFor(request, now, 3, 4, 2);
        auto noTransmit = replyFor(request, now);
        std::fill(noTransmit.begin() + 40, noTransmit.end(), 0);
        check(parseNtpReply(modeError.data(), 48, now, now + 40, 0).error.find("mode 3") !=
                      std::string::npos &&
                  parseNtpReply(lowStratum.data(), 48, now, now + 40, 0).error.find("stratum 0") !=
                      std::string::npos &&
                  parseNtpReply(highStratum.data(), 48, now, now + 40, 0).error.find("stratum 16") !=
                      std::string::npos &&
                  parseNtpReply(unsynchronised.data(), 48, now, now + 40, 0).error.find("leap") !=
                      std::string::npos &&
                  parseNtpReply(noTransmit.data(), 48, now, now + 40, 0).error.find("no transmit") !=
                      std::string::npos &&
                  parseNtpReply(goodReply.data(), 47, now, now + 40, 0).error.find("short") !=
                      std::string::npos,
              "NTP rejects malformed, unsynchronised and non-server replies");
        const auto slowReply = replyFor(request, now + 1500);
        check(parseNtpReply(slowReply.data(), 48, now, now + 3000, 0).error.find("round trip") !=
                      std::string::npos,
              "NTP rejects uncertain long round trips");
        const auto oldReply = replyFor(request, 1672531200000.0);
        check(parseNtpReply(oldReply.data(), 48, now, now + 40, 0).error.find("before 2026") !=
                  std::string::npos,
              "NTP rejects clock values before the release floor");

        NtpServerInput input = parseNtpServerInput("  time.lan:1123 ");
        check(input.configured && input.valid && input.settings.server == "time.lan" &&
                  input.settings.port == 1123,
              "NTP server input accepts a hostname and port");
        input = parseNtpServerInput("fd00::1");
        check(input.valid && input.settings.server == "fd00::1" && input.settings.port == 123,
              "NTP server input accepts a bare IPv6 address");
        input = parseNtpServerInput("[fd00::1]:123");
        check(input.valid && input.settings.server == "fd00::1" && input.settings.port == 123,
              "NTP server input accepts bracketed IPv6");
        check(parseNtpServerInput("time.lan:99999").error == "port" &&
                  parseNtpServerInput("time lan").error == "host" &&
                  parseNtpServerInput("[time.lan]:123").error == "host" &&
                  parseNtpServerInput("[fd00::1]:123x").error == "host" &&
                  !parseNtpServerInput("  ").configured,
              "NTP server input rejects bad hosts and ports, and accepts empty-to-disable");

        NtpSettings settings;
        check(readNtpSettings(Json::parse(
                  "{\"ntp\":{\"server\":\" time.lan \",\"port\":123}}"), settings) &&
                  settings.server == "time.lan" && settings.port == 123,
              "NTP runtime settings match the saved config");
        check(!readNtpSettings(Json::parse("{\"ntp\":{\"server\":\"time.lan\",\"enabled\":false}}"),
                               settings),
              "NTP disabled setting keeps the server but does not activate it");

        NtpAnswer answer;
        answer.atMs = now + 600;
        answer.atMonoMs = 1234567.891;
        const Json payload = ntpSetTimePayload(answer);
        check(payload["utc"].number() == std::round((now + 600) / 1000) &&
                  payload["source"].string() == "sdp" && payload["available"].boolean() &&
                  payload["timestamp"]["source"].string() == "monotonic" &&
                  payload["timestamp"]["sec"].number() == 1234 &&
                  std::abs(payload["timestamp"]["nsec"].number() - 567891000) < 1000,
              "NTP clock/setTime payload uses monotonic time");

        int server = -1;
        const uint16_t port = bindLoopback(server);
        std::atomic<bool> received{false};
        std::thread responder([server, &received] {
            std::array<uint8_t, 512> buffer{};
            sockaddr_in client{};
            socklen_t length = sizeof(client);
            const ssize_t count = recvfrom(server, buffer.data(), buffer.size(), 0,
                                           reinterpret_cast<sockaddr *>(&client), &length);
            if (count < 48)
                return;
            std::array<uint8_t, 48> requestPacket{};
            std::memcpy(requestPacket.data(), buffer.data(), requestPacket.size());
            const double sentTime = readNtpTimestamp(requestPacket.data(), 40);
            const auto response = replyFor(requestPacket, sentTime + 50.0);
            received = sendto(server, response.data(), response.size(), 0,
                              reinterpret_cast<sockaddr *>(&client), length) ==
                       static_cast<ssize_t>(response.size());
        });
        NtpSettings localSettings{"127.0.0.1", port};
        const NtpResult network = queryNtp(localSettings, std::chrono::milliseconds(1000));
        responder.join();
        close(server);
        check(received && network.ok && network.answer.stratum == 2 &&
                  std::abs(network.answer.offsetMs - 50.0) < 100.0,
              "NTP client exchanges a validated packet over loopback UDP");

        int silentServer = -1;
        const uint16_t silentPort = bindLoopback(silentServer);
        const NtpResult timeout = queryNtp(NtpSettings{"127.0.0.1", silentPort},
                                           std::chrono::milliseconds(30));
        close(silentServer);
        check(!timeout.ok && timeout.error.find("no answer") != std::string::npos,
              "NTP query times out when its server is silent");

        std::printf("%u native NTP checks passed\n", checks);
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
