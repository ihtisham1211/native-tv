#pragma once

#include <string>

namespace nativetv {
namespace daemon {
// TVWEB_DATA_DIR is the daemon's relocatable state root. The TV install keeps
// the legacy location unless a test or runtime explicitly supplies it.
std::string dataDirectory();
} // namespace daemon
} // namespace nativetv
