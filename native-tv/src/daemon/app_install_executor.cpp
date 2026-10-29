#include "daemon/app_install_executor.hpp"

#include "core/json.hpp"
#include "service/app_package_inspector.hpp"
#include "service/app_package_policy.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <memory>
#include <utility>

namespace nativetv {
namespace daemon {
namespace {
const size_t MAX_FRAME_BYTES = 64 * 1024;
const size_t MAX_STREAM_BYTES = 1024 * 1024;
const size_t MAX_TIMEOUT_MS = 120000;

bool isSha256(const std::string &value) {
    if (value.size() != 64) return false;
    for (unsigned char character : value)
        if (!((character >= '0' && character <= '9') ||
              (character >= 'a' && character <= 'f') ||
              (character >= 'A' && character <= 'F'))) return false;
    return true;
}

std::string lowercase(const std::string &value) {
    std::string result = value;
    for (char &character : result)
        if (character >= 'A' && character <= 'F')
            character = static_cast<char>(character - 'A' + 'a');
    return result;
}

bool validName(const std::string &value) {
    if (value.empty() || value.size() > 128 ||
        !std::isalnum(static_cast<unsigned char>(value[0]))) return false;
    for (unsigned char character : value)
        if (!std::isalnum(character) && character != '.' && character != '_' &&
            character != '+' && character != '-') return false;
    return true;
}

std::string firstString(const Json &first, const Json &second) {
    if (first.isString() && !first.string().empty()) return first.string();
    if (second.isString() && !second.string().empty()) return second.string();
    return std::string();
}

std::string errorCode(const Json &value) {
    if (value.isString()) return value.string();
    if (value.isNumber()) return compactNumber(value.number(), 0);
    return std::string();
}

std::string stagingPath(const std::string &directory, const std::string &name) {
    if (directory.empty()) return name;
    if (directory[directory.size() - 1] == '/') return directory + name;
    return directory + "/" + name;
}
} // namespace

AppInstallExecutor::AppInstallExecutor(ChildProcessRunner &children,
                                       std::string stagingDirectory,
                                       std::string lunaSendPath)
    : children_(children), stagingDirectory_(std::move(stagingDirectory)),
      lunaSendPath_(std::move(lunaSendPath)) {}

bool AppInstallExecutor::run(const std::string &packageId,
                             const std::string &fileName,
                             const std::string &expectedSha256,
                             std::chrono::milliseconds timeout,
                             AppInstallResult &result,
                             std::string *error) const {
    result = AppInstallResult();
    if (!validName(packageId)) {
        if (error) *error = "app package id is invalid";
        return false;
    }
    if (!isSha256(expectedSha256)) {
        if (error) *error = "verified package SHA-256 is invalid";
        return false;
    }
    if (timeout.count() < 1 ||
        static_cast<unsigned long long>(timeout.count()) > MAX_TIMEOUT_MS) {
        if (error) *error = "install timeout is outside the allowed range";
        return false;
    }

    Json packageInfo;
    std::string inspectError;
    if (!service::AppPackageInspector::inspect(stagingDirectory_, fileName,
                                                packageInfo, inspectError)) {
        if (error) *error = inspectError.empty() ? "the ipk could not be read" : inspectError;
        return false;
    }
    service::AppPackagePolicyFailure policyFailure;
    if (!service::AppPackagePolicy::validate(
            packageInfo, service::AppPackagePolicy::systemAppDirectories(),
            policyFailure)) {
        if (error) *error = "the ipk is not allowed: " + policyFailure.code;
        return false;
    }
    if (packageInfo["package"].string() != packageId) {
        if (error) *error = "staged package does not match the confirmed package";
        return false;
    }
    if (!isSha256(packageInfo["sha256"].string()) ||
        lowercase(packageInfo["sha256"].string()) != lowercase(expectedSha256)) {
        if (error) *error = "staged package changed after it was verified";
        return false;
    }
    result.packageVersion = packageInfo["version"].string();
    const Json packageApps = packageInfo["apps"];
    if (packageApps.isArray()) {
        for (const Json &app : packageApps.items()) {
            if (!app["id"].isString()) continue;
            result.apps.push_back({app["id"].string(), app["version"].string()});
        }
    }
    if (lunaSendPath_.empty()) {
        if (error) *error = "luna-send is unavailable";
        return false;
    }

    const std::string installFile = stagingPath(stagingDirectory_, fileName);
    Json payload = Json::object();
    payload.set("id", Json::text(packageId));
    payload.set("ipkUrl", Json::text(installFile));
    payload.set("subscribe", Json::flag(true));

    bool terminal = false;
    size_t streamBytes = 0;
    std::shared_ptr<std::atomic<bool>> cancellation(new std::atomic<bool>(false));
    std::string frameBuffer;
    const auto readFrame = [&result, &terminal, &cancellation](const std::string &frame) {
        Json response = Json::parse(frame);
        if (!response.isObject() || terminal) return;
        const Json details = response["details"];
        if (response["returnValue"].isBoolean() &&
            !response["returnValue"].boolean()) {
            result.outcome = AppInstallOutcome::Failed;
            result.error = firstString(response["errorText"], details["reason"]);
            if (result.error.empty()) result.error = "the install service rejected the package";
            terminal = true;
        } else if ((details["errorCode"].isString() &&
                    !details["errorCode"].string().empty()) ||
                   (details["errorCode"].isNumber() &&
                    details["errorCode"].number() != 0) ||
                   (details["errorCode"].isBoolean() &&
                    details["errorCode"].boolean())) {
            result.outcome = AppInstallOutcome::Failed;
            result.error = firstString(details["reason"], response["errorText"]);
            if (result.error.empty()) result.error = errorCode(details["errorCode"]);
            terminal = true;
        } else {
            const Json statusValue = response["statusValue"].valid()
                ? response["statusValue"] : details["statusValue"];
            const bool finished = statusValue.isNumber() && statusValue.number() == 30 &&
                                  details["packageId"].isString() &&
                                  !details["packageId"].string().empty();
            const bool installed = response["state"].string() == "installed" ||
                                   details["state"].string() == "installed";
            if (finished || installed) {
                result.outcome = AppInstallOutcome::Installed;
                terminal = true;
            }
        }
        if (terminal) cancellation->store(true, std::memory_order_relaxed);
    };

    ChildOptions options;
    options.arguments = {lunaSendPath_, "-i",
                         "luna://com.webos.appInstallService/dev/install",
                         payload.dump()};
    options.timeout = timeout;
    options.captureOutput = false;
    options.cancellation = cancellation;
    options.onStandardOutput = [&frameBuffer, &readFrame, &result, &terminal,
                                &streamBytes,
                                &cancellation](const std::string &chunk) {
        if (terminal) return;
        if (chunk.size() > MAX_STREAM_BYTES - streamBytes) {
            result.outcome = AppInstallOutcome::Failed;
            result.error = "the install service stream exceeded its limit";
            terminal = true;
            cancellation->store(true, std::memory_order_relaxed);
            frameBuffer.clear();
            return;
        }
        streamBytes += chunk.size();
        frameBuffer += chunk;
        if (frameBuffer.size() > MAX_FRAME_BYTES) {
            result.outcome = AppInstallOutcome::Failed;
            result.error = "the install service response exceeded its limit";
            terminal = true;
            cancellation->store(true, std::memory_order_relaxed);
            frameBuffer.clear();
            return;
        }
        size_t newline = frameBuffer.find('\n');
        while (newline != std::string::npos && !terminal) {
            const std::string frame = frameBuffer.substr(0, newline);
            frameBuffer.erase(0, newline + 1);
            if (!frame.empty()) readFrame(frame);
            newline = frameBuffer.find('\n');
        }
    };

    const ChildResult child = children_.run(options);
    if (!terminal && !frameBuffer.empty()) readFrame(frameBuffer);
    if (terminal) return true;
    if (child.timedOut) {
        result.outcome = AppInstallOutcome::TimedOut;
    } else {
        result.outcome = AppInstallOutcome::Closed;
    }
    return true;
}

} // namespace daemon
} // namespace nativetv
