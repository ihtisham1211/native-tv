#include "service/piccap.hpp"
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <regex>
#include <string>
#include <unistd.h>
#include <algorithm>
#include <cctype>

namespace nativetv {
namespace service {
namespace {
const char *kPiccapAppPath =
    "/media/developer/apps/usr/palm/applications/org.webosbrew.piccap";

std::string rootedPath(const std::string &root, const std::string &absolutePath) {
    if (root.empty() || root == "/")
        return absolutePath;
    size_t end = root.size();
    while (end > 1 && root[end - 1] == '/')
        --end;
    return root.substr(0, end) + absolutePath;
}

std::string readFile(const std::string &path) {
    std::ifstream file(path.c_str());
    if (!file)
        return std::string();
    return std::string(std::istreambuf_iterator<char>(file),
                       std::istreambuf_iterator<char>());
}

bool webosMajorVersion(const std::string &text, bool starfish, unsigned long &major) {
    static const std::regex issueVersion(
        "webOS(?:\\s+TV)?\\s+([0-9]+)(?:\\.[0-9]+)*", std::regex::icase);
    static const std::regex starfishVersion(
        "release\\s+([0-9]+)(?:\\.[0-9]+)*", std::regex::icase);
    std::smatch match;
    const std::regex &pattern = starfish ? starfishVersion : issueVersion;
    if (!std::regex_search(text, match, pattern))
        return false;
    const std::string majorText = match[1].str();
    char *end = nullptr;
    major = std::strtoul(majorText.c_str(), &end, 10);
    return end && *end == '\0';
}
} // namespace

bool piccapInstalled(const std::string &systemRoot) {
    const std::string path = rootedPath(systemRoot, kPiccapAppPath);
    return ::access(path.c_str(), F_OK) == 0;
}

bool piccapSupportedWebos(const std::string &systemRoot) {
    const std::string root = systemRoot.empty() ? "/" : systemRoot;
    std::string issue = readFile(rootedPath(root, "/etc/issue"));
    if (issue.empty())
        issue = readFile(rootedPath(root, "/etc/issue.net"));
    unsigned long major = 0;
    if (webosMajorVersion(issue, false, major))
        return major >= 3;
    const std::string starfish = readFile(rootedPath(root, "/etc/starfish-release"));
    if (webosMajorVersion(starfish, true, major))
        return major >= 3;
    // Match the legacy gate: if the version cannot be read, allow PicCap.
    return true;
}

bool piccapServiceMissing(const Json &reply, const std::string &raw) {
    std::string text = reply["errorText"].isString() && !reply["errorText"].string().empty()
        ? reply["errorText"].string()
        : reply["error"].isString() && !reply["error"].string().empty() ? reply["error"].string() : raw;
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static const std::regex missing(
        "unknown method|no such service|service.{0,24}(not found|does not exist|not registered)|"
        "method.{0,24}(not found|does not exist)");
    return std::regex_search(text, missing);
}

PiccapStatus::PiccapStatus(std::string systemRoot, Ask ask, std::function<long long()> nowMs)
    : systemRoot_(std::move(systemRoot)), ask_(std::move(ask)), nowMs_(std::move(nowMs)) {}

Json PiccapStatus::read() {
    if (!piccapInstalled(systemRoot_) || !piccapSupportedWebos(systemRoot_))
        return Json();
    // Held across the ask, so readers that arrive meanwhile share its answer.
    std::lock_guard<std::mutex> lock(mutex_);
    const long long now = nowMs_();
    if (!(checkedAt_ && now >= checkedAt_ && now - checkedAt_ < 4000)) {
        checkedAt_ = now;
        Json reply;
        std::string raw;
        const bool answered = ask_(reply, raw);
        if (answered && !(reply["returnValue"].isBoolean() && !reply["returnValue"].boolean()) &&
            reply["isRunning"].isBoolean()) {
            available_ = true;
            running_ = reply["isRunning"].boolean();
        } else if (piccapServiceMissing(answered ? reply : Json(), raw)) {
            available_ = false;
            running_ = false;
        }
    }
    if (!available_)
        return Json();
    Json out = Json::object();
    out.set("running", Json::flag(running_));
    return out;
}

Json piccapConfigMigration(const Json &config) {
    const Json piccap = config["piccap"];
    if (!piccap.isObject() || !piccap.has("enabled")) return Json();
    Json changes = Json::object();
    if (piccap["enabled"].isBoolean() && !piccap["enabled"].boolean()) {
        Json mqtt = Json::object();
        if (config["mqtt"].isObject())
            for (const std::string &key : config["mqtt"].keys()) mqtt.set(key, config["mqtt"][key]);
        Json entities = Json::object();
        if (mqtt["entities"].isObject())
            for (const std::string &key : mqtt["entities"].keys()) entities.set(key, mqtt["entities"][key]);
        Json disabled = Json::array();
        bool listed = false;
        if (entities["disabled"].isArray())
            for (const Json &item : entities["disabled"].items()) {
                if (item.isString() && item.string() == "piccap") listed = true;
                disabled.add(item);
            }
        if (!listed) disabled.add(Json::text("piccap"));
        entities.set("disabled", disabled);
        mqtt.set("entities", entities);
        changes.set("mqtt", mqtt);
    }
    Json rest = Json::object();
    for (const std::string &key : piccap.keys())
        if (key != "enabled") rest.set(key, piccap[key]);
    changes.set("piccap", rest.keys().empty() ? Json() : rest);
    return changes;
}

} // namespace service
} // namespace nativetv
