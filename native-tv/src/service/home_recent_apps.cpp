#include "service/home_recent_apps.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace nativetv {
namespace service {
namespace {
const char *kLeftOut[] = {"com.webos.app.home", "com.tvweb.home", "com.tvweb.native",
                          "com.tvweb.splash"};
const size_t kMaximumImageBytes = 16 * 1024 * 1024;
const size_t kMaximumImagePathBytes = 4096;

bool excluded(const std::string &appId) {
    for (const char *id : kLeftOut)
        if (appId == id) return true;
    return false;
}

std::string recentImagePath(const Json &app) {
    static const char *fields[] = {
        "customImageFilePath", "cardSnapShotFilePath", "splashBackground"};
    for (const char *field : fields) {
        const Json candidate = app[field];
        if (!candidate.isString()) continue;
        const std::string path = candidate.string();
        if (path.empty() || path[0] != '/' || path.size() > kMaximumImagePathBytes ||
            path.find('\0') != std::string::npos)
            continue;
        std::string suffix = path.substr(path.find_last_of('.') == std::string::npos
                                            ? path.size()
                                            : path.find_last_of('.'));
        std::transform(suffix.begin(), suffix.end(), suffix.begin(),
                       [](unsigned char value) {
            return static_cast<char>(value >= 'A' && value <= 'Z'
                                         ? value - 'A' + 'a' : value);
        });
        if (suffix == ".jpg" || suffix == ".jpeg" || suffix == ".png")
            return path;
    }
    return std::string();
}

bool validListReply(const Json &reply) {
    return reply.isObject() &&
           !(reply["returnValue"].isBoolean() && !reply["returnValue"].boolean()) &&
           reply["recentsAppList"].isArray();
}

std::string javascriptString(const Json &value) {
    if (!value.valid()) return "";
    if (value.isString()) return value.string();
    if (value.isBoolean()) return value.boolean() ? "true" : "false";
    if (value.isNumber()) return value.dump();
    if (value.isObject()) return "[object Object]";
    std::string result;
    const std::vector<Json> items = value.items();
    for (size_t index = 0; index < items.size(); ++index) {
        if (index) result.push_back(',');
        if (items[index].valid()) result += javascriptString(items[index]);
    }
    return result;
}

bool javascriptTruthy(const Json &value) {
    if (!value.valid()) return false;
    if (value.isBoolean()) return value.boolean();
    if (value.isNumber()) return value.number() != 0;
    if (value.isString()) return !value.string().empty();
    return true;
}

int64_t modifiedMilliseconds(const struct stat &info) {
#if defined(__APPLE__)
    return static_cast<int64_t>(info.st_mtimespec.tv_sec) * 1000 +
           info.st_mtimespec.tv_nsec / 1000000;
#else
    return static_cast<int64_t>(info.st_mtim.tv_sec) * 1000 +
           info.st_mtim.tv_nsec / 1000000;
#endif
}

std::string makeEtag(size_t size, const struct stat &info) {
    std::ostringstream etag;
    etag << '"' << std::hex << size << '-' << modifiedMilliseconds(info) << '"';
    return etag.str();
}
} // namespace

HomeRecentApps homeRecentApps(const Json &reply) {
    HomeRecentApps result;
    if (!validListReply(reply)) return result;
    result.available = true;
    result.apps = Json::array();
    std::vector<std::string> seen;
    for (const Json &app : reply["recentsAppList"].items()) {
        const std::string id = app["appId"].isString() ? app["appId"].string() : "";
        if (id.empty() || excluded(id) ||
            std::find(seen.begin(), seen.end(), id) != seen.end())
            continue;
        seen.push_back(id);
        const Json title = javascriptTruthy(app["title"]) ? app["title"] : Json::text(id);
        Json item = Json::object();
        item.set("id", Json::text(id));
        item.set("title", Json::text(javascriptString(title)));
        const std::string path = recentImagePath(app);
        item.set("image", Json::flag(!path.empty()));
        if (!path.empty()) result.imagePaths[id] = path;
        result.apps.add(item);
    }
    return result;
}

HomeRecentAppImage readHomeRecentAppImagePath(const std::string &path) {
    HomeRecentAppImage result;
    if (path.empty() || path[0] != '/' || path.size() > kMaximumImagePathBytes ||
        path.find('\0') != std::string::npos)
        return result;
    const size_t extension = path.find_last_of('.');
    if (extension == std::string::npos) return result;
    std::string suffix = path.substr(extension);
    std::transform(suffix.begin(), suffix.end(), suffix.begin(),
                   [](unsigned char value) {
        return static_cast<char>(value >= 'A' && value <= 'Z'
                                     ? value - 'A' + 'a' : value);
    });
    if (suffix != ".jpg" && suffix != ".jpeg" && suffix != ".png")
        return result;
    const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) return result;
    struct stat info = {};
    bool ok = fstat(descriptor, &info) == 0 && S_ISREG(info.st_mode) &&
              info.st_size >= 0 &&
              static_cast<unsigned long long>(info.st_size) <= kMaximumImageBytes;
    if (!ok) {
        result.readError = true;
        close(descriptor);
        return result;
    }
    result.bytes.reserve(static_cast<size_t>(info.st_size));
    char buffer[16384];
    for (;;) {
        const ssize_t count = read(descriptor, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 || static_cast<size_t>(count) > kMaximumImageBytes ||
            result.bytes.size() > kMaximumImageBytes - static_cast<size_t>(count)) {
            ok = false;
            break;
        }
        if (count == 0) break;
        result.bytes.append(buffer, static_cast<size_t>(count));
    }
    close(descriptor);
    if (!ok) {
        result.bytes.clear();
        result.readError = true;
        return result;
    }
    result.contentType = suffix == ".png" ? "image/png" : "image/jpeg";
    result.etag = makeEtag(result.bytes.size(), info);
    result.found = true;
    return result;
}

HomeRecentAppImage readHomeRecentAppImage(const HomeRecentApps &recent,
                                          const std::string &appId) {
    HomeRecentAppImage result;
    const auto path = recent.imagePaths.find(appId);
    if (path == recent.imagePaths.end()) return result;
    result.listed = true;
    HomeRecentAppImage image = readHomeRecentAppImagePath(path->second);
    image.listed = true;
    return image;
}

} // namespace service
} // namespace nativetv
