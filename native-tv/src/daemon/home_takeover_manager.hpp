#pragma once

#include "core/json.hpp"
#include "daemon/home_takeover_stager.hpp"

#include <mutex>
#include <string>

namespace nativetv {
namespace daemon {
class ChildProcessRunner;

// Owns staging and the bind mount that puts Native Tv Home (the native client,
// home_takeover_stager) in LG's Home's place. HomeTakeoverLifecycleManager
// sequences sam's rereads, stopping the old instance and rollback around it.
class HomeTakeoverManager {
  public:
    HomeTakeoverManager(ChildProcessRunner &children, std::string sourceRoot,
                        std::string stagingRoot, std::string systemRoot);

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
