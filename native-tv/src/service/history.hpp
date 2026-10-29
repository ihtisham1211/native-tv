#pragma once

#include "core/json.hpp"
#include <cstdint>
#include <string>

namespace nativetv {
namespace service {

// Reads the durable monthly history files and overlays the currently dirty
// hours from the live Node recorder. The overlay is bounded and contains only
// the requested time range, so API reads do not force a flash write.
bool queryHistory(const std::string &directory, int64_t fromMs, int64_t toMs,
                  const Json &overlay, Json &result, std::string &error);

} // namespace service
} // namespace nativetv
