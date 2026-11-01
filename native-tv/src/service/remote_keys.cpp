#include "service/remote_keys.hpp"
#include "domain/names.hpp"
#include "domain/say.hpp"
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <set>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace nativetv {
namespace service {
namespace {
const char *kFilterDirectory = "/usr/lib/qt5/qml/KeyFilters";
const char *kFilterFilename = "remotekeys-filter.js";
const char *kLiveTv = "com.webos.app.livetv";
const char *kInputSwitcher = "com.webos.app.quickinputpicker";
const char *kMediaPrefix = "media:";
const size_t kMaximumFilterBytes = 1024 * 1024;
const size_t kMaximumFilterSetBytes = 8 * 1024 * 1024;

struct KeyDefinition {
    const char *key;
    const char *symbol;
    const char *title;
    const char *region;
};

const KeyDefinition kKeys[] = {
    {"netflix", "WebOS.Key_webOS_Netflix", "Netflix", ""},
    {"amazon", "WebOS.Key_webOS_Amazon", "Prime Video", ""},
    {"disneyplus", "WebOS.Key_webOS_Disneyplus", "Disney+", ""},
    {"lgchannels", "WebOS.Key_webOS_LGChannels", "LG Channels", ""},
    {"rakutentv", "WebOS.Key_webOS_RakutenTV", "Rakuten TV", ""},
    {"hulu", "WebOS.Key_webOS_Hulu", "Hulu", "us"},
    {"iplayer", "WebOS.Key_webOS_IPlayer", "BBC iPlayer", "gb"},
    {"freeviewplay", "WebOS.Key_webOS_FreeviewPlay", "Freeview Play", "gb"},
    {"slingtv", "WebOS.Key_webOS_SlingTV", "Sling TV", "us"},
    {"alexa", "WebOS.Key_webOS_ALEXA", "Alexa", ""},
    {"googleassistant", "WebOS.Key_webOS_GoogleVoice", "Google Assistant", ""},
    {"apps", "WebOS.Key_webOS_Apps", "Apps", ""},
    {"homehub", "", "Home Hub", ""},
    {"ai", "", "AI", ""},
    {"multikey", "", "? button", ""},
    {"multikeyhold", "", "? button, held", ""},
    {"guide", "WebOS.Key_webOS_TVGuide", "Guide", ""},
    {"input", "WebOS.Key_webOS_InputSource", "Input", ""},
    {"livetv", "WebOS.Key_webOS_InputTV", "TV", ""},
    {"search", "Qt.Key_Search", "Search", ""},
    {"info", "WebOS.Key_webOS_Info", "Info", ""},
    {"help", "Qt.Key_Help", "Help", ""},
    {"red", "WebOS.Key_webOS_Red", "Red", ""},
    {"green", "WebOS.Key_webOS_Green", "Green", ""},
    {"yellow", "WebOS.Key_webOS_Yellow", "Yellow", ""},
    {"blue", "WebOS.Key_webOS_Blue", "Blue", ""}
};

struct Choice {
    std::string id;
    std::string title;
    std::string kind;
};

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

bool readRegularFile(const std::string &path, std::string &contents,
                     size_t limit) {
    contents.clear();
    const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) return false;
    struct stat info = {};
    if (fstat(descriptor, &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_size < 0 || static_cast<uint64_t>(info.st_size) > limit) {
        close(descriptor);
        return false;
    }
    char buffer[8192];
    for (;;) {
        const ssize_t count = read(descriptor, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 || contents.size() + static_cast<size_t>(count) > limit) {
            close(descriptor);
            contents.clear();
            return false;
        }
        if (count == 0) break;
        contents.append(buffer, static_cast<size_t>(count));
    }
    close(descriptor);
    return true;
}

std::string directoryOf(const std::string &path) {
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return ".";
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

bool ensureDirectory(const std::string &path, std::string *error) {
    if (path.empty() || path == "." || path == "/") return true;
    if (mkdir(path.c_str(), 0700) == 0) return true;
    if (errno == EEXIST) {
        struct stat info = {};
        if (stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode)) return true;
        if (error) *error = path + " exists but is not a directory";
        return false;
    }
    if (errno != ENOENT) {
        if (error) *error = "could not create " + path + ": " + std::strerror(errno);
        return false;
    }
    const std::string parent = directoryOf(path);
    if (parent == path || !ensureDirectory(parent, error)) return false;
    if (mkdir(path.c_str(), 0700) == 0 || errno == EEXIST) return true;
    if (error) *error = "could not create " + path + ": " + std::strerror(errno);
    return false;
}

bool writeAtomic(const std::string &path, const std::string &contents,
                 std::string *error) {
    if (!ensureDirectory(directoryOf(path), error)) return false;
    std::string pattern = path + ".tmp.XXXXXX";
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    const int descriptor = mkstemp(name.data());
    if (descriptor < 0) {
        if (error) *error = "could not create a temporary filter file: " +
                            std::string(std::strerror(errno));
        return false;
    }
    const std::string temporary(name.data());
    bool ok = fchmod(descriptor, 0644) == 0;
    if (!ok && error)
        *error = "could not set remote key filter permissions: " +
                 std::string(std::strerror(errno));
    size_t offset = 0;
    while (ok && offset < contents.size()) {
        const ssize_t count = write(descriptor, contents.data() + offset,
                                    contents.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            ok = false;
            if (error) *error = "could not write the remote key filter: " +
                                std::string(std::strerror(errno));
            break;
        }
        offset += static_cast<size_t>(count);
    }
    if (ok && fsync(descriptor) != 0) {
        ok = false;
        if (error) *error = "could not sync the remote key filter: " +
                            std::string(std::strerror(errno));
    }
    const int closeResult = close(descriptor);
    if (closeResult != 0 && ok) {
        ok = false;
        if (error) *error = "could not close the remote key filter: " +
                            std::string(std::strerror(errno));
    }
    if (ok && rename(temporary.c_str(), path.c_str()) != 0) {
        ok = false;
        if (error) *error = "could not install the remote key filter: " +
                            std::string(std::strerror(errno));
    }
    if (ok) {
        const int parent = open(directoryOf(path).c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (parent < 0) {
            ok = false;
            if (error) *error = "could not open remote key filter directory: " +
                                std::string(std::strerror(errno));
        } else {
            if (fsync(parent) != 0) {
                ok = false;
                if (error) *error = "could not sync the remote key filter directory: " +
                                    std::string(std::strerror(errno));
            }
            close(parent);
        }
    }
    if (!ok) unlink(temporary.c_str());
    return ok;
}

bool readFilterSource(const std::string &systemRoot, std::string &source) {
    source.clear();
    const std::string directoryPath = rootedPath(systemRoot, kFilterDirectory);
    DIR *directory = opendir(directoryPath.c_str());
    if (!directory) return false;
    bool ok = true;
    struct dirent *entry = nullptr;
    while ((entry = readdir(directory)) != nullptr) {
        const std::string filename(entry->d_name);
        if (filename.size() < 3 || filename.compare(filename.size() - 3, 3, ".js") != 0)
            continue;
        std::string content;
        if (!readRegularFile(directoryPath + "/" + filename, content,
                             kMaximumFilterBytes) ||
            source.size() + content.size() + 1 > kMaximumFilterSetBytes) {
            ok = false;
            break;
        }
        source += content;
        source.push_back('\n');
    }
    closedir(directory);
    if (!ok) source.clear();
    return ok;
}

std::vector<const KeyDefinition *> keysInSource(const std::string &source) {
    std::vector<const KeyDefinition *> result;
    for (const KeyDefinition &key : kKeys)
        if (key.symbol[0] && source.find(key.symbol) != std::string::npos)
            result.push_back(&key);
    return result;
}

Json readObjectFile(const std::string &path, size_t limit) {
    std::string contents;
    if (!readRegularFile(path, contents, limit))
        return Json::object();
    const Json parsed = Json::parse(contents);
    return parsed.isObject() ? parsed : Json::object();
}

Json readNativeLgRecord(const std::string &dataRoot) {
    return readObjectFile(dataFile(dataRoot, "remotekeys-lg.json"), 256 * 1024);
}

Json nativeMappingEntry(const Json &mapping, const std::string &name) {
    for (const Json &entry : mapping.items())
        if (entry.isObject() && entry.has(name))
            return entry[name];
    return Json();
}

bool nativeKeyOffered(const KeyDefinition &key,
                      const NativeRemoteKeysReadState &state,
                      const Json &lg) {
    if (std::string(key.key) == "guide")
        return state.actions["tvGuide"].isObject();
    if (key.key == std::string("homehub"))
        return state.actions["inputHub"].isObject();
    if (key.key == std::string("multikey"))
        return state.buttonSettings["customShortPress"].isObject();
    if (key.key == std::string("multikeyhold"))
        return state.buttonSettings["customLongPress"].isObject();
    if (key.key == std::string("ai"))
        return state.voiceConductorAvailable ||
               lg["services"]["voiceconductor"].boolean();
    if (key.key == std::string("alexa"))
        return state.alexaViewAvailable || lg["blocks"]["amazon.alexa.view"].boolean();
    const Json original = lg["mapping"][key.key].isObject()
        ? lg["mapping"][key.key] : nativeMappingEntry(state.mappingInfo, key.key);
    return original.isObject() &&
        !(original["isActive"].isBoolean() && !original["isActive"].boolean());
}

Json readMap(const std::string &dataRoot) {
    std::string contents;
    if (!readRegularFile(dataFile(dataRoot, "remotekeys.json"), contents, 64 * 1024))
        return Json::object();
    const Json document = Json::parse(contents);
    return document["keys"].isObject() ? document["keys"] : Json::object();
}

std::string country(const std::string &dataRoot) {
    std::string value;
    if (!readRegularFile(dataFile(dataRoot, "country"), value, 64)) return "";
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.pop_back();
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])))
        ++begin;
    value.erase(0, begin);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool supportedChain(const Json &chain) { return chain.isArray(); }

bool filterInChain(const Json &chain, const std::string &filterPath) {
    for (const Json &entry : chain.items())
        if (entry.isObject() && entry["file"].string() == filterPath)
            return true;
    return false;
}

std::vector<Choice> inputChoices(const Json &devices) {
    std::map<std::string, std::string> labels;
    for (const Json &device : devices.items()) {
        const std::string id = device["id"].string();
        const Json keyValue = names::hdmiKeyForPortId(id);
        if (!keyValue.isString()) continue;
        const std::string key = keyValue.string();
        const std::string label = device["label"].string();
        labels[key] = label.empty() ? names::input(key)["display"].string() : label;
    }
    std::vector<Choice> result;
    for (unsigned number = 1; number <= 4; ++number) {
        const std::string key = "hdmi" + std::to_string(number);
        const auto found = labels.find(key);
        if (found != labels.end())
            result.push_back({"com.webos.app." + key, found->second, "input"});
    }
    result.push_back({kLiveTv, say::msg("srv.remoteKeys.liveTv", "Live TV"), "input"});
    return result;
}

std::vector<Choice> mediaChoices() {
    // Each English beside its say::msg() key, for scripts/check-strings.py.
    static const char *const actions[][3] = {
        {"play", "srv.remoteKeys.play", "Play"},
        {"pause", "srv.remoteKeys.pause", "Pause"},
        {"stop", "srv.remoteKeys.stop", "Stop"},
        {"rewind", "srv.remoteKeys.rewind", "Rewind"},
        {"fastForward", "srv.remoteKeys.fastForward", "Fast forward"},
        {"volumeUp", "srv.remoteKeys.volumeUp", "Volume up"},
        {"volumeDown", "srv.remoteKeys.volumeDown", "Volume down"},
        {"mute", "srv.remoteKeys.mute", "Mute"}
    };
    std::vector<Choice> result;
    for (const auto &action : actions)
        result.push_back({std::string(kMediaPrefix) + action[0],
                          say::msg(action[1], action[2]), "media"});
    return result;
}

std::vector<Choice> appChoices(const Json &apps) {
    std::set<std::string> seen;
    std::vector<Choice> result;
    for (const Json &app : apps.items()) {
        const std::string id = app["id"].string();
        if (id.empty() || (app["visible"].isBoolean() && !app["visible"].boolean()) ||
            id == kLiveTv || id.compare(0, std::string("com.webos.app.hdmi").size(),
                                        "com.webos.app.hdmi") == 0 ||
            id.compare(0, std::string("com.webos.app.container").size(),
                       "com.webos.app.container") == 0 ||
            !seen.insert(id).second)
            continue;
        const std::string title = app["title"].string();
        result.push_back({id, title.empty() ? id : title, "app"});
    }
    std::stable_sort(result.begin(), result.end(), [](const Choice &left, const Choice &right) {
        std::string leftTitle = left.title;
        std::string rightTitle = right.title;
        std::transform(leftTitle.begin(), leftTitle.end(), leftTitle.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        std::transform(rightTitle.begin(), rightTitle.end(), rightTitle.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return leftTitle < rightTitle;
    });
    return result;
}

void appendChoices(Json &output, const std::vector<Choice> &choices,
                   std::map<std::string, std::string> &titles) {
    for (const Choice &choice : choices) {
        Json entry = Json::object();
        entry.set("id", Json::text(choice.id));
        entry.set("title", Json::text(choice.title));
        entry.set("kind", Json::text(choice.kind));
        output.add(entry);
        titles[choice.id] = choice.title;
    }
}

Json unsupported() {
    Json result = Json::object();
    result.set("ok", Json::flag(true));
    result.set("supported", Json::flag(false));
    return result;
}

bool sameJson(const Json &left, const Json &right) {
    if (left.valid() != right.valid()) return false;
    if (left.isObject() != right.isObject() || left.isArray() != right.isArray() ||
        left.isString() != right.isString() || left.isBoolean() != right.isBoolean() ||
        left.isNumber() != right.isNumber())
        return false;
    if (left.isObject()) {
        std::vector<std::string> leftKeys = left.keys();
        std::vector<std::string> rightKeys = right.keys();
        std::sort(leftKeys.begin(), leftKeys.end());
        std::sort(rightKeys.begin(), rightKeys.end());
        if (leftKeys != rightKeys) return false;
        for (const std::string &key : leftKeys)
            if (!sameJson(left[key], right[key])) return false;
        return true;
    }
    if (left.isArray()) {
        if (left.size() != right.size()) return false;
        for (size_t index = 0; index < left.size(); ++index)
            if (!sameJson(left[index], right[index])) return false;
        return true;
    }
    if (left.isString()) return left.string() == right.string();
    if (left.isBoolean()) return left.boolean() == right.boolean();
    if (left.isNumber()) return left.number() == right.number();
    return left.dump() == right.dump();
}

Json cloneJson(const Json &value) {
    return value.valid() ? Json::parse(value.dump()) : Json();
}

Json withoutKey(const Json &object, const std::string &removed) {
    Json result = Json::object();
    for (const std::string &key : object.keys())
        if (key != removed)
            result.set(key, object[key]);
    return result;
}

struct NativeReconcileResult {
    bool write = false;
    Json value;
    Json saved;
    Json written;
};

template <typename Wanted>
NativeReconcileResult reconcileNativeValue(const std::string &name,
                                           const Json &current,
                                           const Json &saved,
                                           const Json &written,
                                           Wanted wanted) {
    const bool had = saved.has(name);
    const bool lgWrote = had && !sameJson(current, written[name]);
    const Json lgValue = had && !lgWrote ? saved[name] : current;
    const Json desired = wanted(lgValue);
    NativeReconcileResult result;
    result.saved = saved;
    result.written = written;
    if (!desired.valid()) {
        result.saved = withoutKey(result.saved, name);
        result.written = withoutKey(result.written, name);
        if (had && !sameJson(lgValue, current)) {
            result.write = true;
            result.value = lgValue;
        }
        return result;
    }
    if (lgWrote && sameJson(current, desired))
        return result;
    result.saved.set(name, lgValue);
    result.written.set(name, desired);
    if (!sameJson(current, desired)) {
        result.write = true;
        result.value = desired;
    }
    return result;
}

bool isNativeMappingKey(const std::string &name) {
    if (name == "alexa" || name == "homehub" || name == "ai" ||
        name == "multikey" || name == "multikeyhold" || name == "guide")
        return false;
    for (const KeyDefinition &key : kKeys)
        if (name == key.key) return key.symbol[0] != '\0';
    return false;
}

Json mappingWanted(const Json &map, const std::string &key,
                   const Json &lgValue) {
    if (!map.has(key) || !map[key].isString()) return Json();
    if (map[key].string().empty()) {
        if (!lgValue.isObject()) return Json();
        Json disabled = Json::object();
        for (const std::string &field : lgValue.keys())
            disabled.set(field, lgValue[field]);
        disabled.set("isActive", Json::flag(false));
        return disabled;
    }
    Json enabled = Json::object();
    enabled.set("launch_param", Json());
    enabled.set("app_id", map[key]);
    enabled.set("isActive", Json::flag(true));
    return enabled;
}

Json actionWanted(const Json &map, const std::string &key) {
    if (!map.has(key) || !map[key].isString()) return Json();
    Json result = Json::object();
    if (map[key].string().empty()) {
        result.set("action", Json::text("ignore"));
        result.set("accept", Json::flag(true));
    } else {
        result.set("action", Json::text("launch"));
        result.set("id", map[key]);
    }
    return result;
}

Json buttonSettingWanted(const Json &map, const std::string &key) {
    if (!map.has(key) || !map[key].isString() || map[key].string().empty())
        return Json();
    Json result = Json::object();
    result.set("id", map[key]);
    result.set("type", Json::text("app"));
    result.set("params", Json::object());
    return result;
}

void reconcileRecordValue(Json &saved, Json &written,
                          const NativeReconcileResult &value) {
    saved = value.saved;
    written = value.written;
    if (!saved.isObject()) saved = Json::object();
    if (!written.isObject()) written = Json::object();
}

Json recordCategory(Json &record, const std::string &name) {
    Json category = record[name];
    if (!category.isObject()) category = Json::object();
    record.set(name, category);
    return category;
}
} // namespace

bool hasJavaScriptRemoteKeys(const std::string &systemRoot) {
    std::string source;
    return readFilterSource(systemRoot, source) && !keysInSource(source).empty();
}

bool installRemoteKeysFilterAsset(const std::string &assetPath,
                                  const std::string &installedPath,
                                  bool &changed, std::string *error) {
    changed = false;
    std::string source;
    if (!readRegularFile(assetPath, source, 64 * 1024) || source.empty()) {
        if (error) *error = "bundled remote key filter is missing or invalid";
        return false;
    }
    std::string installed;
    if (readRegularFile(installedPath, installed, 64 * 1024) && installed == source)
        return true;
    if (!writeAtomic(installedPath, source, error))
        return false;
    changed = true;
    return true;
}

Json remoteKeysReadResponse(const RemoteKeysReadState &state) {
    std::string source;
    if (!readFilterSource(state.systemRoot, source) || !supportedChain(state.filterChain))
        return unsupported();
    const std::vector<const KeyDefinition *> keys = keysInSource(source);
    if (keys.empty()) return unsupported();

    Json appList = state.appsReply["apps"];
    if (!appList.isArray()) appList = Json::array();
    const std::vector<Choice> inputs = inputChoices(state.inputDevices);
    const std::vector<Choice> media = mediaChoices();
    const std::vector<Choice> apps = appChoices(appList);
    std::vector<Choice> switchers;
    if (state.inputSwitcherAvailable)
        switchers.push_back({kInputSwitcher,
            say::msg("srv.remoteKeys.inputSwitcher", "Input switcher"), "switcher"});

    std::map<std::string, std::string> titles;
    for (const Json &app : appList.items()) {
        const std::string id = app["id"].string();
        if (id.empty()) continue;
        const std::string title = app["title"].string();
        titles[id] = title.empty() ? id : title;
    }

    Json choices = Json::array();
    appendChoices(choices, switchers, titles);
    appendChoices(choices, media, titles);
    appendChoices(choices, inputs, titles);
    appendChoices(choices, apps, titles);

    const Json map = readMap(state.dataRoot);
    const std::string region = country(state.dataRoot);
    Json keyRows = Json::array();
    for (const KeyDefinition *definition : keys) {
        const bool configured = map.has(definition->key) && map[definition->key].isString();
        const std::string id = configured ? map[definition->key].string() : "";
        Json row = Json::object();
        row.set("key", Json::text(definition->key));
        row.set("title", Json::text(definition->title));
        row.set("action", Json::text(!configured ? "lg" : (id.empty() ? "nothing" : "open")));
        row.set("id", Json::text(id));
        const auto title = titles.find(id);
        row.set("opens", Json::text(id.empty() ? "" : (title == titles.end() ? id : title->second)));
        row.set("elsewhere", Json::flag(!region.empty() && definition->region[0] != '\0' &&
                                        region != definition->region));
        keyRows.add(row);
    }

    const std::string filterPath = dataFile(state.dataRoot, kFilterFilename);
    Json result = Json::object();
    result.set("ok", Json::flag(true));
    result.set("supported", Json::flag(true));
    result.set("keys", keyRows);
    result.set("choices", choices);
    result.set("inChain", Json::flag(filterInChain(state.filterChain, filterPath)));
    return result;
}

Json nativeRemoteKeysReadResponse(const NativeRemoteKeysReadState &state) {
    if (!state.filterChain.isArray() || state.filterChain.size() == 0 ||
        !state.mappingInfo.isArray() || !state.actions.isObject() ||
        !state.buttonSettings.isObject())
        return unsupported();

    Json appList = state.appsReply["apps"];
    if (!appList.isArray()) appList = Json::array();
    const std::vector<Choice> inputs = inputChoices(state.inputDevices);
    const std::vector<Choice> apps = appChoices(appList);
    std::vector<Choice> switchers;
    if (state.inputSwitcherAvailable)
        switchers.push_back({kInputSwitcher,
            say::msg("srv.remoteKeys.inputSwitcher", "Input switcher"), "switcher"});

    std::map<std::string, std::string> titles;
    for (const Json &app : appList.items()) {
        const std::string id = app["id"].string();
        if (id.empty()) continue;
        const std::string title = app["title"].string();
        titles[id] = title.empty() ? id : title;
    }

    Json choices = Json::array();
    appendChoices(choices, switchers, titles);
    appendChoices(choices, inputs, titles);
    appendChoices(choices, apps, titles);

    const Json map = readMap(state.dataRoot);
    const Json lg = readNativeLgRecord(state.dataRoot);
    const std::string region = country(state.dataRoot);
    Json keyRows = Json::array();
    for (const KeyDefinition &definition : kKeys) {
        if (!nativeKeyOffered(definition, state, lg))
            continue;
        const bool configured = map.has(definition.key) &&
                                map[definition.key].isString();
        const std::string id = configured ? map[definition.key].string() : "";
        Json row = Json::object();
        row.set("key", Json::text(definition.key));
        row.set("title", Json::text(definition.title));
        row.set("action", Json::text(!configured ? "lg" :
                                      (id.empty() ? "nothing" : "open")));
        row.set("id", Json::text(id));
        const auto title = titles.find(id);
        row.set("opens", Json::text(id.empty() ? "" :
                                     (title == titles.end() ? id : title->second)));
        row.set("elsewhere", Json::flag(!region.empty() &&
            definition.region[0] != '\0' && region != definition.region));
        if (std::string(definition.key) == "multikey" ||
            std::string(definition.key) == "multikeyhold")
            row.set("noNothing", Json::flag(true));
        if (std::string(definition.key) == "ai")
            row.set("turnsOffVoice", Json::flag(true));
        if (std::string(definition.key) == "alexa")
            row.set("blocksApp", Json::flag(true));
        keyRows.add(row);
    }

    Json result = Json::object();
    result.set("ok", Json::flag(true));
    result.set("supported", Json::flag(true));
    result.set("keys", keyRows);
    result.set("choices", choices);
    result.set("inChain", Json::flag(false));
    return result;
}

NativeRemoteKeysWriteResult nativeRemoteKeysWriteRequests(
    const Json &map, const NativeRemoteKeysWriteState &state) {
    NativeRemoteKeysWriteResult result;
    result.mappingInfo = state.mappingInfo.isArray()
        ? state.mappingInfo : Json::array();
    result.actionValues = Json::object();
    result.buttonSettings = Json::object();
    result.lgRecord = state.lgRecord.isObject()
        ? cloneJson(state.lgRecord) : Json::object();

    Json savedMapping = recordCategory(result.lgRecord, "mapping");
    Json writtenMapping = recordCategory(result.lgRecord, "mappingWritten");
    Json nextMapping = Json::array();
    for (const Json &entry : result.mappingInfo.items()) {
        if (!entry.isObject()) {
            nextMapping.add(entry);
            continue;
        }
        Json nextEntry = Json::object();
        for (const std::string &key : entry.keys()) {
            Json current = entry[key];
            if (!isNativeMappingKey(key)) {
                nextEntry.set(key, current);
                continue;
            }
            const NativeReconcileResult reconciled = reconcileNativeValue(
                key, current, savedMapping, writtenMapping,
                [&map, &key](const Json &lgValue) {
                    return mappingWanted(map, key, lgValue);
                });
            reconcileRecordValue(savedMapping, writtenMapping, reconciled);
            nextEntry.set(key, reconciled.write ? reconciled.value : current);
            result.mappingChanged = result.mappingChanged || reconciled.write;
        }
        nextMapping.add(nextEntry);
    }
    result.mappingInfo = nextMapping;
    result.lgRecord.set("mapping", savedMapping);
    result.lgRecord.set("mappingWritten", writtenMapping);

    Json savedActions = recordCategory(result.lgRecord, "actions");
    Json writtenActions = recordCategory(result.lgRecord, "actionsWritten");
    const char *actionKeys[][2] = {
        {"guide", "tvGuide"}, {"homehub", "inputHub"}
    };
    for (const auto &names : actionKeys) {
        const std::string key = names[0];
        const std::string name = names[1];
        const Json current = state.actions[name];
        if (!current.isObject()) continue;
        const NativeReconcileResult reconciled = reconcileNativeValue(
            name, current, savedActions, writtenActions,
            [&map, &key](const Json &) { return actionWanted(map, key); });
        reconcileRecordValue(savedActions, writtenActions, reconciled);
        if (reconciled.write)
            result.actionValues.set("com.webos.keyaction." + name,
                                    reconciled.value);
    }
    result.lgRecord.set("actions", savedActions);
    result.lgRecord.set("actionsWritten", writtenActions);

    Json savedSettings = recordCategory(result.lgRecord, "settings");
    Json writtenSettings = recordCategory(result.lgRecord, "settingsWritten");
    const char *settingKeys[][2] = {
        {"multikey", "customShortPress"},
        {"multikeyhold", "customLongPress"}
    };
    for (const auto &names : settingKeys) {
        const std::string key = names[0];
        const std::string name = names[1];
        const Json current = state.buttonSettings[name];
        if (!current.isObject()) continue;
        const NativeReconcileResult reconciled = reconcileNativeValue(
            name, current, savedSettings, writtenSettings,
            [&map, &key](const Json &) { return buttonSettingWanted(map, key); });
        reconcileRecordValue(savedSettings, writtenSettings, reconciled);
        if (reconciled.write)
            result.buttonSettings.set(name, reconciled.value);
    }
    result.lgRecord.set("settings", savedSettings);
    result.lgRecord.set("settingsWritten", writtenSettings);
    return result;
}

bool nativeRemoteKeysLgRecordHasValues(const Json &record) {
    if (!record.isObject()) return false;
    static const char *categories[] = {
        "mapping", "actions", "settings", "services", "blocks"
    };
    for (const char *category : categories)
        if (record[category].isObject() && !record[category].keys().empty())
            return true;
    return false;
}

} // namespace service
} // namespace nativetv
