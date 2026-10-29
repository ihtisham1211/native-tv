#pragma once

#include <string>

namespace nativetv {
namespace service {

struct AppIconFile {
    bool found = false;
    std::string bytes;
    std::string etag;
};

// Resolve and read the app's declared icon without allowing paths or symlinks
// to escape that app's installed directory.
AppIconFile readAppIcon(const std::string &appId, const std::string &systemRoot);

} // namespace service
} // namespace nativetv
