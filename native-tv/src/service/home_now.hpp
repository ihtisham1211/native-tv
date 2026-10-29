#pragma once

#include "core/json.hpp"
#include <cstdint>
#include <string>

namespace nativetv {
namespace service {

bool homeNowInputPort(const std::string &appId, unsigned &port);
bool homeNowIsLiveTv(const std::string &appId);
Json homeNowEmpty();
Json homeNowInput(const std::string &appId, int64_t since, const Json &devices);
Json homeNowChannel(const std::string &appId, int64_t since,
                    const Json &channels, const Json &lastChannel,
                    const Json &programme);
Json homeNowApp(const std::string &appId, int64_t since, const Json &apps);

} // namespace service
} // namespace nativetv
