#include "service/app_list.hpp"
#include "service/app_ids.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace nativetv {
namespace service {
namespace {
const char *kBrowserId = "com.webos.app.browser";
const char *kAppBases[] = {
    "/media/system/apps/usr/palm/applications",
    "/usr/palm/applications",
    "/mnt/otncabi/usr/palm/applications",
    "/mnt/otycabi/usr/palm/applications"
};

bool jsTruthy(const Json &value) {
    if (!value.valid()) return false;
    if (value.isBoolean()) return value.boolean();
    if (value.isNumber()) return value.number() != 0;
    if (value.isString()) return !value.string().empty();
    return true;
}

std::string jsString(const Json &value) {
    if (value.isString()) return value.string();
    if (value.isBoolean()) return value.boolean() ? "true" : "false";
    if (value.isNumber()) return compactNumber(value.number(), 12);
    return std::string();
}

std::string rootedPath(const std::string &root, const std::string &absolute) {
    if (root.empty() || root == "/") return absolute;
    std::string prefix = root;
    while (prefix.size() > 1 && prefix.back() == '/') prefix.pop_back();
    return prefix + absolute;
}

std::string dataFile(const std::string &root, const std::string &name) {
    if (root.empty()) return "/var/lib/tvweb/" + name;
    return root[root.size() - 1] == '/' ? root + name : root + "/" + name;
}

std::string findAppInfo(const std::string &appId, const AppListState &state) {
    if (!appIdIsValid(appId)) return std::string();
    for (const char *base : kAppBases) {
        const std::string candidate = rootedPath(state.systemRoot,
            std::string(base) + "/" + appId + "/appinfo.json");
        struct stat info = {};
        if (stat(candidate.c_str(), &info) == 0 && S_ISREG(info.st_mode))
            return candidate;
    }
    return std::string();
}

bool readFile(const std::string &path, std::string &contents, size_t limit) {
    contents.clear();
    int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) return false;
    struct stat info = {};
    if (fstat(descriptor, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0 ||
        static_cast<uint64_t>(info.st_size) > limit) {
        close(descriptor);
        return false;
    }
    char buffer[4096];
    for (;;) {
        const ssize_t count = read(descriptor, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) {
            close(descriptor);
            contents.clear();
            return false;
        }
        if (count == 0) break;
        if (contents.size() + static_cast<size_t>(count) > limit) {
            close(descriptor);
            contents.clear();
            return false;
        }
        contents.append(buffer, static_cast<size_t>(count));
    }
    close(descriptor);
    return true;
}

uint64_t directorySizeAtDepth(const std::string &path, unsigned depth) {
    if (depth > 8) return 0;
    DIR *directory = opendir(path.c_str());
    if (!directory) return 0;
    uint64_t total = 0;
    struct dirent *entry = nullptr;
    while ((entry = readdir(directory)) != nullptr) {
        if (std::string(entry->d_name) == "." || std::string(entry->d_name) == "..")
            continue;
        const std::string child = path + "/" + entry->d_name;
        struct stat info = {};
        if (lstat(child.c_str(), &info) != 0) continue;
        if (S_ISDIR(info.st_mode)) {
            total += directorySizeAtDepth(child, depth + 1);
        } else if (S_ISREG(info.st_mode) && info.st_size > 0) {
            const uint64_t size = static_cast<uint64_t>(info.st_size);
            if (std::numeric_limits<uint64_t>::max() - total < size)
                total = std::numeric_limits<uint64_t>::max();
            else total += size;
        }
    }
    closedir(directory);
    return total;
}

uint64_t directorySize(const std::string &path) {
    using Clock = std::chrono::steady_clock;
    static std::mutex mutex;
    static std::map<std::string, std::pair<Clock::time_point, uint64_t>> cache;
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto found = cache.find(path);
        if (found != cache.end() &&
            Clock::now() - found->second.first < std::chrono::seconds(60))
            return found->second.second;
    }
    const uint64_t size = directorySizeAtDepth(path, 0);
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (cache.size() >= 128) cache.erase(cache.begin());
        cache[path] = {Clock::now(), size};
    }
    return size;
}

std::string iconUrl(const std::string &appId) {
    static const char *safe = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.!~*'()";
    std::ostringstream result;
    result << "/api/apps/icon?id=";
    for (unsigned char character : appId) {
        if (std::strchr(safe, character)) result << static_cast<char>(character);
        else {
            static const char hex[] = "0123456789ABCDEF";
            result << '%' << hex[character >> 4] << hex[character & 15];
        }
    }
    return result.str();
}

