#pragma once

#include "core/json.hpp"
#include <string>

namespace nativetv {
namespace service {

// Inspects a staged .ipk without extracting it. fileName is restricted to a
// basename so callers cannot make the daemon read outside the staging folder.
class AppPackageInspector {
  public:
    static bool inspect(const std::string &stagingDirectory,
                        const std::string &fileName, Json &result,
                        std::string &error);
};

} // namespace service
} // namespace nativetv
