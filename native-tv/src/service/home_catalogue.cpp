#include "service/home_catalogue.hpp"
#include "domain/names.hpp"
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace nativetv {
namespace service {
namespace {
const char *kHomeApp = "com.tvweb.home";
const char *kLgHomeApp = "com.webos.app.home";
const char *kHomeTile = "lg-home";
const char *kSidebarApps[] = {"com.webos.app.notificationcenter", "com.webos.app.voice"};
const char *kRows[] = {"app", "dev", "ch"};
const size_t kMaximumRecentBytes = 1024 * 1024;

bool truthy(const Json &value) {
    if (!value.valid()) return false;
    if (value.isBoolean()) return value.boolean();
    if (value.isNumber()) return value.number() != 0;
    if (value.isString()) return !value.string().empty();
    return value.isArray() || value.isObject();
}

std::string jsString(const Json &value) {
    if (!value.valid()) return "";
    if (value.isString()) return value.string();
    if (value.isBoolean()) return value.boolean() ? "true" : "false";
    if (value.isNumber()) return value.dump();
    if (value.isObject()) return "[object Object]";
    std::string result;
    const std::vector<Json> values = value.items();
    for (size_t index = 0; index < values.size(); ++index) {
        if (index) result.push_back(',');
        if (values[index].valid()) result += jsString(values[index]);
    }
    return result;
}

std::string encodeComponent(const std::string &value) {
    static const char *safe =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.!~*'()";
    static const char hex[] = "0123456789ABCDEF";
    std::string result;
    for (unsigned char character : value) {
        if (std::string(safe).find(static_cast<char>(character)) != std::string::npos) {
            result.push_back(static_cast<char>(character));
        } else {
            result.push_back('%');
            result.push_back(hex[character >> 4]);
            result.push_back(hex[character & 15]);
        }
    }
    return result;
}

bool isInputApp(const std::string &id) {
    const std::string prefix = "com.webos.app.";
    if (id.compare(0, prefix.size(), prefix) != 0) return false;
    const std::string tail = id.substr(prefix.size());
    if (tail.compare(0, 6, "livetv") == 0) return true;
    static const char *numbered[] = {"hdmi", "av", "comp"};
    for (const char *candidate : numbered) {
        const std::string start(candidate);
        if (tail.compare(0, start.size(), start) == 0 &&
            tail.size() > start.size() && tail[start.size()] >= '0' &&
            tail[start.size()] <= '9')
            return true;
    }
    return false;
}

std::string biggestIcon(const Json &point) {
    static const char *fields[] = {"extraLargeIcon", "mediumLargeIcon", "largeIcon", "icon"};
    for (const char *field : fields) {
        const std::string value = point[field].string();
        if (value.size() >= 4) {
            std::string suffix = value.substr(value.size() - 4);
            std::transform(suffix.begin(), suffix.end(), suffix.begin(),
                           [](unsigned char character) {
                return static_cast<char>(std::tolower(character));
            });
            if (suffix == ".png") return value;
        }
    }
    const std::string large = point["largeIcon"].string();
    return !large.empty() ? large : point["icon"].string();
}

std::string brandColor(const Json &point) {
    std::string value = point["iconColor"].string();
    if (value.empty()) value = point["bgColor"].string();
    if (value.size() != 7 || value[0] != '#') return "";
    for (size_t index = 1; index < value.size(); ++index) {
        const char character = value[index];
        if (!((character >= '0' && character <= '9') ||
              (character >= 'a' && character <= 'f') ||
              (character >= 'A' && character <= 'F')))
            return "";
    }
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(character >= 'A' && character <= 'F'
                                     ? character - 'A' + 'a'
                                     : character);
    });
    return value;
}

struct CodePoint {
    uint32_t value;
    size_t width;
};

CodePoint decode(const std::string &text, size_t offset) {
    const unsigned char first = static_cast<unsigned char>(text[offset]);
    if (first < 0x80) return {first, 1};
    if ((first & 0xe0) == 0xc0 && offset + 1 < text.size()) {
        const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
        if ((second & 0xc0) == 0x80)
            return {static_cast<uint32_t>(((first & 0x1f) << 6) | (second & 0x3f)), 2};
    }
    if ((first & 0xf0) == 0xe0 && offset + 2 < text.size()) {
        const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
        const unsigned char third = static_cast<unsigned char>(text[offset + 2]);
        if ((second & 0xc0) == 0x80 && (third & 0xc0) == 0x80)
            return {static_cast<uint32_t>(((first & 0x0f) << 12) |
                                          ((second & 0x3f) << 6) | (third & 0x3f)), 3};
    }
    if ((first & 0xf8) == 0xf0 && offset + 3 < text.size()) {
        const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
        const unsigned char third = static_cast<unsigned char>(text[offset + 2]);
        const unsigned char fourth = static_cast<unsigned char>(text[offset + 3]);
        if ((second & 0xc0) == 0x80 && (third & 0xc0) == 0x80 &&
            (fourth & 0xc0) == 0x80)
            return {static_cast<uint32_t>(((first & 0x07) << 18) |
                                          ((second & 0x3f) << 12) |
                                          ((third & 0x3f) << 6) | (fourth & 0x3f)), 4};
    }
    return {first, 1};
}

bool javascriptWhitespace(uint32_t value) {
    return (value >= 0x0009 && value <= 0x000d) || value == 0x0020 ||
           value == 0x00a0 || value == 0x1680 ||
           (value >= 0x2000 && value <= 0x200a) || value == 0x2028 ||
           value == 0x2029 || value == 0x202f || value == 0x205f ||
           value == 0x3000 || value == 0xfeff;
}

std::string trimWhitespace(const std::string &value) {
    size_t first = 0;
    while (first < value.size()) {
        const CodePoint point = decode(value, first);
        if (!javascriptWhitespace(point.value)) break;
        first += point.width;
    }
    size_t lastNonWhitespace = first;
    for (size_t offset = first; offset < value.size();) {
        const CodePoint point = decode(value, offset);
        offset += point.width;
        if (!javascriptWhitespace(point.value)) lastNonWhitespace = offset;
    }
    return value.substr(first, lastNonWhitespace - first);
}

std::string collapseWhitespace(const std::string &value) {
    std::string result;
    bool space = false;
    for (size_t offset = 0; offset < value.size();) {
        const CodePoint point = decode(value, offset);
        if (javascriptWhitespace(point.value)) {
            space = !result.empty();
        } else {
            if (space) result.push_back(' ');
            result.append(value, offset, point.width);
            space = false;
        }
        offset += point.width;
    }
    return result;
}

std::string slug(const std::string &value) {
    std::string result;
    bool separator = false;
    for (unsigned char character : value) {
        if (character >= 'A' && character <= 'Z') character += 'a' - 'A';
        const bool alphaNumeric = (character >= 'a' && character <= 'z') ||
                                  (character >= '0' && character <= '9');
        if (alphaNumeric) {
            if (separator && !result.empty()) result.push_back('-');
            result.push_back(static_cast<char>(character));
            separator = false;
        } else {
            separator = true;
        }
    }
    return result;
}

Json cecAddress(const Json &value) {
    if (!value.isNumber() || value.number() < 0 || value.number() > 0xffff ||
        std::floor(value.number()) != value.number())
        return Json::parse("null");
    const unsigned address = static_cast<unsigned>(value.number());
    return Json::text(std::to_string((address >> 12) & 15) + "." +
                      std::to_string((address >> 8) & 15) + "." +
                      std::to_string((address >> 4) & 15) + "." +
                      std::to_string(address & 15));
}

Json arrayOrEmpty(const Json &value) {
    return value.isArray() ? value : Json::array();
}

bool contains(const std::vector<std::string> &values, const std::string &value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

std::vector<std::string> strings(const Json &value) {
    std::vector<std::string> result;
    for (const Json &item : value.items())
        if (item.isString()) result.push_back(item.string());
    return result;
}

std::vector<std::string> ordered(const Json &saved, const std::vector<std::string> &available) {
    std::vector<std::string> result;
    for (const std::string &value : strings(saved))
        if (contains(available, value) && !contains(result, value)) result.push_back(value);
    for (const std::string &value : available)
        if (!contains(result, value)) result.push_back(value);
    return result;
}

Json jsonStrings(const std::vector<std::string> &values) {
    Json result = Json::array();
    for (const std::string &value : values) result.add(Json::text(value));
    return result;
}

Json recentOrder(const std::string &row, const std::vector<std::string> &ids,
                 const Json &recent) {
    const Json times = recent[row];
    std::vector<std::pair<std::string, double>> ranked;
    for (const std::string &id : ids) {
        if (id == kHomeTile) continue;
        double stamp = times[id].number();
        if (row == "dev") {
            const size_t colon = id.find(':');
            if (colon != std::string::npos)
                stamp = std::max(stamp, times[id.substr(0, colon)].number());
        }
        ranked.emplace_back(id, stamp);
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const auto &left, const auto &right) {
        return left.second > right.second;
    });
    std::vector<std::string> result;
    for (const auto &entry : ranked) result.push_back(entry.first);
    if (contains(ids, kHomeTile)) result.push_back(kHomeTile);
    return jsonStrings(result);
}

bool readBounded(const std::string &path, std::string &contents) {
    const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) return false;
    struct stat info = {};
    bool ok = fstat(descriptor, &info) == 0 && S_ISREG(info.st_mode) &&
              info.st_size >= 0 &&
              static_cast<unsigned long long>(info.st_size) <= kMaximumRecentBytes;
    contents.clear();
    if (ok) {
        char buffer[4096];
        for (;;) {
            const ssize_t count = read(descriptor, buffer, sizeof(buffer));
            if (count < 0 && errno == EINTR) continue;
            if (count < 0 || static_cast<size_t>(count) > kMaximumRecentBytes ||
                contents.size() > kMaximumRecentBytes - static_cast<size_t>(count)) {
                ok = false;
                break;
            }
            if (count == 0) break;
            contents.append(buffer, static_cast<size_t>(count));
        }
    }
    close(descriptor);
    if (!ok) contents.clear();
    return ok;
}
} // namespace

