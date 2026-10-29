#include "service/app_install_preview.hpp"
#include "service/app_ids.hpp"

#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

namespace nativetv {
namespace service {
namespace {
bool boundedString(const Json &value, size_t maximum) {
    return value.isString() && !value.string().empty() &&
           value.string().size() <= maximum;
}

bool booleanField(const Json &input, const std::string &key) {
    return input.has(key) && input[key].isBoolean();
}

bool stringArray(const Json &value, size_t maximumItems, size_t maximumString) {
    if (!value.isArray() || value.size() > maximumItems) return false;
    for (const Json &item : value.items())
        if (!boundedString(item, maximumString)) return false;
    return true;
}

bool appArray(const Json &value) {
    if (!value.isArray() || value.size() > 20000) return false;
    for (const Json &app : value.items()) {
        if (!app.isObject() || !boundedString(app["id"], 128) ||
            !appPackageIdIsSafe(app["id"].string())) return false;
    }
    return true;
}

bool installedAppsArray(const Json &value) {
    if (!value.isArray() || value.size() > 20000) return false;
    for (const Json &app : value.items())
        if (!app.isObject() || !boundedString(app["id"], 128) ||
            !appPackageIdIsSafe(app["id"].string()) ||
            !app["version"].isString() || app["version"].string().size() > 64)
            return false;
    return true;
}

bool sha256(const Json &value) {
    if (!value.isString() || value.string().size() != 64) return false;
    for (char ch : value.string()) {
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
              (ch >= 'A' && ch <= 'F')))
            return false;
    }
    return true;
}

std::vector<double> versionParts(const std::string &value) {
    std::vector<double> parts;
    size_t cursor = 0;
    while (cursor < value.size()) {
        if (value[cursor] < '0' || value[cursor] > '9') {
            ++cursor;
            continue;
        }
        const size_t start = cursor;
        while (cursor < value.size() && value[cursor] >= '0' && value[cursor] <= '9')
            ++cursor;
        parts.push_back(std::strtod(value.substr(start, cursor - start).c_str(), nullptr));
    }
    return parts;
}

int compareVersion(const std::string &left, const std::string &right) {
    const std::vector<double> a = versionParts(left);
    const std::vector<double> b = versionParts(right);
    for (size_t i = 0; i < (a.size() > b.size() ? a.size() : b.size()); ++i) {
        const double p = i < a.size() ? a[i] : 0;
        const double q = i < b.size() ? b[i] : 0;
        if (p != q) return p > q ? 1 : -1;
    }
    return 0;
}

bool containsScreensaver(const std::string &value) {
    const std::string needle = "screensaver";
    for (size_t i = 0; i + needle.size() <= value.size(); ++i) {
        bool matches = true;
        for (size_t j = 0; j < needle.size(); ++j) {
            char ch = value[i + j];
            if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
            if (ch != needle[j]) {
                matches = false;
                break;
            }
        }
        if (matches) return true;
    }
    return false;
}
} // namespace

bool AppInstallPreview::build(const Json &input, Json &preview, bool &needsReview,
                              bool &capacityOkay, std::string &error) {
    preview = Json();
    needsReview = false;
    capacityOkay = false;
    error.clear();

    const Json freeBytes = input["freeBytes"];
    const Json packageBytes = input["packageBytes"];
    const Json apps = input["apps"];
    const Json installedApps = input["installedApps"];
    const Json services = input["services"];
    const Json reelevate = input["reelevate"];
    if (!input.isObject() || !boundedString(input["jobId"], 128) ||
        !boundedString(input["source"], 16) ||
        (input["source"].string() != "catalog" &&
         input["source"].string() != "url" && input["source"].string() != "file") ||
        !boundedString(input["package"], 128) ||
        !appPackageIdIsSafe(input["package"].string()) ||
        !boundedString(input["version"], 64) ||
        !appArray(apps) || !installedAppsArray(installedApps) ||
        !stringArray(services, 20000, 256) ||
        !stringArray(reelevate, 20000, 256) ||
        !booleanField(input, "rootRequired") || !booleanField(input, "pkgVetted") ||
        !booleanField(input, "cpuMismatch") || !booleanField(input, "storeInstalled") ||
        !booleanField(input, "elevationAvailable") ||
        !packageBytes.isNumber() || !std::isfinite(packageBytes.number()) ||
        packageBytes.number() < 1 || packageBytes.number() > 536870912 ||
        std::floor(packageBytes.number()) != packageBytes.number() ||
        !input.has("freeBytes") ||
        (freeBytes.valid() && (!freeBytes.isNumber() || !std::isfinite(freeBytes.number()) ||
                               freeBytes.number() < 0)) ||
        !sha256(input["sha256"])) {
        error = "invalid installer preview input";
        return false;
    }

    bool isScreensaver = containsScreensaver(input["package"].string());
    for (const Json &app : apps.items())
        if (containsScreensaver(app["id"].string())) isScreensaver = true;

    const bool vetted = input["source"].string() == "catalog" &&
                        input["pkgVetted"].boolean();
    const bool rootRequired = input["rootRequired"].boolean();
    const bool cpuMismatch = input["cpuMismatch"].boolean();
    const bool storeInstalled = input["storeInstalled"].boolean();
    const bool elevationAvailable = input["elevationAvailable"].boolean();
    const Json needBytes = Json::numeric(packageBytes.number() * 2);
    capacityOkay = !freeBytes.valid() || freeBytes.number() >= needBytes.number();
    std::string primaryApp = input["package"].string();
    bool primaryIsPackageApp = false;
    for (const Json &app : apps.items())
        if (app["id"].string() == primaryApp) primaryIsPackageApp = true;
    if (!primaryIsPackageApp && !apps.items().empty())
        primaryApp = apps.items().front()["id"].string();
    Json installedVersion;
    for (const Json &app : installedApps.items())
        if (app["id"].string() == primaryApp) installedVersion = app["version"];
    const std::string version = input["version"].string();
    std::string direction = "new";
    if (installedVersion.isString()) {
        const int order = compareVersion(version, installedVersion.string());
        direction = order > 0 ? "up" : (order < 0 ? "down" : "same");
    }
    const bool unvettedRoot = !vetted && rootRequired && services.size() > 0 &&
                              elevationAvailable;
    needsReview = unvettedRoot || storeInstalled || cpuMismatch ||
                  direction == "down" || isScreensaver;

    preview = Json::object();
    preview.set("jobId", input["jobId"]);
    preview.set("package", input["package"]);
    preview.set("version", input["version"]);
    preview.set("installedVersion", installedVersion);
    preview.set("direction", Json::text(direction));
    preview.set("apps", apps);
    preview.set("services", services);
    preview.set("rootRequired", Json::flag(rootRequired));
    preview.set("willElevate", Json::flag(reelevate.size() > 0));
    preview.set("reelevate", reelevate);
    preview.set("cpuMismatch", Json::flag(cpuMismatch));
    preview.set("storeInstalled", Json::flag(storeInstalled));
    preview.set("isScreensaver", Json::flag(isScreensaver));
    preview.set("source", input["source"]);
    preview.set("vetted", Json::flag(vetted));
    preview.set("freeBytes", freeBytes);
    preview.set("needBytes", needBytes);
    preview.set("sha256", input["sha256"]);
    return true;
}

} // namespace service
} // namespace nativetv
