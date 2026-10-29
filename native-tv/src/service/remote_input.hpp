#pragma once

#include <string>

namespace nativetv {
namespace service {

// Injects one supported LG media key into the paired remote's Linux input
// device. procRoot and systemRoot are injectable filesystem roots for tests.
bool injectRemoteMediaKey(const std::string &procRoot,
                          const std::string &systemRoot, unsigned code);
// Injects Linux KEY_BACK (158), which dismisses a showing screen saver.
bool injectRemoteBackKey(const std::string &procRoot,
                         const std::string &systemRoot);
// Injects Linux KEY_LEFTMETA (125), used by the legacy webOS 3–5 Home fallback.
bool injectRemoteHomeKey(const std::string &procRoot,
                         const std::string &systemRoot);

} // namespace service
} // namespace nativetv
