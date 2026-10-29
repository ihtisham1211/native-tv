#pragma once

#include <sys/types.h>

#include <string>
#include <vector>

namespace nativetv {
namespace service {

struct ReapedChild {
    pid_t pid = 0;
    std::string location;
};

// Kills only direct children that still run the parent's executable after the
// settle interval. This is the fork-before-exec recovery used by tvwebctl.
bool reapStuckChildren(const std::string &procRoot, pid_t parentPid,
                      unsigned settleMs, std::vector<ReapedChild> &reaped,
                      std::string *error = nullptr);

} // namespace service
} // namespace nativetv
