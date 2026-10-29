#include "service/ntp.hpp"
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <random>

namespace nativetv {
namespace service {
namespace {
const uint64_t NTP_UNIX_OFFSET = 2208988800ULL;
const double CLOCK_FLOOR_MS = 1767225600000.0;

std::string trim(const std::string &value) {
    size_t first = 0;
    size_t last = value.size();
    while (first < last && std::isspace(static_cast<unsigned char>(value[first])))
        ++first;
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1])))
        --last;
    return value.substr(first, last - first);
}

std::string numberText(const Json &value) {
    if (value.isString())
        return value.string();
    if (value.isNumber())
        return compactNumber(value.number(), 6);
    return "";
}

long parseInteger(const std::string &value, long fallback) {
    char *end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    return end == value.c_str() ? fallback : parsed;
}

uint32_t read32(const uint8_t *packet, size_t offset) {
    return (static_cast<uint32_t>(packet[offset]) << 24) |
           (static_cast<uint32_t>(packet[offset + 1]) << 16) |
           (static_cast<uint32_t>(packet[offset + 2]) << 8) |
           static_cast<uint32_t>(packet[offset + 3]);
}

void write32(uint8_t *packet, size_t offset, uint32_t value) {
    packet[offset] = static_cast<uint8_t>((value >> 24) & 0xff);
    packet[offset + 1] = static_cast<uint8_t>((value >> 16) & 0xff);
    packet[offset + 2] = static_cast<uint8_t>((value >> 8) & 0xff);
    packet[offset + 3] = static_cast<uint8_t>(value & 0xff);
}

std::string isoTimestamp(double milliseconds) {
    const long long rounded = static_cast<long long>(std::llround(milliseconds));
    const std::time_t seconds = static_cast<std::time_t>(rounded / 1000);
    long millis = static_cast<long>(rounded % 1000);
    if (millis < 0)
        millis += 1000;
    std::tm utc{};
    if (!gmtime_r(&seconds, &utc))
        return "invalid time";
    char buffer[24];
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &utc) == 0)
        return "invalid time";
    std::ostringstream output;
    output << buffer << '.' << (millis / 100) << ((millis / 10) % 10) << (millis % 10)
           << 'Z';
    return output.str();
}

double wallMilliseconds() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

double monotonicMilliseconds() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void fillRandom(uint8_t *destination, size_t size) {
    const int descriptor = open("/dev/urandom", O_RDONLY);
    if (descriptor >= 0) {
        size_t readBytes = 0;
        while (readBytes < size) {
            const ssize_t count = read(descriptor, destination + readBytes, size - readBytes);
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                break;
            readBytes += static_cast<size_t>(count);
        }
        close(descriptor);
        if (readBytes == size)
            return;
    }
    static std::random_device random;
    for (size_t index = 0; index < size; ++index)
        destination[index] = static_cast<uint8_t>(random());
}
} // namespace

NtpServerInput parseNtpServerInput(const std::string &text) {
    NtpServerInput result;
    const std::string input = trim(text);
    if (input.empty())
        return result;
    result.configured = true;

    std::string host = input;
    long port = 123;
    bool hasPort = false;
    const bool bracketedHost = input[0] == '[';
    if (bracketedHost) {
        const size_t close = input.find(']');
        if (close != std::string::npos) {
            host = input.substr(1, close - 1);
            const std::string suffix = input.substr(close + 1);
            if (!suffix.empty()) {
                if (suffix[0] != ':' || suffix.size() == 1) {
                    result.valid = false;
                    result.error = "host";
                    return result;
                }
                hasPort = true;
                const std::string portText = suffix.substr(1);
                if (!std::all_of(portText.begin(), portText.end(), [](unsigned char c) {
                        return std::isdigit(c);
                    })) {
                    result.valid = false;
                    result.error = "host";
                    return result;
                }
                port = parseInteger(portText, -1);
            }
        }
    } else {
        const size_t colon = input.rfind(':');
        if (colon != std::string::npos && colon > 0 && colon + 1 < input.size() &&
            input.find(':') == colon) {
            const std::string suffix = input.substr(colon + 1);
            if (std::all_of(suffix.begin(), suffix.end(), [](unsigned char c) {
                    return std::isdigit(c);
                })) {
                host = input.substr(0, colon);
                hasPort = true;
                port = parseInteger(suffix, -1);
            }
        }
    }

    if (hasPort && (port < 1 || port > 65535)) {
        result.valid = false;
        result.error = "port";
        return result;
    }
    const bool allowed = !host.empty() && host.size() <= 253 &&
                         std::all_of(host.begin(), host.end(), [](unsigned char c) {
                             return std::isalnum(c) || c == '.' || c == '-' || c == ':';
                         }) && host[0] != '.' && host[0] != '-';
    const bool bracketedAllowed = !bracketedHost ||
        std::all_of(host.begin(), host.end(), [](unsigned char c) {
            return std::isxdigit(c) || c == ':' || c == '.';
        });
    if (!allowed || !bracketedAllowed) {
        result.valid = false;
        result.error = "host";
        return result;
    }
    result.settings.server = host;
    result.settings.port = static_cast<uint16_t>(hasPort ? port : 123);
    return result;
}

