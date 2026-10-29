#pragma once

#include "core/json.hpp"
#include <string>

namespace nativetv {
namespace service {

class AppInstallPreview {
  public:
    // Builds the public preview projection and one-click review decision from
    // verified package and TV state. It selects the installed version from
    // the app snapshot and computes capacity from the trusted free-space value.
    static bool build(const Json &input, Json &preview, bool &needsReview,
                      bool &capacityOkay, std::string &error);
};

} // namespace service
} // namespace nativetv
