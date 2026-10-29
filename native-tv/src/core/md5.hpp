#pragma once

#include <string>

namespace nativetv {

// Stable lowercase MD5 encoding used for cache filenames shared with Node.
std::string md5Hex(const std::string &text);

} // namespace nativetv
