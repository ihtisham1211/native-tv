#include "daemon/screensaver_manager.hpp"
#include "daemon/child_process.hpp"
#include "daemon/logger.hpp"
#include "daemon/luna.hpp"
#include "daemon/sam_rescan_manager.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace nativetv {
namespace daemon {
namespace {
const char *const kModes[] = {
    "clock", "starfield", "fireworks", "bokeh", "harmonograph", "vitals"
};

std::string rooted(const std::string &root, const std::string &path) {
    if (root.empty() || root == "/") return path;
    size_t end = root.size();
    while (end > 1 && root[end - 1] == '/') --end;
    return root.substr(0, end) + path;
}

bool isMode(const std::string &mode) {
    for (const char *candidate : kModes)
        if (mode == candidate) return true;
    return false;
}

bool ensureDirectory(const std::string &path) {
    if (path.empty() || path == "/") return true;
    struct stat info;
    if (stat(path.c_str(), &info) == 0) return S_ISDIR(info.st_mode);
    const size_t slash = path.find_last_of('/');
    if (slash != std::string::npos && slash > 0 &&
        !ensureDirectory(path.substr(0, slash))) return false;
    if (mkdir(path.c_str(), 0755) == 0) return true;
    return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

bool readFile(const std::string &path, std::string &contents) {
    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input) return false;
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (!input.good() && !input.eof()) return false;
    contents = buffer.str();
    return true;
}

bool writeFile(const std::string &path, const std::string &contents) {
    std::ofstream output(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    output.close();
    return output.good();
}

void removeTreeContents(const std::string &path) {
    DIR *directory = opendir(path.c_str());
    if (!directory) return;
    struct dirent *entry;
    while ((entry = readdir(directory)) != nullptr) {
        const std::string name(entry->d_name);
        if (name == "." || name == "..") continue;
        const std::string child = path + "/" + name;
        struct stat info;
        if (lstat(child.c_str(), &info) != 0) continue;
        if (S_ISDIR(info.st_mode)) {
            removeTreeContents(child);
            (void)rmdir(child.c_str());
        } else {
            (void)unlink(child.c_str());
        }
    }
    closedir(directory);
}

std::string replaceAll(std::string value, const std::string &from,
                       const std::string &to) {
    size_t at = 0;
    while ((at = value.find(from, at)) != std::string::npos) {
        value.replace(at, from.size(), to);
        at += to.size();
    }
    return value;
}

std::string statsUrl(unsigned short port, const std::string &token) {
    std::string url = "http://127.0.0.1:" + std::to_string(port) + "/api/stats";
    if (!token.empty()) url += "?k=" + encode(token);
    return url;
}

bool copyAssets(const std::string &source, const std::string &target) {
    DIR *directory = opendir(source.c_str());
    if (!directory) return false;
    bool ok = true;
    struct dirent *entry;
    while ((entry = readdir(directory)) != nullptr) {
        const std::string name(entry->d_name);
        if (name == "." || name == ".." ||
            (name.size() >= 4 && name.compare(name.size() - 4, 4, ".qml") == 0)) continue;
        const std::string from = source + "/" + name;
        const std::string to = target + "/" + name;
        struct stat info;
        if (stat(from.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) continue;
        std::string bytes;
        if (!readFile(from, bytes) || !writeFile(to, bytes)) ok = false;
    }
    closedir(directory);
    return ok;
}

int versionMajor(const std::string &contents, const std::string &marker) {
    const size_t markerAt = contents.find(marker);
    if (markerAt == std::string::npos) return 0;
    size_t at = markerAt + marker.size();
    while (at < contents.size() &&
           !std::isdigit(static_cast<unsigned char>(contents[at]))) ++at;
    if (at == contents.size()) return 0;
    int major = 0;
    while (at < contents.size() &&
           std::isdigit(static_cast<unsigned char>(contents[at]))) {
        major = major * 10 + (contents[at] - '0');
        ++at;
    }
    return major;
}

int webosMajor(const std::string &systemRoot) {
    std::string release;
    if (readFile(rooted(systemRoot, "/etc/issue"), release)) {
        const int major = versionMajor(release, "webOS");
        if (major > 0) return major;
    }
    if (readFile(rooted(systemRoot, "/etc/starfish-release"), release))
        return versionMajor(release, "release");
    return 0;
}

bool manifestType(const std::string &path, std::string &type) {
    std::string contents;
    if (!readFile(path, contents)) return false;
    try {
        const Json manifest = Json::parse(contents);
        if (!manifest.isObject() || !manifest["type"].isString()) return false;
        type = manifest["type"].string();
        return !type.empty();
    } catch (...) {
        return false;
    }
}

Json failure(const std::string &error) {
    Json response = Json::object();
    response.set("ok", Json::flag(false));
    response.set("error", Json::text(error));
    return response;
}
} // namespace

ScreensaverManager::ScreensaverManager(ChildProcessRunner &children,
                                       LunaClient &luna,
                                       std::string systemRoot,
                                       std::string assetsRoot,
                                       Logger *logger)
    : children_(children), luna_(luna), systemRoot_(std::move(systemRoot)),
      assetsRoot_(std::move(assetsRoot)), logger_(logger),
      remoteClosePending_(std::make_shared<std::atomic<bool>>(false)) {}

namespace {
ChildResult runTool(ChildProcessRunner &children, const std::string &path,
                    const std::vector<std::string> &arguments) {
    ChildOptions options;
    options.arguments.push_back(path);
    options.arguments.insert(options.arguments.end(), arguments.begin(), arguments.end());
    options.timeout = std::chrono::milliseconds(4000);
    options.maximumOutputBytes = 4096;
    return children.run(options);
}

bool unmountTree(ChildProcessRunner &children, const std::string &systemRoot,
                 const std::string &app) {
    const std::string command = rooted(systemRoot, "/bin/umount");
    (void)runTool(children, command, {"-l", app + "/qml/main.qml"});
    const ChildResult normal = runTool(children, command, {app});
    if (normal.succeeded()) return true;
    (void)runTool(children, command, {"-l", app});
    return false;
}

void clearStaged(const std::string &stage) {
    (void)unlink((stage + "/.tvweb-screensaver").c_str());
    (void)unlink((stage + "/.tvweb-brightness").c_str());
    (void)unlink((stage + "/appinfo.json").c_str());
    removeTreeContents(stage + "/qml");
    (void)rmdir((stage + "/qml").c_str());
}

bool stageFiles(const std::string &systemRoot, const std::string &assetsRoot,
                const std::string &mode, const std::string &level,
                unsigned short publicPort, const std::string &token,
                bool stageManifest, std::string &error) {
    const std::string app = rooted(systemRoot, "/usr/palm/applications/com.webos.app.screensaver");
    const std::string stage = rooted(systemRoot, "/var/lib/tvweb/screensaver");
    const std::string stockAppInfo = app + "/appinfo.json";
    const std::string source = assetsRoot + "/screensavers/" + mode + ".qml";
    const std::string qmlDir = stage + "/qml";
    if (!ensureDirectory(qmlDir)) {
        error = "could not create the screen saver staging directory";
        return false;
    }

    if (stageManifest) {
        std::string appInfo;
        if (!readFile(stockAppInfo, appInfo)) {
            error = "could not read the stock screen saver manifest";
            return false;
        }
        try {
            Json manifest = Json::parse(appInfo);
            if (!manifest.isObject()) throw std::runtime_error("manifest is not an object");
            const std::string stockType = manifest["type"].string();
            if (!stockType.empty()) {
                (void)ensureDirectory(rooted(systemRoot, "/var/lib/tvweb"));
                (void)writeFile(rooted(systemRoot, "/var/lib/tvweb/screensaver-stock-type"),
                                stockType + "\n");
            }
            manifest.set("type", Json::text("qml"));
            manifest.set("main", Json::text("qml/main.qml"));
            appInfo = manifest.dump();
        } catch (...) {
            // Preserve the legacy behavior: a malformed stock manifest is staged
            // unchanged, and the manager reports whether SAM accepted the runner.
        }
        if (!writeFile(stage + "/appinfo.json", appInfo)) {
            error = "could not stage the screen saver manifest";
            return false;
        }
    }

    std::string qml;
    if (!readFile(source, qml)) {
        error = "screen saver asset is missing: " + mode;
        return false;
    }
    qml = replaceAll(qml, "__TVWEB_URL__", statsUrl(publicPort, token));
    qml = replaceAll(qml, "__TVWEB_LEVEL__", level == "bright" ? "1" : "0");
    if (!writeFile(qmlDir + "/main.qml", qml) ||
        !copyAssets(assetsRoot + "/screensavers", qmlDir)) {
        error = "could not stage screen saver assets";
        return false;
    }
    if (!writeFile(stage + "/.tvweb-brightness", level + "\n") ||
        !writeFile(stage + "/.tvweb-screensaver", mode + "\n")) {
        error = "could not write screen saver state";
        return false;
    }
    return true;
}
} // namespace

namespace {
std::string trimmed(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.pop_back();
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])))
        ++start;
    return value.substr(start);
}

