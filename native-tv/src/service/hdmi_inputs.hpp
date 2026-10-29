#pragma once

#include "core/json.hpp"

namespace nativetv {
namespace service {
Json hdmiBaseInputMap();
Json hdmiInputMapFromConfig(const Json &response);
Json hdmiInputs(const Json &systemSnapshot, const Json &devices,
                const Json &foreground, const Json &receiverMap);
} // namespace service
} // namespace nativetv
