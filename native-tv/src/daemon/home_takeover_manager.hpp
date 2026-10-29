#pragma once

#include "core/json.hpp"
#include "daemon/home_takeover_stager.hpp"

#include <mutex>
#include <string>

namespace nativetv {
namespace daemon {
class ChildProcessRunner;

// Owns staging and the bind-mount boundary for the hidden LG Home replacement.
// HomeTakeoverLifecycleManager sequences SAM rereads and rollback around it.
class HomeTakeoverManager {
  public:
    HomeTakeoverManager(ChildProcessRunner &children, std::string sourceRoot,
                        std::string stagingRoot, std::string systemRoot, int port);

    Json takeover();
    Json release();

  private:
    bool mounted() const;
    bool unmount(std::string &error) const;

    ChildProcessRunner &children_;
    HomeTakeoverStager stager_;
    std::string stagingRoot_;
    std::string systemRoot_;
    std::string appDirectory_;
    std::mutex mutex_;
};
} // namespace daemon
} // namespace nativetv