std::string readTrimmed(const std::string &path) {
    std::string contents;
    return readFile(path, contents) ? trimmed(contents) : std::string();
}

std::vector<std::string> splitLines(const std::string &text) {
    std::vector<std::string> lines;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) lines.push_back(line);
    return lines;
}

std::vector<std::string> splitSpaces(const std::string &line) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        const size_t space = line.find(' ', start);
        if (space == std::string::npos) {
            parts.push_back(line.substr(start));
            return parts;
        }
        parts.push_back(line.substr(start, space - start));
        start = space + 1;
    }
}

bool startsWith(const std::string &value, const std::string &prefix) {
    return value.compare(0, prefix.size(), prefix) == 0;
}

// A mount over the screen saver app that is not ours: another tool's saver.
bool externalMountActive(const std::string &systemRoot, const std::string &app,
                         const std::string &stage) {
    std::string info;
    if (readFile(rooted(systemRoot, "/proc/self/mountinfo"), info)) {
        for (const std::string &line : splitLines(info)) {
            const std::vector<std::string> parts = splitSpaces(line);
            if (parts.size() >= 5 && startsWith(parts[4], app) &&
                parts[3].find(stage) == std::string::npos)
                return true;
        }
    }
    std::string mounts;
    if (readFile(rooted(systemRoot, "/proc/mounts"), mounts)) {
        for (const std::string &line : splitLines(mounts)) {
            const std::vector<std::string> parts = splitSpaces(line);
            if (parts.size() >= 2 && startsWith(parts[1], app) &&
                parts[0].find(stage) == std::string::npos && parts[1] != app)
                return true;
        }
    }
    return false;
}

