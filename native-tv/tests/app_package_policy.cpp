#include "service/app_package_policy.hpp"
#include "service/app_ids.hpp"

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace nativetv;
using namespace nativetv::service;

namespace {
Json packageInfo(const std::string &package, const std::vector<std::string> &apps,
                 const std::vector<std::string> &services) {
    Json result = Json::object();
    result.set("package", Json::text(package));
    Json appList = Json::array();
    for (const std::string &id : apps) {
        Json app = Json::object();
        app.set("id", Json::text(id));
        appList.add(app);
    }
    result.set("apps", appList);
    Json serviceList = Json::array();
    for (const std::string &id : services)
        serviceList.add(Json::text(id));
    result.set("services", serviceList);
    return result;
}

void expectFailure(const Json &info, const std::string &expectedCode,
                   const std::vector<std::string> &directories = {}) {
    AppPackagePolicyFailure failure;
    assert(!AppPackagePolicy::validate(info, directories, failure));
    assert(failure.code == expectedCode);
}
} // namespace

int main() {
    AppPackagePolicyFailure failure;
    const std::vector<std::string> noSystemApps;
    assert(appPackageIdIsSafe("com.example.app+plus"));
    assert(!appPackageIdIsSafe("../outside"));
    assert(!appPackageIdIsSafe(".hidden"));
    assert(!appPackageIdIsSafe(std::string(129, 'a')));
    const Json valid = packageInfo("com.example.app",
        {"com.example.app", "com.example.app.more"},
        {"com.example.app.service"});
    assert(AppPackagePolicy::validate(valid, noSystemApps, failure));

    Json selected = valid;
    selected.set("sha256", Json::text(std::string(64, 'a')));
    assert(AppPackagePolicy::validateCatalogSelection(
        selected, "com.example.app.more", std::string(64, 'A'), failure));
    assert(AppPackagePolicy::validateCatalogSelection(
        selected, "com.example.app", std::string(), failure));
    assert(!AppPackagePolicy::validateCatalogSelection(
        selected, "com.other.app", std::string(), failure));
    assert(failure.code == "catalog-id-mismatch" &&
           failure.expected == "com.other.app" &&
           failure.actual == "com.example.app");
    assert(!AppPackagePolicy::validateCatalogSelection(
        selected, std::string(), std::string(64, 'b'), failure));
    assert(failure.code == "hash-mismatch" &&
           failure.expected == std::string(64, 'b') &&
           failure.actual == std::string(64, 'a'));

    expectFailure(packageInfo("com.example.app", {"com.other.app"}, {}), "id-prefix");
    expectFailure(packageInfo("org.webosbrew.hbchannel", {}, {}), "self-updating");
    expectFailure(packageInfo("com.webos.example", {}, {}), "protected");
    expectFailure(packageInfo("com.example.app", {"com.example.app.service"},
                              {"org.foreign.service"}), "id-prefix");

    char directoryTemplate[] = "/tmp/nativetv-app-policy-XXXXXX";
    char *directoryValue = mkdtemp(directoryTemplate);
    assert(directoryValue);
    const std::string directory(directoryValue);
    const std::string existingId = "com.example.app.already-installed";
    assert(mkdir((directory + "/" + existingId).c_str(), 0700) == 0);
    expectFailure(packageInfo("com.example.app", {existingId}, {}),
                  "system-existing", {directory});
    assert(rmdir((directory + "/" + existingId).c_str()) == 0);
    assert(rmdir(directory.c_str()) == 0);

    Json malformed = packageInfo("com.example.app", {}, {});
    malformed.set("apps", Json::text("not-an-array"));
    expectFailure(malformed, "invalid-metadata");

    std::cout << "native app package policy assertions passed\n";
    return 0;
}
