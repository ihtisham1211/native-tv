#pragma once

#include "core/json.hpp"
#include <string>

namespace nativetv {
namespace service {

Json homeStatusSupported(const std::string &webosVersion,
                         bool allowExperimental);
Json homeStatusProjection(const Json &rawDocument,
                          const std::string &webosVersion,
                          bool allowExperimental, bool active,
                          bool lgHomeOpen, bool historyEnabled,
                          const Json &wallpapers,
                          const std::string &mainProfileName = "Main");

} // namespace service
} // namespace nativetv
