#include "service/app_package_policy.hpp"

#include "service/app_ids.hpp"

#include <sys/stat.h>

namespace nativetv {
namespace service {
namespace {

bool selfUpdating(const std::string &id) {
    static const char *const packageIds[] = {
        "org.webosbrew.hbchannel",
        "io.github.ihtisham1211.nativetv"
    };
    for (const char *packageId : packageIds) {
        const std::string prefix(packageId);
        if (id == prefix || (id.size() > prefix.size() &&
            id.compare(0, prefix.size(), prefix) == 0 && id[prefix.size()] == '.'))
            return true;
    }
    return false;
}

bool systemPrefix(const std::string &id) {
    static const char *const prefixes[] = {"com.webos.", "com.palm.", "com.lge."};
    for (const char *prefix : prefixes) {
        const std::string value(prefix);
        if (id.compare(0, value.size(), value) == 0)
            return true;
    }
    return false;
}

bool existsIn(const std::string &directory, const std::string &id) {
    const std::string path = directory + "/" + id;
    struct stat info = {};
    return stat(path.c_str(), &info) == 0;
}

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

bool fail(AppPackagePolicyFailure &failure, const std::string &code,
          const std::string &id, const std::string &package) {
    failure.code = code;
    failure.id = id;
    failure.package = package;
    return false;
}

} // namespace

const std::vector<std::string> &AppPackagePolicy::systemAppDirectories() {
    static const std::vector<std::string> directories = {
        "/media/system/apps/usr/palm/applications",
        "/usr/palm/applications",
        "/mnt/otncabi/usr/palm/applications",
        "/mnt/otycabi/usr/palm/applications"
    };
    return directories;
}

bool AppPackagePolicy::validate(const Json &packageInfo,
                                const std::vector<std::string> &systemDirectories,
                                AppPackagePolicyFailure &failure) {
    failure = AppPackagePolicyFailure();
    if (!packageInfo.isObject() || !packageInfo["package"].isString() ||
        !packageInfo["apps"].isArray() || !packageInfo["services"].isArray())
        return fail(failure, "invalid-metadata", std::string(), std::string());

    const std::string package = packageInfo["package"].string();
    if (!appPackageIdIsSafe(package))
        return fail(failure, "invalid-metadata", package, package);

    std::vector<std::string> ids;
    ids.push_back(package);
    for (const Json &app : packageInfo["apps"].items()) {
        if (!app.isObject() || !app["id"].isString())
            return fail(failure, "invalid-metadata", std::string(), package);
        ids.push_back(app["id"].string());
    }
    for (const Json &service : packageInfo["services"].items()) {
        if (!service.isString())
            return fail(failure, "invalid-metadata", std::string(), package);
        ids.push_back(service.string());
    }

    for (const std::string &id : ids) {
        if (!appPackageIdIsSafe(id))
            return fail(failure, "invalid-metadata", id, package);
        if (id != package && id.compare(0, package.size() + 1, package + ".") != 0)
            return fail(failure, "id-prefix", id, package);
        if (selfUpdating(id))
            return fail(failure, "self-updating", id, package);
        if (systemPrefix(id) || appIdIsProtected(id))
            return fail(failure, "protected", id, package);
        for (const std::string &directory : systemDirectories) {
            if (existsIn(directory, id))
                return fail(failure, "system-existing", id, package);
        }
    }
    return true;
}

bool AppPackagePolicy::validateCatalogSelection(const Json &packageInfo,
                                                 const std::string &expectedId,
                                                 const std::string &expectedSha256,
                                                 AppPackagePolicyFailure &failure) {
    failure = AppPackagePolicyFailure();
    if (!packageInfo.isObject() || !packageInfo["package"].isString() ||
        !packageInfo["apps"].isArray() || !packageInfo["sha256"].isString())
        return fail(failure, "invalid-metadata", std::string(), std::string());

    const std::string actualSha256 = packageInfo["sha256"].string();
    if (!expectedSha256.empty()) {
        if (!isSha256(expectedSha256) || !isSha256(actualSha256))
            return fail(failure, "invalid-selection", std::string(),
                        packageInfo["package"].string());
        if (lowercase(expectedSha256) != lowercase(actualSha256)) {
            fail(failure, "hash-mismatch", std::string(),
                 packageInfo["package"].string());
            failure.expected = expectedSha256;
            failure.actual = actualSha256;
            return false;
        }
    }

    if (!expectedId.empty()) {
        if (!appPackageIdIsSafe(expectedId))
            return fail(failure, "invalid-selection", expectedId,
                        packageInfo["package"].string());
        bool found = packageInfo["package"].string() == expectedId;
        for (const Json &app : packageInfo["apps"].items())
            if (app["id"].string() == expectedId) found = true;
        if (!found) {
            fail(failure, "catalog-id-mismatch", expectedId,
                 packageInfo["package"].string());
            failure.expected = expectedId;
            failure.actual = packageInfo["package"].string();
            return false;
        }
    }
    return true;
}

} // namespace service
} // namespace nativetv
