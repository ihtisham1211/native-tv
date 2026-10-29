#pragma once

#include <cstddef>
#include <string>

namespace nativetv {
namespace daemon {

// Files earlier releases installed and this one does not. deploy.sh and the
// Homebrew Channel install put a release's files over the old ones and remove
// none, so they stay until start retires them (the Node server's retired.js did the same).
// serverDirectory is the folder holding assets/ and lib/; returns how many
// files went. Folders the removals empty go too, and nothing else is touched.
size_t removeRetiredFiles(const std::string &serverDirectory);

} // namespace daemon
} // namespace nativetv
