#pragma once

#include "core/json.hpp"
#include "daemon/config.hpp"
#include <string>

namespace nativetv {
namespace daemon {
std::string localIpv4Address();
// The MAC of the interface that has `address`, from sysfs; empty where there is none.
std::string localMacAddress(const std::string &address);
Json nativeCapabilities(const ServerConfig &config, const std::string &version,
                       const std::string &dataRoot, const std::string &remoteAddress,
                       const std::string &localAddress);
} // namespace daemon
} // namespace nativetv
