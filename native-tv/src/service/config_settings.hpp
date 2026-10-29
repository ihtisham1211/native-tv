#pragma once

#include "core/json.hpp"
#include <string>
#include <vector>

namespace nativetv {
namespace service {

// Validates the user-writable subset of the general settings form. Security
// sensitive configuration keys are intentionally omitted from the result.
bool validateDashboardSettings(const Json &input, Json &patch,
                               std::vector<std::string> &errors);

} // namespace service
} // namespace nativetv