Json homeCatalogueApps(const Json &launchPointsReply, const Json &sidebarInfoReplies) {
    if (!launchPointsReply["launchPoints"].isArray()) return Json();
    Json result = Json::array();
    for (const Json &point : launchPointsReply["launchPoints"].items()) {
        const std::string id = point["id"].string();
        if (id.empty() || point["lptype"].string() == "bookmark" ||
            (point["visible"].isBoolean() && !point["visible"].boolean()) ||
            id == kHomeApp || id == kLgHomeApp || isInputApp(id))
            continue;
        const std::string icon = biggestIcon(point);
        Json app = Json::object();
        app.set("id", Json::text(id));
        app.set("title", Json::text(truthy(point["title"]) ? jsString(point["title"]) : id));
        app.set("icon", Json::parse(icon.empty()
                                        ? "null"
                                        : "\"/api/home/icon?id=" + encodeComponent(id) + "\""));
        const std::string color = brandColor(point);
        app.set("color", color.empty() ? Json::parse("null") : Json::text(color));
        app.set("description", Json::text(jsString(point["appDescription"])));
        result.add(app);
    }

    for (size_t index = 0; index < sizeof(kSidebarApps) / sizeof(kSidebarApps[0]); ++index) {
        const Json reply = sidebarInfoReplies[index];
        const Json info = reply["appInfo"];
        const std::string id = kSidebarApps[index];
        if (!reply["returnValue"].boolean() || info["id"].string() != id) continue;
        const bool hasIcon = !info["icon"].string().empty() &&
                             !info["folderPath"].string().empty();
        Json app = Json::object();
        app.set("id", Json::text(id));
        app.set("title", Json::text(truthy(info["title"]) ? jsString(info["title"]) : id));
        app.set("icon", hasIcon
                            ? Json::text("/api/home/icon?id=" + encodeComponent(id))
                            : Json::parse("null"));
        app.set("color", Json::parse("null"));
        app.set("description", Json::text(""));
        result.add(app);
    }
    return result;
}