bool externalInitHook(const std::string &systemRoot) {
    DIR *directory = opendir(rooted(systemRoot, "/var/lib/webosbrew/init.d").c_str());
    if (!directory) return false;
    bool found = false;
    struct dirent *entry;
    while (!found && (entry = readdir(directory)) != nullptr) {
        std::string name(entry->d_name);
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
        found = name.find("screensaver") != std::string::npos;
    }
    closedir(directory);
    return found;
}
} // namespace

service::ScreensaverState ScreensaverManager::state(bool allowControl,
                                                    bool allowedAnyway) const {
    const std::string appDir = "/usr/palm/applications/com.webos.app.screensaver";
    const std::string app = rooted(systemRoot_, appDir);
    service::ScreensaverState state;
    state.stockType = readTrimmed(rooted(systemRoot_, "/var/lib/tvweb/screensaver-stock-type"));
    state.webosMajor = webosMajor(systemRoot_);
    state.allowedAnyway = allowedAnyway;
    state.allowControl = allowControl;
    state.markerMode = readTrimmed(app + "/.tvweb-screensaver");
    state.markerLevel = readTrimmed(app + "/.tvweb-brightness");
    for (const char *mode : kModes) {
        struct stat info;
        if (stat((assetsRoot_ + "/screensavers/" + mode + ".qml").c_str(), &info) == 0 &&
            S_ISREG(info.st_mode))
            state.assets.insert(mode);
    }
    state.externalActive = externalMountActive(systemRoot_, appDir,
                                               "/var/lib/tvweb/screensaver");
    state.externalHook = externalInitHook(systemRoot_);
    return state;
}

void ScreensaverManager::reconcileStartup() {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string app = rooted(systemRoot_, "/usr/palm/applications/com.webos.app.screensaver");
    const std::string stage = rooted(systemRoot_, "/var/lib/tvweb/screensaver");
    std::string mode;
    if (readFile(app + "/.tvweb-screensaver", mode)) {
        while (!mode.empty() && (mode.back() == '\n' || mode.back() == '\r')) mode.pop_back();
        if (isMode(mode)) return;
    }
    clearStaged(stage);
    std::string appInfo;
    if (!readFile(app + "/appinfo.json", appInfo)) return;
    try {
        const Json manifest = Json::parse(appInfo);
        const std::string stockType = manifest["type"].string();
        if (stockType.empty()) return;
        const std::string stateRoot = rooted(systemRoot_, "/var/lib/tvweb");
        if (ensureDirectory(stateRoot))
            (void)writeFile(stateRoot + "/screensaver-stock-type", stockType + "\n");
    } catch (...) {
    }
}

