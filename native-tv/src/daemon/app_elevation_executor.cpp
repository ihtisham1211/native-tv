#include "daemon/app_elevation_executor.hpp"
#include <unistd.h>
#include <utility>

namespace nativetv {
namespace daemon {
namespace {
const size_t MAX_SERVICES = 256;
const size_t MAX_CAPTURED_OUTPUT = 64 * 1024;
const char *ELEVATE_SERVICE_PATH =
    "/media/developer/apps/usr/palm/services/"
    "org.webosbrew.hbchannel.service/elevate-service";

std::string rooted(const std::string &root, const std::string &path) {
    if (root.empty() || root == "/") return path;
    size_t end = root.size();
    while (end > 1 && root[end - 1] == '/') --end;
    return root.substr(0, end) + path;
}

bool validName(const std::string &value) {
    if (value.empty() || value.size() > 128 ||
        !((value[0] >= 'a' && value[0] <= 'z') ||
          (value[0] >= 'A' && value[0] <= 'Z') ||
          (value[0] >= '0' && value[0] <= '9'))) return false;
    for (unsigned char character : value)
        if (!((character >= 'a' && character <= 'z') ||
              (character >= 'A' && character <= 'Z') ||
              (character >= '0' && character <= '9') || character == '.' ||
              character == '_' || character == '+' || character == '-')) return false;
    return true;
}

std::string firstLine(const std::string &text) {
    const size_t start = text.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return std::string();
    const size_t end = text.find_first_of("\r\n", start);
    std::string line = text.substr(start, end == std::string::npos
                                           ? std::string::npos : end - start);
    if (line.size() > 1024) line.resize(1024);
    return line;
}

std::string failureText(const ChildResult &result) {
    std::string detail = firstLine(result.standardError);
    if (!detail.empty()) return detail;
    if (result.timedOut) return "elevation command timed out";
    if (result.cancelled) return "elevation command was cancelled";
    if (result.outputLimitExceeded) return "elevation command output exceeded its limit";
    if (!result.error.empty()) return result.error;
    if (result.signal != 0)
        return "elevation command terminated by signal " + std::to_string(result.signal);
    return "elevation command exited with status " + std::to_string(result.exitCode);
}
} // namespace

AppElevationExecutor::AppElevationExecutor(ChildProcessRunner &children,
                                           std::string systemRoot,
                                           std::chrono::milliseconds timeout)
    : children_(children), systemRoot_(std::move(systemRoot)), timeout_(timeout) {}

bool AppElevationExecutor::run(const std::string &packageId,
                              const std::vector<std::string> &services,
                              AppElevationResult &result, std::string *error) const {
    result = AppElevationResult();
    if (!validName(packageId)) {
        if (error) *error = "app package id is invalid";
        return false;
    }
    if (services.empty() || services.size() > MAX_SERVICES) {
        if (error) *error = "elevated services must contain between 1 and 256 names";
        return false;
    }
    const std::string prefix = packageId + ".";
    for (const std::string &service : services) {
        if (!validName(service) || service.compare(0, prefix.size(), prefix) != 0 ||
            service.size() == prefix.size()) {
            if (error) *error = "elevated service must belong to its package";
            return false;
        }
    }

    const std::string executable = rooted(systemRoot_, ELEVATE_SERVICE_PATH);
    if (access(executable.c_str(), X_OK) != 0) {
        for (const std::string &service : services)
            result.failed.push_back({service, "Homebrew Channel elevate-service is unavailable"});
        return true;
    }

    for (const std::string &service : services) {
        ChildOptions options;
        options.arguments = {executable, service};
        options.timeout = timeout_;
        options.maximumOutputBytes = MAX_CAPTURED_OUTPUT;
        const ChildResult child = children_.run(options);
        if (child.succeeded()) {
            result.done.push_back(service);
        } else {
            result.failed.push_back({service, failureText(child)});
        }
    }
    return true;
}

} // namespace daemon
} // namespace nativetv