Json homeCatalogueDevices(const Json &deviceReply) {
    if (!deviceReply["devices"].isArray()) return Json();
    Json result = Json::array();
    for (const Json &device : deviceReply["devices"].items()) {
        const std::string id = device["id"].string();
        if (id.size() != 6 || id.compare(0, 5, "HDMI_") != 0 ||
            id[5] < '0' || id[5] > '9')
            continue;
        const int port = id[5] - '0';
        const std::string input = "hdmi" + std::to_string(port);
        const bool plugged = device["hdmiPlugIn"].isBoolean()
                                 ? device["hdmiPlugIn"].boolean()
                                 : !(device["connected"].isBoolean() &&
                                     !device["connected"].boolean());
        if (!plugged) {
            Json entry = Json::object();
            entry.set("id", Json::text(input));
            entry.set("name", Json::text("HDMI " + std::to_string(port)));
            entry.set("input", Json::text(input));
            entry.set("port", Json::numeric(port));
            entry.set("via", Json::parse("null"));
            entry.set("address", Json::parse("null"));
            entry.set("unused", Json::flag(true));
            result.add(entry);
            continue;
        }

        std::vector<Json> subs;
        const Json subList = device["subList"];
        for (const Json &sub : subList.items())
            if (sub["id"].string() == "SIMPLINK" && sub["osdName"].isString() &&
                !trimWhitespace(sub["osdName"].string()).empty())
                subs.push_back(sub);
        if (subs.empty()) {
            Json entry = Json::object();
            entry.set("id", Json::text(input));
            entry.set("name", Json::text(truthy(device["label"])
                                             ? jsString(device["label"])
                                             : "HDMI " + std::to_string(port)));
            entry.set("input", Json::text(input));
            entry.set("port", Json::numeric(port));
            entry.set("via", Json::parse("null"));
            entry.set("address", Json::parse("null"));
            result.add(entry);
            continue;
        }
        std::string top;
        for (const Json &sub : subs)
            if (sub["depth"].number() == 1 && top.empty())
                top = trimWhitespace(sub["osdName"].string());
        for (const Json &sub : subs) {
            const std::string name = collapseWhitespace(trimWhitespace(sub["osdName"].string()));
            Json entry = Json::object();
            entry.set("id", Json::text(input + ":" + slug(name)));
            entry.set("name", Json::text(name));
            entry.set("input", Json::text(input));
            entry.set("port", Json::numeric(port));
            entry.set("via", sub["depth"].number() > 1 && !top.empty() && top != name
                                 ? Json::text(top)
                                 : Json::parse("null"));
            entry.set("address", cecAddress(sub["physicalAddress"]));
            result.add(entry);
        }
    }
    return result;
}

