#include "service/preload.hpp"
#include <algorithm>
#include <cctype>
#include <utility>
#include <vector>

namespace nativetv {
namespace service {
namespace {
const std::string kConfigKey = "com.webos.service.preloadmanager.supportAppList";

bool containsId(const Json &array, const std::string &id) {
    if (!array.isArray())
        return false;
    const std::vector<Json> items = array.items();
    for (const Json &item : items)
        if (item.isString() && item.string() == id)
            return true;
    return false;
}

Json copyEntries(const Json &array) {
    Json copy = Json::array();
    if (!array.isArray())
        return copy;
    for (const Json &entry : array.items())
        copy.add(entry);
    return copy;
}

Json copyObject(const Json &object) {
    Json copy = Json::object();
    if (!object.isObject())
        return copy;
    for (const std::string &key : object.keys())
        copy.set(key, object[key]);
    return copy;
}

std::string lower(const std::string &value) {
    std::string result = value;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return result;
}

std::string appTitle(const Json &app, const std::string &id) {
    const std::string title = app["title"].string();
    return title.empty() ? id : title;
}

} // namespace

const std::string &preloadConfigKey() {
    return kConfigKey;
}

bool preloadEssentialApp(const std::string &id) {
    if (id == "airplay")
        return true;
    const std::string prefix = "com.webos.app.";
    if (id.compare(0, prefix.size(), prefix) != 0)
        return false;
    const std::string name = id.substr(prefix.size());
    if (name == "home" || name == "livetv" || name == "livemenu" ||
        name == "miracast" || name == "miracast-overlay" || name == "voice" ||
        name == "container" || name == "inputcommon")
        return true;
    if (name.compare(0, 4, "hdmi") == 0) {
        for (size_t i = 4; i < name.size(); ++i)
            if (!std::isdigit(static_cast<unsigned char>(name[i])))
                return false;
        return true;
    }
    return false;
}

bool javascriptTruthy(const Json &value) {
    if (!value.valid())
        return false;
    if (value.isBoolean())
        return value.boolean();
    if (value.isNumber())
        return value.number() != 0;
    if (value.isString())
        return !value.string().empty();
    return true;
}

std::string javascriptString(const Json &value) {
    if (!value.valid())
        return "undefined";
    if (value.isString())
        return value.string();
    if (value.isBoolean())
        return value.boolean() ? "true" : "false";
    if (value.isNumber()) {
        std::string number = value.dump();
        if (number.size() > 2 && number.compare(number.size() - 2, 2, ".0") == 0)
            number.erase(number.size() - 2);
        return number;
    }
    if (value.isObject())
        return "[object Object]";
    if (!value.isArray())
        return "null";

    std::string result;
    const std::vector<Json> values = value.items();
    for (size_t i = 0; i < values.size(); ++i) {
        if (i)
            result.push_back(',');
        if (!values[i].valid())
            continue;
        if (values[i].isArray() || values[i].isObject())
            result += javascriptString(values[i]);
        else if (values[i].isString())
            result += values[i].string();
        else
            result += values[i].dump();
    }
    return result;
}

Json normalizePreloadState(const Json &state) {
    Json normalized = Json::object();
    normalized.set("off", copyEntries(state["off"]));
    normalized.set("lg", state["lg"].isArray() ? copyEntries(state["lg"]) : Json());
    return normalized;
}

bool preloadStateContainsOff(const Json &state, const std::string &id) {
    return containsId(normalizePreloadState(state)["off"], id);
}

bool preloadChange(const Json &stateValue, const Json &currentList,
                   const std::string &id, bool off, Json &updatedState,
                   Json &updatedList, bool &enabled) {
    updatedState = Json();
    updatedList = Json();
    enabled = false;
    if (!currentList.isArray() || id.empty() || preloadEssentialApp(id))
        return false;

    const std::vector<Json> current = currentList.items();
    bool found = false;
    for (const Json &entry : current)
        if (entry.isObject() && entry["id"].isString() &&
            entry["id"].string() == id)
            found = true;
    if (!found)
        return false;

    Json state = normalizePreloadState(stateValue);
    Json lg = state["lg"];
    if (!lg.isArray())
        lg = copyEntries(currentList);

    std::vector<Json> offEntries = state["off"].items();
    std::vector<Json>::iterator at = std::find_if(
        offEntries.begin(), offEntries.end(),
        [&id](const Json &entry) {
            return entry.isString() && entry.string() == id;
        });
    if (off && at == offEntries.end())
        offEntries.push_back(Json::text(id));
    else if (!off && at != offEntries.end())
        offEntries.erase(at);

    const std::vector<Json> lgEntries = lg.items();
    const Json *original = nullptr;
    for (const Json &entry : lgEntries)
        if (entry.isObject() && entry["id"].isString() &&
            entry["id"].string() == id)
            original = &entry;
    const Json originalEnabled = original ? (*original)["isEnabled"] : Json();
    enabled = off ? false : !(originalEnabled.isBoolean() &&
                              !originalEnabled.boolean());

    Json offArray = Json::array();
    for (const Json &offEntry : offEntries)
        offArray.add(offEntry);
    updatedState = Json::object();
    updatedState.set("off", offArray);
    updatedState.set("lg", lg);

    updatedList = Json::array();
    for (const Json &entry : current) {
        Json copy = entry.isObject() ? copyObject(entry) : entry;
        const std::string entryId = entry["id"].string();
        if (containsId(offArray, entryId))
            copy.set("isEnabled", Json::flag(false));
        else if (entryId == id)
            copy.set("isEnabled", Json::flag(enabled));
        updatedList.add(copy);
    }
    return true;
}

Json preloadPolicyRequest(const std::string &id, bool enabled) {
    Json request = Json::object();
    Json application = Json::object();
    application.set("id", Json::text(id));
    application.set("isEnabled", Json::flag(enabled));
    request.set("application", application);
    return request;
}

Json preloadConfigsRequest(const Json &list) {
    Json configs = Json::object();
    configs.set(preloadConfigKey(), list);
    Json request = Json::object();
    request.set("configs", configs);
    return request;
}

Json preloadApps(const Json &list, const Json &installedApps,
                 const Json &runningApps, const Json &stateValue) {
    Json result = Json::array();
    if (!list.isArray())
        return result;

    const Json state = normalizePreloadState(stateValue);
    const Json lg = state["lg"].isArray() ? state["lg"] : list;
    std::vector<std::pair<std::string, Json>> titles;
    if (installedApps.isArray()) {
        for (const Json &app : installedApps.items()) {
            if (!app.isObject() || !app["id"].isString())
                continue;
            const std::string id = app["id"].string();
            titles.push_back(std::make_pair(id, Json::text(appTitle(app, id))));
        }
    }
    std::vector<std::string> loaded;
    if (runningApps.isArray()) {
        for (const Json &app : runningApps.items())
            if (app.isObject() && app["id"].isString())
                loaded.push_back(app["id"].string());
    }
    std::vector<std::pair<std::string, Json>> lgById;
    for (const Json &entry : lg.items())
        if (entry.isObject() && entry["id"].isString())
            lgById.push_back(std::make_pair(entry["id"].string(), entry));

    struct Row {
        std::string title;
        std::string id;
        Json value;
    };
    std::vector<Row> rows;
    for (const Json &entry : list.items()) {
        if (!entry.isObject() || !entry["id"].isString())
            continue;
        const std::string id = entry["id"].string();
        if (preloadEssentialApp(id))
            continue;

        const Json *title = nullptr;
        for (const auto &candidate : titles)
            if (candidate.first == id)
                title = &candidate.second;
        if (!title)
            continue;

        const Json *lgEntry = nullptr;
        for (const auto &candidate : lgById)
            if (candidate.first == id)
                lgEntry = &candidate.second;
        const bool off = containsId(state["off"], id);
        if (lgEntry && (*lgEntry)["isEnabled"].isBoolean() &&
            !(*lgEntry)["isEnabled"].boolean() && !off)
            continue;

        Row row;
        row.title = title->string();
        row.id = id;
        row.value = Json::object();
        row.value.set("id", Json::text(id));
        row.value.set("title", *title);
        row.value.set("memoryMb", entry["requiredMemory"].isNumber()
                                     ? entry["requiredMemory"] : Json());
        row.value.set("off", Json::flag(off));
        row.value.set("loaded",
                      Json::flag(std::find(loaded.begin(), loaded.end(), id) != loaded.end()));
        rows.push_back(row);
    }
    std::stable_sort(rows.begin(), rows.end(),
                     [](const Row &left, const Row &right) {
                         return lower(left.title) < lower(right.title);
                     });
    for (const Row &row : rows)
        result.add(row.value);
    return result;
}

} // namespace service
} // namespace nativetv
