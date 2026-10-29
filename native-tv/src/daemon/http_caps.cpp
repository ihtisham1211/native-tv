#include "daemon/http_caps.hpp"
#include "daemon/http_auth.hpp"
#include <arpa/inet.h>
#include <fstream>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/socket.h>
#include <unistd.h>

namespace nativetv {
namespace daemon {
namespace {
bool endsWith(const std::string &value, const std::string &suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool networkOpen(const std::string &host) {
    return !(host.compare(0, 4, "127.") == 0 || endsWith(host, "::1") ||
             endsWith(host, "localhost"));
}

bool exists(const std::string &path) {
    return access(path.c_str(), F_OK) == 0;
}

std::string join(const std::string &directory, const std::string &name) {
    return directory.empty() || directory[directory.size() - 1] == '/'
               ? directory + name
               : directory + "/" + name;
}
} // namespace

std::string localIpv4Address() {
    struct ifaddrs *interfaces = nullptr;
    if (getifaddrs(&interfaces) != 0 || !interfaces)
        return std::string();

    std::string best;
    bool bestIsWired = false;
    for (const struct ifaddrs *item = interfaces; item; item = item->ifa_next) {
        if (!item->ifa_addr || item->ifa_addr->sa_family != AF_INET ||
            (item->ifa_flags & IFF_LOOPBACK) != 0)
            continue;
        const struct sockaddr_in *address =
            reinterpret_cast<const struct sockaddr_in *>(item->ifa_addr);
        char text[INET_ADDRSTRLEN] = {};
        if (!inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text)))
            continue;
        const std::string interfaceName = item->ifa_name ? item->ifa_name : "";
        const bool wired = interfaceName.compare(0, 3, "eth") == 0;
        if (best.empty() || (wired && !bestIsWired)) {
            best = text;
            bestIsWired = wired;
        }
    }
    freeifaddrs(interfaces);
    return best;
}

std::string localMacAddress(const std::string &address) {
    struct ifaddrs *interfaces = nullptr;
    if (address.empty() || getifaddrs(&interfaces) != 0 || !interfaces)
        return std::string();
    std::string name;
    for (const struct ifaddrs *item = interfaces; item && name.empty(); item = item->ifa_next) {
        if (!item->ifa_addr || item->ifa_addr->sa_family != AF_INET)
            continue;
        char text[INET_ADDRSTRLEN] = {};
        const struct sockaddr_in *found = reinterpret_cast<const struct sockaddr_in *>(item->ifa_addr);
        if (inet_ntop(AF_INET, &found->sin_addr, text, sizeof(text)) && address == text && item->ifa_name)
            name = item->ifa_name;
    }
    freeifaddrs(interfaces);
    if (name.empty())
        return std::string();
    std::ifstream file("/sys/class/net/" + name + "/address");
    std::string mac;
    std::getline(file, mac);
    return mac;
}

Json nativeCapabilities(const ServerConfig &config, const std::string &version,
                       const std::string &dataRoot, const std::string &remoteAddress,
                       const std::string &localAddress) {
    Json result = Json::object();
    result.set("ok", Json::flag(true));
    result.set("allowControl", Json::flag(config.allowControl));
    result.set("allowPower", Json::flag(config.allowPower));
    if (networkOpen(config.host) && !localAddress.empty())
        result.set("origin", Json::text("http://" + localAddress + ":" +
                                         std::to_string(config.port)));
    else
        result.set("origin", Json());
    result.set("version", Json::text(version));
    result.set("fromHomebrewChannel",
               Json::flag(exists(join(dataRoot, ".from-homebrew-channel"))));
    const Json emulateSetup = config.document["emulateSetup"];
    result.set("setupNeeded", Json::flag(emulateSetup.boolean() ||
                                          exists(join(dataRoot, ".setup-pending"))));
    if (!config.token.empty() && isLoopbackPeer(remoteAddress))
        result.set("key", Json::text(config.token));
    return result;
}
} // namespace daemon
} // namespace nativetv