Json homeCatalogue(const Json &document, const Json &apps, const Json &devices,
                   const Json &channels, bool channelsRequested,
                   const Json &recent, const Json &historyTotals,
                   bool historyEnabled) {
    std::vector<Json> appItems = apps.items();
    if (historyEnabled) {
        std::stable_sort(appItems.begin(), appItems.end(), [&historyTotals](const Json &left,
                                                                            const Json &right) {
            const double leftUse = historyTotals[names::shortAppId(left["id"].string())].number();
            const double rightUse = historyTotals[names::shortAppId(right["id"].string())].number();
            return leftUse > rightUse;
        });
    }
    Json orderedApps = Json::array();
    std::vector<std::string> appIds;
    for (const Json &app : appItems) {
        orderedApps.add(app);
        appIds.push_back(app["id"].string());
    }
    appIds.push_back(kHomeTile);

    std::vector<std::string> deviceIds;
    for (const Json &device : devices.items()) deviceIds.push_back(device["id"].string());
    std::vector<std::string> pickedIds;
    std::vector<Json> pickedChannels;
    if (channels.isArray()) {
        const Json selected = document["channels"];
        for (const Json &channel : channels.items()) {
            const std::string id = channel["id"].string();
            bool selectedId = false;
            for (const Json &candidate : selected.items())
                if (candidate.string() == id) selectedId = true;
            if (selectedId) {
                pickedIds.push_back(id);
                pickedChannels.push_back(channel);
            }
        }
        const std::vector<std::string> configured = strings(selected);
        std::stable_sort(pickedChannels.begin(), pickedChannels.end(),
                         [&configured](const Json &left, const Json &right) {
            const auto leftAt = std::find(configured.begin(), configured.end(),
                                          left["id"].string());
            const auto rightAt = std::find(configured.begin(), configured.end(),
                                           right["id"].string());
            return leftAt < rightAt;
        });
        pickedIds.clear();
        for (const Json &channel : pickedChannels) pickedIds.push_back(channel["id"].string());
    }

    Json order = Json::object();
    const Json savedOrder = document["order"];
    order.set("app", jsonStrings(ordered(savedOrder["app"], appIds)));
    order.set("dev", jsonStrings(ordered(savedOrder["dev"], deviceIds)));
    order.set("ch", jsonStrings(pickedIds));
    const Json sort = document["sort"];
    for (const char *row : kRows)
        if (sort[row].string() == "recent")
            order.set(row, recentOrder(row, strings(order[row]), recent));

    Json channelItems = Json::array();
    for (const Json &channel : pickedChannels) channelItems.add(channel);
    Json response = Json::object();
    response.set("apps", orderedApps);
    response.set("devices", arrayOrEmpty(devices));
    response.set("channels", channelItems);
    response.set("order", order);
    Json sources = Json::object();
    sources.set("apps", Json::flag(apps.isArray()));
    sources.set("devices", Json::flag(devices.isArray()));
    sources.set("channels", channelsRequested && channels.isArray()
                                 ? Json::numeric(static_cast<double>(channels.size()))
                                 : Json::parse("null"));
    response.set("sources", sources);
    return response;
}

Json readHomeRecent(const std::string &dataRoot) {
    const std::string root = dataRoot.empty() ? "/var/lib/tvweb" : dataRoot;
    const std::string path = root + (root.back() == '/' ? "" : "/") + "home-recent.json";
    std::string contents;
    if (!readBounded(path, contents)) return Json::object();
    const Json parsed = Json::parse(contents);
    return parsed.isObject() ? parsed : Json::object();
}

} // namespace service
} // namespace nativetv