Json ScreensaverManager::reconcileBoot(SamRescanManager &samRescan,
                                       const CommandPolicyContext &context,
                                       bool allowExperimental) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string app = rooted(systemRoot_, "/usr/palm/applications/com.webos.app.screensaver");
    const std::string stage = rooted(systemRoot_, "/var/lib/tvweb/screensaver");
    const std::string stockPath = app + "/appinfo.json";
    std::string stockType;
    if (!manifestType(stockPath, stockType))
        return failure("could not read LG's stock screen saver manifest");

    const std::string dataRoot = rooted(systemRoot_, "/var/lib/tvweb");
    if (!ensureDirectory(dataRoot) ||
        !writeFile(dataRoot + "/screensaver-stock-type", stockType + "\n"))
        return failure("could not record LG's stock screen saver type");

    const std::string appId = "com.webos.app.screensaver";
    CommandPolicyContext maintenanceContext = context;
    maintenanceContext.allowControl = true;
    if (stockType != "qml")
        (void)samRescan.releaseBlockedApp(appId, maintenanceContext);

    const int major = webosMajor(systemRoot_);
    const bool held = stockType != "qml" && major != 10 && major != 11 &&
                      !allowExperimental;
    std::string mode;
    if (!readFile(stage + "/.tvweb-screensaver", mode)) {
        Json response = Json::object();
        response.set("ok", Json::flag(true));
        response.set("mounted", Json::flag(false));
        response.set("stockType", Json::text(stockType));
        return response;
    }
    while (!mode.empty() && (mode.back() == '\n' || mode.back() == '\r'))
        mode.pop_back();
    if (!isMode(mode)) return failure("custom screen saver marker is invalid");

    std::string stagedType;
    (void)manifestType(stage + "/appinfo.json", stagedType);
    if (held) {
        if (logger_)
            logger_->info("screensaver: custom saver held back on webOS " +
                          (major > 0 ? std::to_string(major) : "unknown") +
                          " (stock " + stockType + ", custom " +
                          (stagedType.empty() ? "unknown" : stagedType) + ")");
        Json response = Json::object();
        response.set("ok", Json::flag(true));
        response.set("mounted", Json::flag(false));
        response.set("heldBack", Json::flag(true));
        response.set("stockType", Json::text(stockType));
        return response;
    }

    const ChildResult mounted = runTool(
        children_, rooted(systemRoot_, "/bin/mount"), {"--bind", stage, app});
    if (!mounted.succeeded())
        return failure("could not mount the custom screen saver");

    bool accepted = stockType.empty() || stagedType.empty() || stockType == stagedType;
    if (!accepted) {
        const Json reread = samRescan.rereadApp(appId, maintenanceContext);
        accepted = reread["result"].boolean() &&
                   samRescan.reportsAppType(appId, stagedType, maintenanceContext);
        if (!accepted) {
            const Json nudged = samRescan.nudge(maintenanceContext);
            accepted = nudged["result"].boolean() &&
                       samRescan.reportsAppType(appId, stagedType,
                                                maintenanceContext);
        }
        if (!accepted) {
            (void)unmountTree(children_, systemRoot_, app);
            if (logger_)
                logger_->warning("screensaver: SAM kept LG's saver; custom mount removed");
        } else if (logger_) {
            logger_->info("screensaver: SAM accepted custom type " + stagedType);
        }
    }

    Json response = Json::object();
    response.set("ok", Json::flag(true));
    response.set("mounted", Json::flag(accepted));
    response.set("stockType", Json::text(stockType));
    if (!stagedType.empty()) response.set("stagedType", Json::text(stagedType));
    return response;
}

