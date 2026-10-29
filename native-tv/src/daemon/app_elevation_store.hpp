#pragma once

#include "core/json.hpp"
#include <string>

namespace nativetv {
namespace daemon {

bool readElevatedServices(const std::string &path, const std::string &appId,
                         Json &services, std::string *error = nullptr);
bool recordElevatedServices(const std::string &path, const std::string &appId,
                           const Json &services, std::string *error = nullptr);

} // namespace daemon
} // namespace nativetv
