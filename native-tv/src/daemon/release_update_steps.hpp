#pragma once

#include "daemon/child_process.hpp"
#include "service/app_package_fetcher.hpp"
#include "service/release_update.hpp"
#include <chrono>
#include <functional>
#include <string>

namespace nativetv {
namespace daemon {

struct ReleaseUpdateEnvironment {
    // Where the downloaded package waits, on the TV's writable partition.
    std::string stagingDirectory;
    // The install the update replaces (/var/lib/tvweb).
    std::string dataRoot;
    std::string lunaSendPath;
    std::string userAgent;
    // NATIVETV_SERVER_VERSION: a build from a git checkout has a "+" in it.
    std::string serverVersion;
    std::function<std::string()> ipPreference;
    ChildProcessRunner *children = nullptr;
    // dev/install can finish after luna-send has stopped waiting for it: the app's
    // files are waited for this long before the package is called not installed.
    std::chrono::seconds settleTime{30};
    // libcurl, which the tests may replace.
    service::AppPackageDownload downloader;
    std::function<void(const std::string &)> warn;
};

// The daemon's own steps for ReleaseUpdate: GitHub over libcurl, the TV's app
// installer through luna-send, and the app's payload/install.sh.
service::ReleaseUpdateSteps productionReleaseUpdateSteps(const ReleaseUpdateEnvironment &environment);

} // namespace daemon
} // namespace nativetv
