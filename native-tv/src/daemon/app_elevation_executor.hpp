#pragma once

#include "daemon/child_process.hpp"
#include <chrono>
#include <string>
#include <vector>

namespace nativetv {
namespace daemon {

struct AppElevationFailure {
    std::string service;
    std::string error;
};

struct AppElevationResult {
    std::vector<std::string> done;
    std::vector<AppElevationFailure> failed;
};

class AppElevationExecutor {
  public:
    AppElevationExecutor(ChildProcessRunner &children, std::string systemRoot,
                         std::chrono::milliseconds timeout = std::chrono::milliseconds(60000));

    bool run(const std::string &packageId, const std::vector<std::string> &services,
             AppElevationResult &result, std::string *error = nullptr) const;

  private:
    ChildProcessRunner &children_;
    std::string systemRoot_;
    std::chrono::milliseconds timeout_;
};

} // namespace daemon
} // namespace nativetv