bool readNtpSettings(const Json &config, NtpSettings &settings) {
    const Json ntp = config["ntp"];
    if (!ntp.isObject())
        return false;
    const std::string server = trim(ntp["server"].string());
    if (server.empty() || (ntp["enabled"].isBoolean() && !ntp["enabled"].boolean()))
        return false;
    long port = parseInteger(numberText(ntp["port"]), 123);
    if (port < 1 || port > 65535)
        port = 123;
    settings.server = server;
    settings.port = static_cast<uint16_t>(port);
    return true;
}

std::array<uint8_t, 48> buildNtpRequest(double nowMs) {
    std::array<uint8_t, 48> packet{};
    packet[0] = 0x23; // LI=0, version=4, client mode=3.
    writeNtpTimestamp(packet.data(), 40, nowMs);
    fillRandom(packet.data() + 46, 2);
    return packet;
}

bool ntpRequestAnswered(const uint8_t *reply, size_t replySize,
                        const std::array<uint8_t, 48> &request) {
    return reply && replySize >= 48 &&
           std::memcmp(reply + 24, request.data() + 40, 8) == 0;
}

double readNtpTimestamp(const uint8_t *packet, size_t offset) {
    uint64_t seconds = read32(packet, offset);
    const uint64_t fraction = read32(packet, offset + 4);
    if (seconds < 0x80000000ULL)
        seconds += 0x100000000ULL;
    return (static_cast<double>(seconds) - static_cast<double>(NTP_UNIX_OFFSET)) * 1000.0 +
           static_cast<double>(fraction) / 4294967296.0 * 1000.0;
}

void writeNtpTimestamp(uint8_t *packet, size_t offset, double unixMs) {
    const double whole = std::floor(unixMs / 1000.0);
    const double fraction = std::floor((unixMs - whole * 1000.0) / 1000.0 * 4294967296.0);
    const uint64_t seconds = static_cast<uint64_t>(static_cast<int64_t>(whole) +
                                                   static_cast<int64_t>(NTP_UNIX_OFFSET));
    write32(packet, offset, static_cast<uint32_t>(seconds & 0xffffffffULL));
    write32(packet, offset + 4, static_cast<uint32_t>(fraction));
}

NtpResult parseNtpReply(const uint8_t *reply, size_t replySize, double t1Ms,
                        double t4Ms, double t4MonoMs) {
    NtpResult result;
    if (!reply || replySize < 48) {
        result.error = "not a server reply (short packet)";
        return result;
    }
    const unsigned leap = reply[0] >> 6;
    const unsigned mode = reply[0] & 7;
    const unsigned stratum = reply[1];
    if (mode != 4) {
        result.error = "not a server reply (mode " + std::to_string(mode) + ")";
        return result;
    }
    if (stratum < 1 || stratum > 15) {
        result.error = "server not synchronised (stratum " + std::to_string(stratum) + ")";
        return result;
    }
    if (leap == 3) {
        result.error = "server not synchronised (leap indicator 3)";
        return result;
    }
    if (read32(reply, 40) == 0 && read32(reply, 44) == 0) {
        result.error = "reply has no transmit time";
        return result;
    }

    const double receiveMs = readNtpTimestamp(reply, 32);
    const double transmitMs = readNtpTimestamp(reply, 40);
    const double offset = ((receiveMs - t1Ms) + (transmitMs - t4Ms)) / 2.0;
    const double roundTrip = (t4Ms - t1Ms) - (transmitMs - receiveMs);
    const double serverMs = t4Ms + offset;
    if (roundTrip > 1000.0) {
        result.error = "round trip too long (" +
                       std::to_string(static_cast<long>(std::llround(roundTrip))) + " ms)";
        return result;
    }
    if (serverMs < CLOCK_FLOOR_MS) {
        result.error = "server time " + isoTimestamp(serverMs) + " is before 2026";
        return result;
    }
    result.ok = true;
    result.answer.atMs = serverMs;
    result.answer.atMonoMs = t4MonoMs;
    result.answer.offsetMs = offset;
    result.answer.roundTripMs = std::max(0.0, roundTrip);
    result.answer.stratum = stratum;
    return result;
}

