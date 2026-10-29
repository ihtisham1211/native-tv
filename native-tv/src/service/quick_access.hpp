#pragma once

#include "core/json.hpp"
#include <string>

namespace nativetv {
namespace service {

Json quickAccessSettingKeys();
bool quickAccessSettingsSupported(const Json &settings);
bool quickAccessTargetAllowed(const std::string &id, const Json &devices,
                              const Json &apps);
Json quickAccessWriteRequest(unsigned key, const std::string &id);

} // namespace service
} // namespace nativetv
