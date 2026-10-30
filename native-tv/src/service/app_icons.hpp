#pragma once

#include <string>

namespace nativetv {
namespace service {

struct AppIconFile {
    bool found = false;
    std::string bytes;
    std::string etag;
};

// The icon for an app: the themed one in assetsRoot/app-icons/app-icons.pack
// when it has the app's id or title (tools/gen_app_icons.py builds the pack),
// otherwise the app's own declared icon, read without allowing paths or
// symlinks to escape that app's installed directory.
AppIconFile readAppIcon(const std::string &appId, const std::string &systemRoot,
                        const std::string &assetsRoot);

} // namespace service
} // namespace nativetv
