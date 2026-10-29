#include "daemon/tv_app_manager.hpp"
#include "daemon/child_process.hpp"
#include <chrono>
#include <sys/stat.h>

namespace nativetv {
namespace daemon {
namespace {
Json unsupported() {
    Json response = Json::object();
    response.set("ok", Json::flag(true));
    response.set("supported", Json::flag(false));
    response.set("installed", Json::flag(false));
    return response;
}

std::string trimmed(const std::string &value) {
    size_t start = 0, end = value.size();
    while (start < end && (value[start] == ' ' || value[start] == '\n' ||
                           value[start] == '\r' || value[start] == '\t')) ++start;
    while (end > start && (value[end - 1] == ' ' || value[end - 1] == '\n' ||
                           value[end - 1] == '\r' || value[end - 1] == '\t')) --end;
    return value.substr(start, end - start);
}
} // namespace

TvAppManager::TvAppManager(ChildProcessRunner &children, std::string assetsRoot)
    : children_(children), assetsRoot_(std::move(assetsRoot)) {}

bool TvAppManager::assetPath(const std::string &relative, std::string &path) const {
    // Nothing from the request reaches here, but keep the lookup inside the root.
    if (relative.find("..") != std::string::npos ||
        relative.find('\0') != std::string::npos) return false;
    const std::string full = assetsRoot_ + "/" + relative;
    struct stat info;
    if (stat(full.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) return false;
    path = full;
    return true;
}

Json TvAppManager::run(const std::string &action, const std::string &appDir) {
    std::string script;
    if (!assetPath("dashboard-app/install-app.sh", script)) return unsupported();
    ChildOptions options;
    options.arguments = {"/bin/sh", script, action};
    if (!appDir.empty()) {
        std::string manifest;
        if (!assetPath(appDir + "/appinfo.json", manifest)) return unsupported();
        options.arguments.push_back(manifest.substr(0, manifest.find_last_of('/')));
    }
    options.timeout = std::chrono::milliseconds(90000);
    const ChildResult result = children_.run(options);

    const std::string output = trimmed(result.standardOutput);
    const size_t lastBreak = output.find_last_of('\n');
    const std::string last = lastBreak == std::string::npos
        ? output : output.substr(lastBreak + 1);
    try {
        const Json parsed = Json::parse(last);
        if (parsed.isObject()) return parsed;
    } catch (...) {
    }
    // install prints a sentence rather than JSON, so read its wording.
    Json response = Json::object();
    if (action == "install" || action == "refresh") {
        response.set("ok", Json::flag(output.find("added to the home screen") != std::string::npos));
        response.set("refreshed", Json::flag(action == "refresh"));
        return response;
    }
    const bool failed = !result.succeeded();
    response.set("ok", Json::flag(!failed));
    response.set("error", Json::text(failed
        ? (result.error.empty() ? "Command failed" : result.error)
        : "unreadable result"));
    return response;
}

Json TvAppManager::refreshInstalled() {
    Json tile = run("refresh");
    // The splash is the same client in a folder of its own, so it goes stale with the tile.
    const Json splash = run("refresh", "splash-app");
    if (splash["ok"].boolean() && splash["refreshed"].boolean())
        tile.set("refreshed", Json::flag(true));
    return tile;
}

} // namespace daemon
} // namespace nativetv
