#include "daemon/release_update_steps.hpp"

#include "core/sha256.hpp"
#include "daemon/app_install_executor.hpp"
#include "domain/say.hpp"
#include "service/app_install_space.hpp"
#include "service/release_check.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <initializer_list>
#include <spawn.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern char **environ;

namespace nativetv {
namespace daemon {
namespace {

// A package is about 25 MB; the limit is for a server sending more than that.
const uint64_t kMaximumPackageBytes = 256ULL * 1024 * 1024;
// How long the TV's app installer, and then the app's, may take.
const std::chrono::milliseconds kPackageInstallTimeout(120000);
const std::chrono::milliseconds kServerInstallTimeout(5 * 60 * 1000);

std::string msg(const char *key, const char *english,
                std::initializer_list<std::pair<const char *, std::string>> values = {}) {
    Json variables = Json::object();
    for (const auto &value : values) variables.set(value.first, Json::text(value.second));
    return say::msg(key, english, variables);
}

std::string joinPath(const std::string &directory, const std::string &name) {
    return directory + "/" + name;
}

std::string readTrimmed(const std::string &path) {
    std::ifstream input(path.c_str());
    std::stringstream text;
    text << input.rdbuf();
    std::string value = text.str();
    const size_t end = value.find_last_not_of(" \t\r\n");
    value.erase(end == std::string::npos ? 0 : end + 1);
    const size_t begin = value.find_first_not_of(" \t\r\n");
    return begin == std::string::npos ? std::string() : value.substr(begin);
}

bool exists(const std::string &path) {
    struct stat info = {};
    return stat(path.c_str(), &info) == 0;
}

// Where the app package put its files: install.sh wrote it down when it took the
// server over, and a server deploy.sh installed has none.
std::string appDirectory(const ReleaseUpdateEnvironment &environment) {
    const std::string directory = readTrimmed(joinPath(environment.dataRoot, ".from-homebrew-channel"));
    const bool safe = directory.size() > 1 && directory[0] == '/' &&
                      directory.find("..") == std::string::npos &&
                      directory.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                                  "0123456789._+-/") == std::string::npos;
    return safe ? directory : std::string();
}

std::string unavailableReason(const ReleaseUpdateEnvironment &environment) {
    if (environment.serverVersion.find('+') != std::string::npos)
        return msg("srv.update.source",
                   "this server was built from a git checkout: pull the release and deploy it again");
    if (!exists(joinPath(environment.dataRoot, ".from-homebrew-channel")))
        return msg("srv.update.notPackage",
                   "this server was not installed from the Native Tv app package, so it is updated by deploying it again");
    const std::string app = appDirectory(environment);
    if (app.empty() || !exists(joinPath(app, "payload/install.sh")))
        return msg("srv.update.appGone", "the Native Tv app is no longer installed on the TV");
    return std::string();
}

std::string stagedFile(const ReleaseUpdateEnvironment &environment, const service::ReleaseOffer &offer) {
    return joinPath(environment.stagingDirectory, service::releasePackageFile(offer.version));
}

bool downloadPackage(const ReleaseUpdateEnvironment &environment, const service::ReleaseOffer &offer,
                     const std::function<void(uint64_t)> &progress, const std::function<bool()> &stopping,
                     std::string &error) {
    (void)mkdir(environment.stagingDirectory.c_str(), 0700);
    uint64_t freeBytes = 0;
    const bool freeKnown = service::AppInstallSpace::availableBytes(environment.stagingDirectory, freeBytes);
    service::AppInstallDownloadCapacity capacity;
    if (!service::AppInstallSpace::downloadCapacity(kMaximumPackageBytes, 0, freeKnown, freeBytes, capacity) ||
        !capacity.capacityOkay) {
        error = msg("srv.update.noSpace", "there is not enough free space on the TV to download the update");
        return false;
    }
    const std::string path = stagedFile(environment, offer);
    (void)unlink(path.c_str());
    const int output = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (output < 0) {
        error = "could not create " + path + ": " + std::strerror(errno);
        return false;
    }
    uint64_t received = 0;
    std::string writeError;
    const service::AppPackageDownloadSink sink = [&](const char *bytes, size_t length) {
        size_t offset = 0;
        while (offset < length) {
            const ssize_t written = write(output, bytes + offset, length - offset);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) {
                writeError = std::string("could not write the package: ") + std::strerror(errno);
                return false;
            }
            offset += static_cast<size_t>(written);
        }
        received += length;
        progress(received);
        return true;
    };
    long status = 0;
    service::AppPackageDownloadFailure failure;
    const service::AppPackageDownload &download = environment.downloader
        ? environment.downloader : service::AppPackageDownload(&service::AppPackageFetcher::download);
    bool ok = download(offer.ipkUrl, environment.userAgent,
                       environment.ipPreference ? environment.ipPreference() : std::string(),
                       capacity.maxDownloadBytes, sink, status, failure, stopping);
    ok = ok && fsync(output) == 0;
    ok = (close(output) == 0) && ok;
    if (!ok) {
        error = !writeError.empty() ? writeError
              : failure.offline ? std::string("GitHub could not be reached")
              : failure.message.empty() ? std::string("the package could not be downloaded") : failure.message;
        return false;
    }
    return true;
}

bool verifyPackage(const ReleaseUpdateEnvironment &environment, const service::ReleaseOffer &offer,
                   std::string &error) {
    std::ifstream input(stagedFile(environment, offer).c_str(), std::ios::binary);
    crypto::Sha256 hash;
    char block[16 * 1024];
    while (input.read(block, sizeof(block)) || input.gcount() > 0)
        hash.update(block, static_cast<size_t>(input.gcount()));
    if (input.bad() || crypto::hexDigest(hash.finish()) != offer.sha256) {
        error = msg("srv.update.badHash", "the downloaded package does not match the release's checksum");
        return false;
    }
    return true;
}

bool installPackage(const ReleaseUpdateEnvironment &environment, const service::ReleaseOffer &offer,
                    std::string &error) {
    if (!environment.children) {
        error = "child processes are unavailable";
        return false;
    }
    AppInstallExecutor executor(*environment.children, environment.stagingDirectory,
                                environment.lunaSendPath);
    AppInstallResult result;
    if (!executor.run("io.github.ihtisham1211.nativetv", service::releasePackageFile(offer.version),
                      offer.sha256, kPackageInstallTimeout, result, &error, true))
        return false;
    if (result.outcome == AppInstallOutcome::Failed) {
        error = msg("srv.update.installFailed", "the TV's app installer failed: {error}", {{"error", result.error}});
        return false;
    }
    // Closed and TimedOut can still have installed it: applyPackage looks at the files.
    return true;
}

// The app's own installer, as the launch page runs it, but by the server.
bool applyPackage(const ReleaseUpdateEnvironment &environment, const service::ReleaseOffer &offer,
                  const std::function<bool()> &stopping, std::string &error) {
    const std::string app = appDirectory(environment);
    const std::string script = joinPath(app, "payload/install.sh");
    // The installer of the package that is still there would move the old server over.
    const auto deadline = std::chrono::steady_clock::now() + environment.settleTime;
    while (readTrimmed(joinPath(app, "payload/tvweb/VERSION")) != offer.version) {
        if (stopping() || std::chrono::steady_clock::now() > deadline) {
            error = msg("srv.update.notReplaced",
                        "the TV's app installer did not replace the app with version {version}",
                        {{"version", offer.version}});
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    if (!exists(script)) {
        error = msg("srv.update.appGone", "the Native Tv app is no longer installed on the TV");
        return false;
    }
    // Its output goes to a file, and it is a process group of its own: it restarts this
    // server, which would close a pipe under it, and a write to a closed pipe kills a shell.
    const std::string log = joinPath(environment.dataRoot, "update.log");
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    posix_spawn_file_actions_init(&actions);
    posix_spawnattr_init(&attributes);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attributes, 0);
    char shell[] = "/bin/sh";
    std::string scriptCopy = script;
    char *const arguments[] = {shell, &scriptCopy[0], nullptr};
    pid_t installer = -1;
    const int spawned = posix_spawn(&installer, shell, &actions, &attributes, arguments, environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    if (spawned != 0) {
        error = std::string("could not run the app's installer: ") + std::strerror(spawned);
        return false;
    }
    // An installer that works ends this server, so the wait is for one that does not.
    const auto giveUp = std::chrono::steady_clock::now() + kServerInstallTimeout;
    int status = 0;
    while (waitpid(installer, &status, WNOHANG) == 0) {
        if (stopping() || std::chrono::steady_clock::now() > giveUp) {
            error = msg("srv.update.notUpgraded",
                        "the app's installer did not move the server over to version {version}",
                        {{"version", offer.version}});
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    std::string last;
    std::istringstream lines(readTrimmed(log));
    for (std::string line; std::getline(lines, line);) last = line;
    const Json answer = Json::parse(last);
    if (answer["ok"].isBoolean() && answer["ok"].boolean() && answer["action"].string() == "upgraded") return true;
    error = answer["error"].isString() && !answer["error"].string().empty()
        ? answer["error"].string()
        : msg("srv.update.notUpgraded", "the app's installer did not move the server over to version {version}",
                   {{"version", offer.version}});
    return false;
}

} // namespace

service::ReleaseUpdateSteps productionReleaseUpdateSteps(const ReleaseUpdateEnvironment &environment) {
    service::ReleaseUpdateSteps steps;
    const ReleaseUpdateEnvironment env = environment;
    const service::ReleaseFetch fetch = service::fetchOverCurl(env.userAgent, env.ipPreference);
    steps.unavailable = [env]() { return unavailableReason(env); };
    steps.lastFailure = [env](std::string &version, std::string &error) {
        const Json failure = Json::parse(readTrimmed(joinPath(env.dataRoot, ".update-failed")));
        if (!failure["version"].isString()) return false;
        version = failure["version"].string();
        error = failure["error"].string();
        return true;
    };
    steps.readManifest = [fetch](const std::string &version, service::ReleaseOffer &offer, std::string &error) {
        std::string body;
        return fetch(service::releaseManifestUrl(version), body, error) &&
               service::parseReleaseManifest(body, version, offer, error);
    };
    steps.download = [env](const service::ReleaseOffer &offer, const std::function<void(uint64_t)> &progress,
                           const std::function<bool()> &stopping, std::string &error) {
        return downloadPackage(env, offer, progress, stopping, error);
    };
    steps.verify = [env](const service::ReleaseOffer &offer, std::string &error) {
        return verifyPackage(env, offer, error);
    };
    steps.installPackage = [env](const service::ReleaseOffer &offer, std::string &error) {
        return installPackage(env, offer, error);
    };
    steps.applyPackage = [env](const service::ReleaseOffer &offer, const std::function<bool()> &stopping,
                               std::string &error) { return applyPackage(env, offer, stopping, error); };
    // The staging folder holds this update's package and nothing else.
    steps.discard = [env]() {
        DIR *directory = opendir(env.stagingDirectory.c_str());
        if (!directory) return;
        std::vector<std::string> names;
        while (const dirent *entry = readdir(directory))
            if (std::strcmp(entry->d_name, ".") != 0 && std::strcmp(entry->d_name, "..") != 0)
                names.push_back(entry->d_name);
        closedir(directory);
        for (const std::string &name : names) (void)unlink(joinPath(env.stagingDirectory, name).c_str());
    };
    return steps;
}

} // namespace daemon
} // namespace nativetv