Json ntpSetTimePayload(const NtpAnswer &answer) {
    const double seconds = std::floor(answer.atMonoMs / 1000.0);
    const double nanoseconds = std::round((answer.atMonoMs - seconds * 1000.0) * 1000000.0);
    Json timestamp = Json::object();
    timestamp.set("source", Json::text("monotonic"));
    timestamp.set("sec", Json::numeric(seconds));
    timestamp.set("nsec", Json::numeric(nanoseconds));
    Json result = Json::object();
    result.set("utc", Json::numeric(std::round(answer.atMs / 1000.0)));
    result.set("source", Json::text("sdp"));
    result.set("available", Json::flag(true));
    result.set("timestamp", timestamp);
    return result;
}

NtpResult queryNtp(const NtpSettings &settings, std::chrono::milliseconds timeout) {
    NtpResult failure;
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo *addresses = nullptr;
    const std::string port = std::to_string(settings.port);
    const int lookup = getaddrinfo(settings.server.c_str(), port.c_str(), &hints, &addresses);
    if (lookup != 0) {
        failure.error = "could not resolve " + settings.server + ": " + gai_strerror(lookup);
        return failure;
    }

    int descriptor = -1;
    addrinfo *selected = nullptr;
    for (addrinfo *address = addresses; address; address = address->ai_next) {
        descriptor = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (descriptor >= 0) {
            selected = address;
            break;
        }
    }
    if (descriptor < 0 || !selected) {
        freeaddrinfo(addresses);
        failure.error = "could not create NTP socket: " + std::string(std::strerror(errno));
        return failure;
    }
    const int oldFlags = fcntl(descriptor, F_GETFD);
    if (oldFlags >= 0)
        (void)fcntl(descriptor, F_SETFD, oldFlags | FD_CLOEXEC);

    const double t1Ms = wallMilliseconds();
    const double t1MonoMs = monotonicMilliseconds();
    const std::array<uint8_t, 48> request = buildNtpRequest(t1Ms);
    const ssize_t sent = sendto(descriptor, request.data(), request.size(), 0,
                                selected->ai_addr, selected->ai_addrlen);
    if (sent != static_cast<ssize_t>(request.size())) {
        failure.error = "could not send to " + settings.server + ": " +
                        std::string(std::strerror(errno));
        close(descriptor);
        freeaddrinfo(addresses);
        return failure;
    }
    freeaddrinfo(addresses);

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
            break;
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        pollfd wait{descriptor, POLLIN, 0};
        const int ready = poll(&wait, 1, static_cast<int>(std::max<int64_t>(1, remaining.count())));
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0) {
            failure.error = "socket error: " + std::string(std::strerror(errno));
            close(descriptor);
            return failure;
        }
        if (ready == 0)
            break;
        std::array<uint8_t, 512> reply{};
        sockaddr_storage sender{};
        socklen_t senderLength = sizeof(sender);
        const ssize_t count = recvfrom(descriptor, reply.data(), reply.size(), 0,
                                       reinterpret_cast<sockaddr *>(&sender), &senderLength);
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0) {
            failure.error = "socket error: " + std::string(std::strerror(errno));
            close(descriptor);
            return failure;
        }
        uint16_t senderPort = 0;
        if (sender.ss_family == AF_INET)
            senderPort = ntohs(reinterpret_cast<sockaddr_in *>(&sender)->sin_port);
        else if (sender.ss_family == AF_INET6)
            senderPort = ntohs(reinterpret_cast<sockaddr_in6 *>(&sender)->sin6_port);
        if (senderPort != settings.port ||
            !ntpRequestAnswered(reply.data(), static_cast<size_t>(count), request))
            continue;
        const double t4MonoMs = monotonicMilliseconds();
        const double t4Ms = t1Ms + (t4MonoMs - t1MonoMs);
        NtpResult result = parseNtpReply(reply.data(), static_cast<size_t>(count), t1Ms,
                                         t4Ms, t4MonoMs);
        close(descriptor);
        return result;
    }
    close(descriptor);
    failure.error = "no answer from " + settings.server + " in " +
                    std::to_string(timeout.count()) + " ms";
    return failure;
}

} // namespace service
} // namespace nativetv
