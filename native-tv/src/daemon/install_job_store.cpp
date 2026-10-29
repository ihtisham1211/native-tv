#include "daemon/install_job_store.hpp"
#include "daemon/json_file.hpp"
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace nativetv {
namespace daemon {
namespace {
bool validateJob(const Json &job, std::string *error) {
    if (!job.isObject() || !job["jobId"].isString() || job["jobId"].string().empty() ||
        job["jobId"].string().size() > 128 || !job["state"].isString()) {
        if (error) *error = "installer job id and state are required";
        return false;
    }
    const std::string state = job["state"].string();
    const bool knownState = state == "downloading" || state == "verifying" ||
        state == "awaiting-confirm" || state == "installing" ||
        state == "elevating" || state == "installed" || state == "error" ||
        state == "interrupted";
    if (!knownState) {
        if (error) *error = "installer job state is unknown";
        return false;
    }
    const Json package = job["package"];
    if (package.valid() && (!package.isString() || package.string().size() > 128)) {
        if (error) *error = "installer job package is invalid";
        return false;
    }
    const Json version = job["version"];
    if (version.valid() && (!version.isString() || version.string().size() > 64)) {
        if (error) *error = "installer job version is invalid";
        return false;
    }
    const Json updated = job["updated"];
    if (updated.valid() && (!updated.isNumber() || updated.number(-1) < 0)) {
        if (error) *error = "installer job timestamp is invalid";
        return false;
    }
    return true;
}

bool initialState(const std::string &state) {
    return state == "downloading" || state == "verifying";
}

bool terminalState(const std::string &state) {
    return state == "installed" || state == "error" || state == "interrupted";
}

bool allowedTransition(const std::string &from, const std::string &to) {
    if (from == to) return true;
    if (from == "downloading")
        return to == "verifying" || to == "error" || to == "interrupted";
    if (from == "verifying")
        return to == "awaiting-confirm" || to == "error" || to == "interrupted";
    if (from == "awaiting-confirm")
        return to == "installing" || to == "error";
    if (from == "installing")
        return to == "elevating" || to == "installed" || to == "error" ||
               to == "interrupted";
    if (from == "elevating")
        return to == "installed" || to == "error" || to == "interrupted";
    return false;
}

bool syncDirectory(const std::string &path, std::string *error) {
    const size_t slash = path.find_last_of('/');
    const std::string directory = slash == std::string::npos ? "." :
        (slash == 0 ? "/" : path.substr(0, slash));
    const int descriptor = open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0) {
        if (error) *error = "open installer state directory: " +
                            std::string(std::strerror(errno));
        return false;
    }
    const int result = fsync(descriptor);
    const int savedError = errno;
    close(descriptor);
    if (result != 0) {
        if (error) *error = "sync installer state directory: " +
                            std::string(std::strerror(savedError));
        return false;
    }
    return true;
}
} // namespace

bool readInstallerJob(const std::string &path, Json &job, bool &found,
                      std::string *error) {
    const JsonFileResult loaded = JsonFile(path, 4096).load();
    found = loaded.found;
    if (!loaded.error.empty()) {
        if (error) *error = loaded.error;
        return false;
    }
    if (!found) {
        job = Json();
        return true;
    }
    if (!validateJob(loaded.value, error)) return false;
    job = loaded.value;
    return true;
}

bool recoverInstallerJob(const std::string &path, Json &job, bool &found,
                         std::string *error) {
    Json stored;
    if (!readInstallerJob(path, stored, found, error)) return false;
    if (!found) {
        job = Json();
        return true;
    }

    const std::string state = stored["state"].string();
    if (state == "interrupted") {
        if (!clearInstallerJob(path, error)) return false;
        found = false;
        job = Json();
        return true;
    }
    if (state != "downloading" && state != "verifying" &&
        state != "installing" && state != "elevating") {
        found = false;
        job = Json();
        return true;
    }

    stored.set("state", Json::text("interrupted"));
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto updated = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    stored.set("updated", Json::numeric(static_cast<double>(updated)));
    if (!recordInstallerJob(path, stored, error)) return false;
    job = stored;
    found = true;
    return true;
}

bool recordInstallerJob(const std::string &path, const Json &job,
                        std::string *error) {
    if (!validateJob(job, error)) return false;
    Json normalized = Json::object();
    normalized.set("jobId", job["jobId"]);
    normalized.set("state", job["state"]);
    normalized.set("package", job["package"]);
    normalized.set("version", job["version"]);
    normalized.set("updated", job["updated"]);
    return JsonFile(path, 4096).save(normalized, error);
}

bool transitionInstallerJob(const std::string &path, const Json &job,
                            std::string *error) {
    if (!validateJob(job, error)) return false;
    Json previous;
    bool found = false;
    if (!readInstallerJob(path, previous, found, error)) return false;
    const std::string nextState = job["state"].string();
    if (!found) {
        if (!initialState(nextState)) {
            if (error) *error = "installer job must start in downloading or verifying";
            return false;
        }
    } else if (previous["jobId"].string() == job["jobId"].string()) {
        if (!allowedTransition(previous["state"].string(), nextState)) {
            if (error) *error = "invalid installer job state transition";
            return false;
        }
    } else if (!terminalState(previous["state"].string()) ||
               !initialState(nextState)) {
        if (error) *error = "a new installer job cannot replace an active job";
        return false;
    }
    return recordInstallerJob(path, job, error);
}

bool clearInstallerJob(const std::string &path, std::string *error) {
    bool changed = false;
    const std::string files[] = {path, path + ".last-good"};
    for (const std::string &file : files) {
        if (unlink(file.c_str()) == 0) {
            changed = true;
        } else if (errno != ENOENT) {
            if (error) *error = "remove installer job state: " +
                                std::string(std::strerror(errno));
            return false;
        }
    }
    return !changed || syncDirectory(path, error);
}

} // namespace daemon
} // namespace nativetv
