#pragma once

#include "core/json.hpp"
#include <string>

namespace nativetv {
namespace daemon {
class ChildProcessRunner;

// Runs the packaged install-app.sh for the TV apps: the dashboard tile and the
// splash, which are the native client, and the old Home app, still a web page.
// The script keeps the developer install service details; this wraps it the
// way the server did: one action, the last line of output read back as JSON.
class TvAppManager {
  public:
    TvAppManager(ChildProcessRunner &children, std::string assetsRoot);

    // action: status, install, remove, refresh or retire. appDir names another
    // app the script installs ("home-app", "splash-app"); empty is the dashboard.
    Json run(const std::string &action, const std::string &appDir = std::string());

    // Brings the installed native client apps up to date with the release: the
    // dashboard tile, whose answer is returned (refreshed is also set when only
    // the splash was), then the splash. An app that is not installed stays out.
    Json refreshInstalled();

  private:
    bool assetPath(const std::string &relative, std::string &path) const;

    ChildProcessRunner &children_;
    std::string assetsRoot_;
};
} // namespace daemon
} // namespace nativetv