struct AppEntry {
    std::string id;
    Json title;
    Json version;
    Json vendor;
    Json removable;
    Json systemApp;
    Json icon;
    Json size;
    std::string folderPath;
};

Json truthyOr(const Json &value, const Json &fallback) {
    return jsTruthy(value) ? value : fallback;
}

bool isPathPrefix(const std::string &path, const char *prefix) {
    return path.compare(0, std::char_traits<char>::length(prefix), prefix) == 0;
}

Json appListCore(const Json &rawApps, const Json &rawLaunchPoints,
                 const AppListState &state, bool writable) {
    std::vector<AppEntry> entries;
    std::map<std::string, size_t> indexes;
    if (rawApps.isArray()) {
        for (const Json &app : rawApps.items()) {
            const std::string id = app["id"].string();
            if (id.empty()) continue;
            auto found = indexes.find(id);
            size_t index;
            if (found == indexes.end()) {
                index = entries.size();
                indexes[id] = index;
                entries.emplace_back();
            } else {
                index = found->second;
            }
            AppEntry item;
            item.id = id;
            item.title = truthyOr(app["title"], Json::text(id));
            item.version = truthyOr(app["version"], Json::text(""));
            item.vendor = truthyOr(app["vendor"], Json::text(""));
            item.folderPath = app["folderPath"].string();
            item.systemApp = Json::flag(jsTruthy(app["systemApp"]));
            item.removable = Json::flag(jsTruthy(app["removable"]));
            item.icon = truthyOr(app["largeIcon"], truthyOr(app["icon"], Json::text("")));
            const Json appSize = app["appsize"];
            const Json installedSize = app["installedSize"];
            item.size = appSize.isNumber() && appSize.number() > 0 ? appSize
                : installedSize.isNumber() && installedSize.number() > 0 ? installedSize : Json();
            entries[index] = std::move(item);
        }
    }

    std::set<std::string> launchPointIds;
    Json savedPages = Json::array();
    Json titles = Json::object();
    std::string titleContents;
    if (readFile(dataFile(state.dataRoot, "saved_page_titles.json"), titleContents,
                 1024 * 1024)) {
        const Json parsed = Json::parse(titleContents);
        if (parsed.isObject()) titles = parsed;
    }
    if (rawLaunchPoints.isArray()) {
        for (const Json &point : rawLaunchPoints.items()) {
            const std::string id = point["id"].string();
            if (id.empty()) continue;
            if (id == kBrowserId && point["lptype"].string() == "bookmark") {
                Json saved = Json::object();
                if (point["launchPointId"].valid())
                    saved.set("launchPointId", point["launchPointId"]);
                saved.set("title", truthyOr(point["title"], Json::text("")));
                saved.set("appId", Json::text(kBrowserId));
                const Json params = point["params"];
                saved.set("address", truthyOr(params["target"],
                    truthyOr(params["url"], Json::text(""))));
                const std::string launchPointId = jsString(point["launchPointId"]);
                saved.set("renamed", Json::flag(!launchPointId.empty() && titles.has(launchPointId)));
                savedPages.add(saved);
                continue;
            }

            launchPointIds.insert(id);
            auto found = indexes.find(id);
            size_t index;
            if (found == indexes.end()) {
                index = entries.size();
                indexes[id] = index;
                entries.emplace_back();
                AppEntry &item = entries.back();
                item.id = id;
                item.title = truthyOr(point["title"], Json::text(id));
                item.version = Json::text("");
                item.vendor = Json::text("");
                item.folderPath.clear();
                item.systemApp = Json::flag(jsTruthy(point["systemApp"]));
                item.removable = Json::flag(!point.has("removable") ||
                                             point["removable"].boolean(true));
                item.icon = truthyOr(point["largeIcon"],
                                     truthyOr(point["icon"], Json::text("")));
                item.size = Json();
                continue;
            }
            index = found->second;
            AppEntry &item = entries[index];
            if (jsTruthy(point["title"])) item.title = point["title"];
            if (jsTruthy(point["icon"]) && !jsTruthy(item.icon)) item.icon = point["icon"];
            if (jsTruthy(point["largeIcon"]) && !jsTruthy(item.icon))
                item.icon = point["largeIcon"];
            if (point.has("removable")) item.removable = point["removable"];
            if (point.has("systemApp")) item.systemApp = point["systemApp"];
            const Json appSize = point["appsize"];
            if (appSize.isNumber() && appSize.number() > 0 && !jsTruthy(item.size))
                item.size = appSize;
        }
    }

    Json installed = Json::array();
    Json systemTiles = Json::array();
    for (const AppEntry &item : entries) {
        if (appIdIsProtected(item.id)) continue;
        const bool isDev = isPathPrefix(item.folderPath, "/media/developer");
        const bool isCrypto = isPathPrefix(item.folderPath, "/media/cryptofs");
        const bool fixedRoot = isPathPrefix(item.folderPath, "/usr/palm") ||
                               isPathPrefix(item.folderPath, "/mnt/otncabi") ||
                               isPathPrefix(item.folderPath, "/mnt/otycabi") ||
                               isPathPrefix(item.folderPath, "/media/system");
        const bool isBuiltIn = fixedRoot || (!isDev && !isCrypto &&
                                               !findAppInfo(item.id, state).empty());
        if (!isBuiltIn && (jsTruthy(item.removable) || isDev || isCrypto)) {
            Json result = Json::object();
            result.set("id", Json::text(item.id));
            result.set("title", item.title);
            result.set("version", item.version);
            result.set("vendor", item.vendor);
            result.set("iconUrl", Json::text(iconUrl(item.id)));
            result.set("removable", Json::flag(true));
            result.set("folderPath", Json::text(item.folderPath));
            Json size = item.size;
            if (!jsTruthy(size) && !item.folderPath.empty()) {
                const uint64_t measured = directorySize(rootedPath(state.systemRoot,
                                                                     item.folderPath));
                if (measured > 0) size = Json::numeric(static_cast<double>(measured));
            }
            result.set("size", size.valid() ? size : Json());
            installed.add(result);
        } else if ((launchPointIds.find(item.id) != launchPointIds.end() ||
                    state.hiddenIds.find(item.id) != state.hiddenIds.end()) &&
                   (jsTruthy(item.systemApp) || isBuiltIn)) {
            Json result = Json::object();
            result.set("id", Json::text(item.id));
            result.set("title", item.title);
            result.set("iconUrl", Json::text(iconUrl(item.id)));
            result.set("hidden", Json::flag(state.hiddenIds.find(item.id) !=
                                               state.hiddenIds.end()));
            result.set("systemApp", Json::flag(true));
            result.set("removable", Json::flag(false));
            systemTiles.add(result);
        }
    }

    for (const std::string &id : state.hiddenIds) {
        if (appIdIsProtected(id) || indexes.find(id) != indexes.end()) continue;
        const std::string appInfoPath = findAppInfo(id, state);
        std::string title = id;
        std::string contents;
        if (!appInfoPath.empty() && readFile(appInfoPath, contents, 1024 * 1024)) {
            const Json info = Json::parse(contents);
            if (jsTruthy(info["title"])) title = jsString(info["title"]);
        }
        Json result = Json::object();
        result.set("id", Json::text(id));
        result.set("title", Json::text(title));
        result.set("iconUrl", Json::text(iconUrl(id)));
        result.set("hidden", Json::flag(true));
        result.set("systemApp", Json::flag(true));
        result.set("removable", Json::flag(false));
        systemTiles.add(result);
    }

    const auto compareTitle = [](const Json &left, const Json &right) {
        std::string a = jsString(left);
        std::string b = jsString(right);
        return a < b;
    };
    auto installedItems = installed.items();
    std::stable_sort(installedItems.begin(), installedItems.end(),
        [&compareTitle](const Json &left, const Json &right) {
            return compareTitle(left["title"], right["title"]);
        });
    auto systemItems = systemTiles.items();
    std::stable_sort(systemItems.begin(), systemItems.end(),
        [&compareTitle](const Json &left, const Json &right) {
            return compareTitle(left["title"], right["title"]);
        });
    installed = Json::array();
    systemTiles = Json::array();
    for (const Json &item : installedItems) installed.add(item);
    for (const Json &item : systemItems) systemTiles.add(item);

    Json result = Json::object();
    result.set("ok", Json::flag(true));
    result.set("installed", installed);
    result.set("systemTiles", systemTiles);
    result.set("savedPages", savedPages);
    result.set("hiddenCount", Json::numeric(static_cast<double>(state.hiddenIds.size())));
    result.set("tileHidingEnabled", Json::flag(state.tileHidingEnabled));
    result.set("writable", Json::flag(writable));
    return result;
}
} // namespace

Json appListResponse(const Json &appsReply, const Json &launchPointsReply,
                    const AppListState &state, bool writable) {
    return appListCore(appsReply["apps"].isArray() ? appsReply["apps"]
                                                     : appsReply["launchPoints"],
                       launchPointsReply["launchPoints"].isArray()
                           ? launchPointsReply["launchPoints"]
                           : launchPointsReply["apps"],
                       state, writable);
}

} // namespace service
} // namespace nativetv