void ScreensaverManager::remoteKeyPressed() {
    const std::string marker = rooted(systemRoot_, "/var/run/tvweb-screensaver-launched");
    std::string markerContents;
    if (!readFile(marker, markerContents)) return;

    const std::shared_ptr<std::atomic<bool>> pending = remoteClosePending_;
    bool expected = false;
    if (!pending->compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return;

    LunaClient *const luna = &luna_;
    Logger *const logger = logger_;
    const auto finish = [pending]() {
        pending->store(false, std::memory_order_release);
    };
    const auto powerReply = [luna, logger, finish](const LunaReply &power) {
        if (!power.error.empty() || !power.response["returnValue"].boolean()) {
            finish();
            return;
        }

        std::string state = power.response["state"].string();
        state.erase(std::remove_if(state.begin(), state.end(), [](unsigned char value) {
            return !std::isalnum(value);
        }), state.end());
        std::transform(state.begin(), state.end(), state.begin(), [](unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
        if (state == "screensaver") {
            finish();
            return;
        }

        const bool queued = luna->call(
            "com.webos.applicationManager/running", Json::object(),
            [luna, logger, finish](const LunaReply &running) {
                if (!running.error.empty() ||
                    !running.response["returnValue"].boolean()) {
                    finish();
                    return;
                }
                bool saverRunning = false;
                for (const Json &app : running.response["running"].items()) {
                    if (app["id"].string() == "com.webos.app.screensaver") {
                        saverRunning = true;
                        break;
                    }
                }
                if (!saverRunning) {
                    finish();
                    return;
                }

                Json payload = Json::object();
                payload.set("id", Json::text("com.webos.app.screensaver"));
                const bool closeQueued = luna->call(
                    "com.webos.applicationManager/closeByAppId", payload,
                    [logger, finish](const LunaReply &closed) {
                        if ((!closed.error.empty() ||
                             !closed.response["returnValue"].boolean()) && logger)
                            logger->debug("screensaver: remote dismissal was not accepted");
                        finish();
                    });
                if (!closeQueued) finish();
            });
        if (!queued) finish();
    };

    if (!luna_.call("com.webos.service.tvpower/power/getPowerState", Json::object(),
                    powerReply))
        finish();
}

Json ScreensaverManager::prepare(const std::string &mode, const std::string &level,
                                 unsigned short publicPort,
                                 const std::string &token) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (mode != "stock" && !isMode(mode)) return failure("unknown screen saver mode");
    const std::string app = rooted(systemRoot_, "/usr/palm/applications/com.webos.app.screensaver");
    const std::string stage = rooted(systemRoot_, "/var/lib/tvweb/screensaver");
    (void)unmountTree(children_, systemRoot_, app);
    if (mode == "stock") {
        clearStaged(stage);
        Json response = Json::object();
        response.set("ok", Json::flag(true));
        response.set("mounted", Json::flag(false));
        response.set("current", Json::text("stock"));
        response.set("level", Json::text("dim"));
        return response;
    }

    std::string error;
    const std::string normalizedLevel = level == "bright" ? "bright" : "dim";
    if (!stageFiles(systemRoot_, assetsRoot_, mode, normalizedLevel,
                    publicPort, token, true, error)) return failure(error);
    const std::string mount = rooted(systemRoot_, "/bin/mount");
    const ChildResult result = runTool(children_, mount, {"--bind", stage, app});
    Json response = Json::object();
    response.set("ok", Json::flag(true));
    response.set("mounted", Json::flag(result.succeeded()));
    response.set("current", Json::text(mode));
    response.set("level", Json::text(normalizedLevel));
    if (!result.succeeded()) response.set("error", Json::text("the mount did not take"));
    return response;
}

Json ScreensaverManager::restoreStock() {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string app = rooted(systemRoot_, "/usr/palm/applications/com.webos.app.screensaver");
    const std::string stage = rooted(systemRoot_, "/var/lib/tvweb/screensaver");
    (void)unmountTree(children_, systemRoot_, app);
    clearStaged(stage);
    Json response = Json::object();
    response.set("ok", Json::flag(true));
    response.set("current", Json::text("stock"));
    return response;
}

Json ScreensaverManager::unmount() {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string app = rooted(systemRoot_, "/usr/palm/applications/com.webos.app.screensaver");
    (void)unmountTree(children_, systemRoot_, app);
    Json response = Json::object();
    response.set("ok", Json::flag(true));
    return response;
}

Json ScreensaverManager::restage(const std::string &mode, const std::string &level,
                                 unsigned short publicPort,
                                 const std::string &token) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!isMode(mode)) return failure("unknown screen saver mode");
    std::string error;
    std::string stagedMode;
    if (!readFile(rooted(systemRoot_, "/var/lib/tvweb/screensaver/.tvweb-screensaver"),
                  stagedMode) || stagedMode != mode + "\n") {
        return failure("no custom screen saver is staged");
    }
    error.clear();
    if (!stageFiles(systemRoot_, assetsRoot_, mode,
                    level == "bright" ? "bright" : "dim",
                    publicPort, token, false, error)) return failure(error);
    Json response = Json::object();
    response.set("ok", Json::flag(true));
    response.set("current", Json::text(mode));
    return response;
}
} // namespace daemon
} // namespace nativetv
