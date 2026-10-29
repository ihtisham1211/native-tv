#pragma once

#include "daemon/child_process.hpp"
#include <chrono>
#include <string>
#include <vector>

namespace nativetv {
namespace daemon {

enum class AppInstallOutcome { Installed, Failed, Closed, TimedOut };

struct AppInstallAppVersion {
    std::string id;
    std::string version;
};

struct AppInstallResult {
    AppInstallOutcome outcome = AppInstallOutcome::Closed;
    std::string error;
    std::string packageVersion;
    std::vector<AppInstallAppVersion> apps;
};

class AppInstallExecutor {
  public:
    AppInstallExecutor(ChildProcessRunner &children, std::string stagingDirectory,
                       std::string lunaSendPath);

    bool run(const std::string &packageId, const std::string &fileName,
             const std::string &expectedSha256, std::chrono::milliseconds timeout,
             AppInstallResult &result,
             std::string *error = nullptr) const;

  private:
    ChildProcessRunner &children_;
    std::string stagingDirectory_;
    std::string lunaSendPath_;
};

} // namespace daemon
} // namespace nativetv
