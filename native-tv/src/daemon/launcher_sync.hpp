#pragma once

#include <string>
#include <vector>

namespace nativetv {
namespace daemon {

// The TV apps that open the dashboard and Home are static pages that cannot
// read config.json, so each carries the port as a `var PORT = n; // tvweb:port`
// line and its appinfo.json asks for a 1920x1080 canvas. An update, a redeploy
// or a changed "port" leaves them out of step; these put them right at start
// (legacy lib/apps.js syncLauncherPort and syncLauncherAppinfo). root is the
// folder holding the installed apps; each returns the ids it changed.
std::vector<std::string> syncLauncherAppinfo(const std::string &root);
// Does syncLauncherAppinfo first, then rewrites the port line of each page.
std::vector<std::string> syncLauncherPort(int port, const std::string &root);

} // namespace daemon
} // namespace nativetv
