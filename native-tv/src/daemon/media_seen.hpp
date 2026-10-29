#pragma once

#include <string>

namespace nativetv {
namespace daemon {

// Persist the legacy media_seen latch without changing its plain-text format.
// Repeated calls leave an already-correct marker untouched.
bool markMediaSeen(const std::string &path, std::string *error = nullptr);

} // namespace daemon
} // namespace nativetv
