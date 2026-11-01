#include "daemon/app_tile_manager.hpp"
#include "domain/say.hpp"
#include "daemon/child_process.hpp"
#include "daemon/sam_rescan_manager.hpp"
#include "service/app_ids.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace nativetv {
namespace daemon {
namespace {

const char *const kAppBases[] = {
    "/media/system/apps/usr/palm/applications",
    "/usr/palm/applications",
    "/mnt/otncabi/usr/palm/applications",
    "/mnt/otycabi/usr/palm/applications"
};
const size_t kMaximumAppInfoBytes = 4 * 1024 * 1024;

std::string rooted(const std::string &root, const std::string &path) {
    if (root.empty() || root == "/") return path;
    size_t end = root.size();
    while (end > 1 && root[end - 1] == '/') --end;
    return root.substr(0, end) + path;
}

std::string joined(const std::string &left, const std::string &right) {
    if (left.empty()) return right;
    if (left[left.size() - 1] == '/') return left + right;
    return left + "/" + right;
}

std::string directoryOf(const std::string &path) {
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return ".";
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

bool ensureDirectory(const std::string &path, std::string *error) {
    struct stat info = {};
    if (lstat(path.c_str(), &info) == 0) {
        if (S_ISDIR(info.st_mode)) return true;
        if (error) *error = path + " is not a directory";
        return false;
    }
    if (errno != ENOENT) {
        if (error) *error = "could not inspect " + path + ": " + std::strerror(errno);
        return false;
    }
    const std::string parent = directoryOf(path);
    if (parent != path && !ensureDirectory(parent, error)) return false;
    if (mkdir(path.c_str(), 0700) == 0 || errno == EEXIST) {
        if (lstat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode)) return true;
    }
    if (error) *error = "could not create " + path + ": " + std::strerror(errno);
    return false;
}

bool readRegularFile(const std::string &path, size_t maximumBytes,
                     std::string &contents, std::string *error) {
    const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
        if (error) *error = "could not read " + path + ": " + std::strerror(errno);
        return false;
    }
    struct stat info = {};
    if (fstat(descriptor, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0 ||
        static_cast<unsigned long long>(info.st_size) > maximumBytes) {
        if (error) *error = path + " is not a regular appinfo file or exceeds the size limit";
        close(descriptor);
        return false;
    }
    contents.clear();
    contents.reserve(static_cast<size_t>(info.st_size));
    char buffer[16384];
    while (true) {
        const ssize_t count = read(descriptor, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) {
            if (error) *error = "could not read " + path + ": " + std::strerror(errno);
            close(descriptor);
            return false;
        }
        if (count == 0) break;
        if (contents.size() + static_cast<size_t>(count) > maximumBytes) {
            if (error) *error = path + " exceeds the appinfo size limit";
            close(descriptor);
            return false;
        }
        contents.append(buffer, static_cast<size_t>(count));
    }
    close(descriptor);
    return true;
}

bool writeAtomic(const std::string &path, const std::string &contents,
                 std::string *error) {
    if (!ensureDirectory(directoryOf(path), error)) return false;
    std::string pattern = path + ".tmp.XXXXXX";
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    const int descriptor = mkstemp(name.data());
    if (descriptor < 0) {
        if (error) *error = "could not create override file: " + std::string(std::strerror(errno));
        return false;
    }
    const std::string temporary(name.data());
    bool ok = fchmod(descriptor, 0600) == 0;
    size_t offset = 0;
    while (ok && offset < contents.size()) {
        const ssize_t count = write(descriptor, contents.data() + offset,
                                    contents.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) ok = false;
        else offset += static_cast<size_t>(count);
    }
    if (ok && fsync(descriptor) != 0) ok = false;
    if (close(descriptor) != 0) ok = false;
    if (!ok || rename(temporary.c_str(), path.c_str()) != 0) {
        const int saved = errno;
        unlink(temporary.c_str());
        if (error) *error = "could not atomically write appinfo override: " +
                            std::string(std::strerror(saved));
        return false;
    }
    const int directory = open(directoryOf(path).c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory >= 0) {
        (void)fsync(directory);
        close(directory);
    }
    return true;
}

ChildResult runTool(ChildProcessRunner &children, const std::string &systemRoot,
                    const std::string &tool,
                    const std::vector<std::string> &arguments) {
    ChildOptions options;
    options.arguments.push_back(rooted(systemRoot, tool));
    options.arguments.insert(options.arguments.end(), arguments.begin(), arguments.end());
    options.timeout = std::chrono::milliseconds(4000);
    options.maximumOutputBytes = 4096;
    return children.run(options);
}

Json failure(const std::string &error) {
    Json result = Json::object();
    result.set("ok", Json::flag(false));
    result.set("error", Json::text(error));
    return result;
}

Json success(const std::string &appId, bool hidden) {
    Json result = Json::object();
    result.set("ok", Json::flag(true));
    result.set("id", Json::text(appId));
    result.set("hidden", Json::flag(hidden));
    return result;
}

} // namespace

bool appTileIdIsValid(const std::string &appId) {
    return service::appIdIsValid(appId);
}

bool appTileIdIsProtected(const std::string &appId) {
    return service::appIdIsProtected(appId);
}

AppTileManager::AppTileManager(ChildProcessRunner &children, std::string systemRoot,
                               std::string dataRoot)
    : children_(children), systemRoot_(std::move(systemRoot)),
      dataRoot_(std::move(dataRoot)) {}

std::vector<AppTileManager::Target> AppTileManager::targetsFor(
    const std::string &appId) const {
    std::vector<Target> targets;
    for (const char *base : kAppBases) {
        const std::string appDirectory = rooted(systemRoot_, joined(base, appId));
        const std::string appInfo = joined(appDirectory, "appinfo.json");
        struct stat info = {};
        if (stat(appInfo.c_str(), &info) == 0 && S_ISREG(info.st_mode))
            targets.push_back(Target{appInfo, appDirectory});
    }
    return targets;
}

std::string AppTileManager::overridePath(const std::string &appId) const {
    const std::string root = dataRoot_.empty() ? "/var/lib/tvweb" : dataRoot_;
    return joined(joined(root, "appinfo-overrides"), appId + ".json");
}

bool AppTileManager::unmountTargets(const std::vector<Target> &targets) {
    bool ok = true;
    for (const Target &target : targets) {
        ok = runTool(children_, systemRoot_, "/bin/umount", {"-l", target.appInfo}).succeeded() && ok;
        ok = runTool(children_, systemRoot_, "/bin/umount", {"-l", target.appDirectory}).succeeded() && ok;
        ok = runTool(children_, systemRoot_, "/bin/umount", {"-l", target.appInfo}).succeeded() && ok;
    }
    return ok;
}

bool AppTileManager::mountTargets(const std::string &overrideFile,
                                  const std::vector<Target> &targets,
                                  std::vector<std::string> *errors) {
    size_t mounted = 0;
    for (const Target &target : targets) {
        const ChildResult result = runTool(
            children_, systemRoot_, "/bin/mount", {"--bind", overrideFile, target.appInfo});
        if (result.succeeded()) {
            ++mounted;
        } else if (errors) {
            errors->push_back(result.standardError.empty()
                ? (result.error.empty() ? "mount failed" : result.error)
                : result.standardError);
        }
    }
    return mounted > 0;
}

bool AppTileManager::mountPersistedOverride(const std::string &appId) {
    if (!appTileIdIsValid(appId)) return false;
    const std::vector<Target> targets = targetsFor(appId);
    if (targets.empty()) return false;
    const std::string file = overridePath(appId);
    struct stat info = {};
    if (lstat(file.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) return false;
    return mountTargets(file, targets, nullptr);
}

Json AppTileManager::restoreHiddenAppsAtBoot(
    const Json &config, SamRescanManager &samRescan,
    const CommandPolicyContext &context) {
    std::lock_guard<std::mutex> lock(mutex_);
    Json response = Json::object();
    response.set("ok", Json::flag(true));

    std::string enabledText;
    if (!readRegularFile(joined(dataRoot_, "tile_hiding_enabled"), 16,
                         enabledText, nullptr)) {
        response.set("skipped", Json::flag(true));
        response.set("reason", Json::text("tile hiding is not enabled"));
        return response;
    }
    while (!enabledText.empty() &&
           (enabledText.back() == '\n' || enabledText.back() == '\r'))
        enabledText.pop_back();
    if (enabledText != "1") {
        response.set("skipped", Json::flag(true));
        response.set("reason", Json::text("tile hiding is not enabled"));
        return response;
    }

    const Json allowTileHiding = config["allowTileHiding"];
    bool allowed = allowTileHiding.valid() && allowTileHiding.boolean();
    if (!allowTileHiding.valid()) {
        std::string stockType;
        (void)readRegularFile(joined(dataRoot_, "screensaver-stock-type"), 64,
                              stockType, nullptr);
        while (!stockType.empty() &&
               (stockType.back() == '\n' || stockType.back() == '\r'))
            stockType.pop_back();
        const bool experimentalSaver =
            config["allowExperimentalScreensavers"].boolean() ||
            config["allowOnWebos10"].boolean();
        allowed = (stockType.empty() || stockType == "qml" || experimentalSaver);
    }
    if (!allowed) {
        response.set("skipped", Json::flag(true));
        response.set("reason", Json::text("allowTileHiding is not enabled"));
        return response;
    }

    struct stat homebrewMarker = {};
    if (stat(joined(dataRoot_, ".from-homebrew-channel").c_str(),
             &homebrewMarker) == 0 && S_ISREG(homebrewMarker.st_mode)) {
        response.set("skipped", Json::flag(true));
        response.set("reason", Json::text("Homebrew Channel install"));
        return response;
    }

    std::string hiddenApps;
    if (!readRegularFile(joined(dataRoot_, "hidden_apps"),
                         kMaximumAppInfoBytes, hiddenApps, nullptr)) {
        response.set("skipped", Json::flag(true));
        response.set("reason", Json::text("hidden app list is unavailable"));
        return response;
    }

    std::istringstream lines(hiddenApps);
    std::string appId;
    std::string firstHidden;
    size_t mountedCount = 0;
    while (std::getline(lines, appId)) {
        while (!appId.empty() && (appId.back() == '\r' || appId.back() == '\n'))
            appId.pop_back();
        if (appId.empty() || !appTileIdIsValid(appId)) continue;
        if (mountPersistedOverride(appId)) {
            ++mountedCount;
            if (firstHidden.empty()) firstHidden = appId;
        }
    }

    response.set("mountedCount", Json::numeric(static_cast<double>(mountedCount)));
    if (firstHidden.empty()) {
        response.set("mounted", Json::flag(false));
        return response;
    }

    Json expectedVisibility = Json::object();
    expectedVisibility.set(firstHidden, Json::flag(false));
    CommandPolicyContext maintenanceContext = context;
    // This is the persisted boot reconciliation allowed by allowTileHiding;
    // it must work when interactive TV controls are disabled.
    maintenanceContext.allowControl = true;
    const Json refreshed = samRescan.refreshAppVisibility(
        firstHidden, false, maintenanceContext);
    response.set("mounted", Json::flag(true));
    response.set("firstHidden", Json::text(firstHidden));
    response.set("samRefreshed", Json::flag(
        refreshed["result"].isBoolean() && refreshed["result"].boolean()));
    if (refreshed["error"].isString())
        response.set("samError", refreshed["error"]);
    return response;
}

Json AppTileManager::hide(const std::string &appId) {
    const std::vector<Target> targets = targetsFor(appId);
    if (targets.empty()) return failure("appinfo.json not found for " + appId);
    (void)unmountTargets(targets);

    std::string stockText;
    std::string error;
    if (!readRegularFile(targets.front().appInfo, kMaximumAppInfoBytes,
                         stockText, &error))
        return failure("Failed to read stock appinfo: " + error);
    Json stock = Json::parse(stockText);
    if (!stock.isObject()) return failure("Failed to read stock appinfo: malformed JSON");
    stock.set("visible", Json::flag(false));

    const std::string overrideFile = overridePath(appId);
    if (!writeAtomic(overrideFile, stock.dump(), &error))
        return failure("Failed to write override appinfo: " + error);

    std::vector<std::string> errors;
    if (!mountTargets(overrideFile, targets, &errors)) {
        std::ostringstream joinedErrors;
        for (size_t i = 0; i < errors.size(); ++i) {
            if (i) joinedErrors << "; ";
            joinedErrors << errors[i];
        }
        return failure("Bind mount failed: " + joinedErrors.str());
    }
    return success(appId, true);
}

Json AppTileManager::unhide(const std::string &appId) {
    (void)unmountTargets(targetsFor(appId));
    const std::string file = overridePath(appId);
    (void)unlink(file.c_str());
    return success(appId, false);
}

Json AppTileManager::setMounted(const std::string &appId, bool mounted) {
    const std::vector<Target> targets = targetsFor(appId);
    if (mounted) {
        const std::string file = overridePath(appId);
        struct stat info = {};
        if (lstat(file.c_str(), &info) != 0 || !S_ISREG(info.st_mode))
            return success(appId, true);
        (void)mountTargets(file, targets, nullptr);
    } else {
        (void)unmountTargets(targets);
    }
    return success(appId, true);
}

Json AppTileManager::apply(const std::string &action, const std::string &appId) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!appTileIdIsValid(appId)) return failure("Invalid app ID format");
    if (action == "hide") {
        if (appTileIdIsProtected(appId))
            return failure(say::msg("srv.apps.protectedHide", "Protected core system app cannot be hidden"));
        return hide(appId);
    }
    if (action == "unhide") return unhide(appId);
    if (action == "mount") return setMounted(appId, true);
    if (action == "unmount") return setMounted(appId, false);
    return failure("unknown app tile operation");
}

} // namespace daemon
} // namespace nativetv
