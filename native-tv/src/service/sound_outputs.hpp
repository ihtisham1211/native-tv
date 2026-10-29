#pragma once

#include "core/json.hpp"
#include <string>
#include <vector>

namespace nativetv {
namespace service {

const std::vector<std::string> &soundOutputConfigNames();
Json offeredSoundOutputs(const Json &values, const Json &configs,
                         const std::string &current);

} // namespace service
} // namespace nativetv
