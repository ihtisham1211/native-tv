#pragma once

#include "core/json.hpp"
#include <string>
#include <vector>

namespace nativetv {
namespace service {

struct AppPackagePolicyFailure {
    std::string code;
    std::string id;
    std::string package;
    std::string expected;
    std::string actual;
};

class AppPackagePolicy {
  public:
    // allowNativeTv lets Native Tv's own package through, for its release update.
    static bool validate(const Json &packageInfo,
                         const std::vector<std::string> &systemAppDirectories,
                         AppPackagePolicyFailure &failure, bool allowNativeTv = false);
    static bool validateCatalogSelection(const Json &packageInfo,
                                         const std::string &expectedId,
                                         const std::string &expectedSha256,
                                         AppPackagePolicyFailure &failure);
    static const std::vector<std::string> &systemAppDirectories();
};

} // namespace service
} // namespace nativetv
