#include "daemon/home_takeover_manager.hpp"

#include "daemon/child_process.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <utility>
#include <vector>

namespace nativetv {
namespace daemon {
namespace {
std::string rooted(const std::string &root, const std::string &path) {
    if (root.empty() || root == "/") return path;
    size_t end = root.size();
    while (end > 1 && root[end - 1] == '/') --end;
    return root.substr(0, end) + path;
}

std::string findExecutable(const std::string &root,
                           const std::vector<std::string> &paths) {
    for (const std::string &path : paths) {
        const std::string candidate = rooted(root, path);
        if (access(candidate.c_str(), X_OK) == 0) return candidate;
    }
    return std::string();
}

ChildResult run(ChildProcessRunner &children, const std::string &executable,
                const std::vector<std::string> &arguments,
                std::chrono::milliseconds timeout) {
    ChildOptions options;
    options.arguments.push_back(executable);
    options.arguments.insert(options.arguments.end(), arguments.begin(),
                             arguments.end());
    options.timeout = timeout;
    options.maximumOutputBytes = 4096;
    return children.run(options);
}

std::string commandError(const ChildResult &result, const std::string &command) {
    if (!result.error.empty()) return result.error;
    if (result.timedOut) return command + " timed out";
    if (!result.standardError.empty()) return result.standardError;
    return command + " exited with status " + std::to_string(result.exitCode);
}

Json failure(const std::string &error) {
    Json result = Json::object();
    result.set("ok", Json::flag(false));
    result.set("error", Json::text(error));
    return result;
}

Json success(bool changed, bool mounted, bool already) {
    Json result = Json::object();
    result.set("ok", Json::flag(true));
    result.set("changed", Json::flag(changed));
    result.set("mounted", Json::flag(mounted));
    result.set("already", Json::flag(already));
    return result;
}
} // namespace

HomeTakeoverManager::HomeTakeoverManager(ChildProcessRunner &children,
                                         std::string sourceRoot,
                                         std::string stagingRoot,
                                         std::string systemRoot, int port)
    : children_(children),
      stager_(std::move(sourceRoot), stagingRoot, port),
      stagingRoot_(std::move(stagingRoot)),
      systemRoot_(std::move(systemRoot)),
      appDirectory_(rooted(systemRoot_, "/usr/palm/applications/com.webos.app.home")) {}

bool HomeTakeoverManager::mounted() const {
    struct stat stagedInfo;
    struct stat appInfo;
    if (stat(stagingRoot_.c_str(), &stagedInfo) != 0 ||
        stat(appDirectory_.c_str(), &appInfo) != 0)
        return false;
    return stagedInfo.st_dev == appInfo.st_dev && stagedInfo.st_ino == appInfo.st_ino;
}

bool HomeTakeoverManager::unmount(std::string &error) const {
    if (!mounted()) return true;
    const std::string executable = findExecutable(
        systemRoot_, {"/bin/umount", "/usr/bin/umount"});
    if (executable.empty()) {
        error = "the umount command is unavailable";
        return false;
    }

    const ChildResult regular = run(children_, executable, {appDirectory_},
                                    std::chrono::milliseconds(4000));
    if (!regular.succeeded() || mounted()) {
        const ChildResult lazy = run(children_, executable, {"-l", appDirectory_},
                                     std::chrono::milliseconds(4000));
        if (!lazy.succeeded() && mounted()) {
            error = commandError(lazy, "lazy umount");
            return false;
        }
    }
    if (mounted()) {
        error = "LG's Home mount is still held";
        return false;
    }
    return true;
}

Json HomeTakeoverManager::takeover() {
    std::lock_guard<std::mutex> lock(mutex_);
    const Json staged = stager_.stage();
    if (!staged["ok"].boolean()) return staged;
    const bool changed = staged["changed"].boolean();
    if (mounted()) return success(changed, true, true);

    struct stat targetInfo;
    if (stat(appDirectory_.c_str(), &targetInfo) != 0 ||
        !S_ISDIR(targetInfo.st_mode))
        return failure("LG's Home app directory is unavailable");

    const std::string executable = findExecutable(
        systemRoot_, {"/bin/mount", "/usr/bin/mount"});
    if (executable.empty()) return failure("the mount command is unavailable");
    const ChildResult mountedResult = run(
        children_, executable, {"--bind", stagingRoot_, appDirectory_},
        std::chrono::milliseconds(4000));
    if (!mountedResult.succeeded() || !mounted()) {
        std::string rollbackError;
        if (mounted() && !unmount(rollbackError))
            return failure(commandError(mountedResult, "Home bind mount") +
                           "; rollback failed: " + rollbackError);
        return failure(!mountedResult.succeeded()
            ? commandError(mountedResult, "Home bind mount")
            : "the Home bind mount did not take");
    }
    return success(changed, true, false);
}

Json HomeTakeoverManager::release() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!mounted()) return success(false, false, true);
    std::string error;
    if (!unmount(error)) return failure(error);
    return success(false, false, false);
}

} // namespace daemon
} // namespace nativetv
