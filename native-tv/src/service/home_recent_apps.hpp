#pragma once

#include "core/json.hpp"
#include <map>
#include <string>

namespace nativetv {
namespace service {

struct HomeRecentApps {
    bool available = false;
    Json apps;
    std::map<std::string, std::string> imagePaths;
};

struct HomeRecentAppImage {
    bool listed = false;
    bool found = false;
    bool readError = false;
    std::string contentType;
    std::string bytes;
    std::string etag;
};

HomeRecentApps homeRecentApps(const Json &reply);
HomeRecentAppImage readHomeRecentAppImage(const HomeRecentApps &recent,
                                          const std::string &appId);
HomeRecentAppImage readHomeRecentAppImagePath(const std::string &path);

} // namespace service
} // namespace nativetv
