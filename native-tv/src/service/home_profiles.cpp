#include "service/home_profiles.hpp"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace nativetv {
namespace service {
namespace {
const char *kRows[] = {"app", "dev", "ch"};
const char *kColors[] = {"#a8c7fa", "#f2b8b5", "#6dd58c", "#ffb74d",
                         "#7fcfff", "#d0bcff"};
const char *kLayoutKeys[] = {"rows", "order", "sort", "hidden", "channels",
                             "picture", "appNames", "clockFormat", "showInputs",
                             "showLgHome", "showLive", "colours"};
const char *kPromoApps[] = {"com.webos.app.lgchannels",
                            "com.webos.app.sportsteamsettings",
                            "com.webos.app.lifeonscreen", "amazon.alexa.view",
                            "com.webos.app.camera"};
const size_t kRowCount = sizeof(kRows) / sizeof(kRows[0]);
const size_t kColorCount = sizeof(kColors) / sizeof(kColors[0]);
const size_t kLayoutKeyCount = sizeof(kLayoutKeys) / sizeof(kLayoutKeys[0]);
const size_t kPromoCount = sizeof(kPromoApps) / sizeof(kPromoApps[0]);
const size_t kMaximumProfiles = 5;
const size_t kMaximumChannels = 40;
const size_t kMaximumStringList = 500;
const size_t kMaximumStringUnits = 200;
const char *kMainProfile = "main";
const char *kHomeTile = "lg-home";

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

size_t utf16Length(const std::string &text) {
    size_t units = 0;
    for (size_t offset = 0; offset < text.size();) {
        const CodePoint point = decode(text, offset);
        units += point.value > 0xffff ? 2 : 1;
        offset += point.width;
    }
    return units;
}

std::string normalizeName(const Json &value) {
    if (!value.isString()) return "";
    const std::string text = value.string();
    std::string result;
    bool pendingSpace = false;
    for (size_t offset = 0; offset < text.size();) {
        const CodePoint point = decode(text, offset);
        if (javascriptWhitespace(point.value)) {
            pendingSpace = !result.empty();
        } else {
            if (pendingSpace) result.push_back(' ');
            result.append(text, offset, point.width);
            pendingSpace = false;
        }
        offset += point.width;
    }
    return utf16Length(result) <= 20 ? result : "";
}

bool contains(const std::vector<std::string> &values, const std::string &value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

Json copyJson(const Json &value) {
    return value.valid() ? Json::parse(value.dump()) : Json();
}

Json strings(const std::vector<std::string> &values) {
    Json result = Json::array();
    for (const std::string &value : values) result.add(Json::text(value));
    return result;
}

std::vector<std::string> stringList(const Json &value, size_t maximum = kMaximumStringList) {
    std::vector<std::string> result;
    if (!value.isArray()) return result;
    for (const Json &item : value.items()) {
        if (result.size() >= maximum) break;
        if (!item.isString()) continue;
        const std::string text = item.string();
        if (text.empty() || utf16Length(text) > kMaximumStringUnits ||
            contains(result, text))
            continue;
        result.push_back(text);
    }
    return result;
}

bool isRow(const std::string &value) {
    for (size_t i = 0; i < kRowCount; ++i)
        if (value == kRows[i]) return true;
    return false;
}

std::vector<std::string> rowValues(const Json &object, const std::string &row) {
    return stringList(object.isObject() ? object[row] : Json());
}

Json defaultDocument() {
    Json document = Json::object();
    document.set("enabled", Json::flag(false));
    document.set("restorePending", Json::flag(false));
    Json rows = Json::array();
    Json order = Json::object();
    Json sort = Json::object();
    Json hidden = Json::object();
    for (size_t i = 0; i < kRowCount; ++i) {
        rows.add(Json::text(kRows[i]));
        order.set(kRows[i], Json::array());
        sort.set(kRows[i], Json::text("custom"));
        hidden.set(kRows[i], Json::array());
    }
    Json hiddenApps = Json::array();
    for (size_t i = 0; i < kPromoCount; ++i)
        hiddenApps.add(Json::text(kPromoApps[i]));
    hidden.set("app", hiddenApps);
    document.set("rows", rows);
    document.set("order", order);
    document.set("sort", sort);
    document.set("hidden", hidden);
    document.set("channels", Json::array());
    document.set("picture", Json::flag(false));
    document.set("appNames", Json::flag(true));
    document.set("clockFormat", Json::text("24h"));
    document.set("showInputs", Json::flag(true));
    document.set("showLgHome", Json::flag(true));
    document.set("showLive", Json::flag(false));
    document.set("openOnPowerOn", Json::flag(false));
    document.set("colours", Json::parse("null"));
    document.set("profiles", Json::array());
    document.set("profile", Json::text(kMainProfile));
    document.set("hideLgHomeAtStart", Json::parse("null"));
    document.set("lgAutoLaunch", Json::parse("null"));
    document.set("lgAutoLaunchCheck", Json::parse("null"));
    document.set("snapshot", Json::parse("null"));
    return document;
}

std::vector<std::string> ordered(const std::vector<std::string> &saved,
                                 const std::vector<std::string> &available) {
    std::vector<std::string> result;
    for (const std::string &value : saved)
        if (contains(available, value) && !contains(result, value)) result.push_back(value);
    for (const std::string &value : available)
        if (!contains(result, value)) result.push_back(value);
    return result;
}

bool validColor(const std::string &value) {
    if (value.size() != 7 || value[0] != '#') return false;
    for (size_t i = 1; i < value.size(); ++i)
        if (!std::isxdigit(static_cast<unsigned char>(value[i]))) return false;
    return true;
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::string profileColor(const Json &value) {
    if (!value.isString()) return "";
    const std::string color = lower(value.string());
    for (size_t i = 0; i < kColorCount; ++i)
        if (color == kColors[i]) return color;
    return "";
}

Json validColours(const Json &value) {
    if (!value.isObject() || !value["text"].isString() ||
        !value["accent"].isString())
        return Json::parse("null");
    const std::string text = lower(value["text"].string());
    const std::string accent = lower(value["accent"].string());
    if (!validColor(text) || !validColor(accent)) return Json::parse("null");
    Json result = Json::object();
    result.set("text", Json::text(text));
    result.set("accent", Json::text(accent));
    return result;
}

bool validProfileId(const std::string &value) {
    if (value.empty() || value.size() > 12) return false;
    for (unsigned char character : value)
        if (!(character >= 'a' && character <= 'z') &&
            !(character >= '0' && character <= '9'))
            return false;
    return true;
}

bool profileExists(const Json &profiles, const std::string &id) {
    if (!profiles.isArray()) return false;
    for (const Json &profile : profiles.items())
        if (profile["id"].string() == id) return true;
    return false;
}

Json findProfile(const Json &profiles, const std::string &id) {
    if (profiles.isArray())
        for (const Json &profile : profiles.items())
            if (profile["id"].string() == id) return profile;
    return Json();
}

Json layoutKeys(const Json &layout) {
    Json result = Json::object();
    for (size_t i = 0; i < kLayoutKeyCount; ++i)
        if (layout.has(kLayoutKeys[i])) result.set(kLayoutKeys[i], layout[kLayoutKeys[i]]);
    return result;
}

Json errorResponse(const std::string &error) {
    Json result = Json::object();
    result.set("ok", Json::flag(false));
    result.set("error", Json::text(error));
    return result;
}

Json layoutResponse(const Json &document) {
    Json result = Json::object();
    result.set("ok", Json::flag(true));
    result.set("layout", homeLayout(document));
    return result;
}
} // namespace

std::string normalizeHomeProfileName(const Json &value) {
    return normalizeName(value);
}

Json normalizeHomeDocument(const Json &raw) {
    Json result = defaultDocument();
    if (!raw.isObject()) return result;

    std::vector<std::string> rows = stringList(raw["rows"]);
    std::vector<std::string> validRows;
    for (const std::string &row : rows)
        if (isRow(row) && !contains(validRows, row)) validRows.push_back(row);
    for (size_t i = 0; i < kRowCount; ++i)
        if (!contains(validRows, kRows[i])) validRows.push_back(kRows[i]);
    result.set("rows", strings(validRows));

    Json order = Json::object();
    Json sort = Json::object();
    Json hidden = Json::object();
    for (size_t i = 0; i < kRowCount; ++i) {
        const std::string row = kRows[i];
        order.set(row, strings(rowValues(raw["order"], row)));
        const std::string sortValue = raw["sort"][row].string();
        sort.set(row, Json::text(sortValue == "recent" ? "recent" : "custom"));
        std::vector<std::string> hiddenValues = rowValues(raw["hidden"], row);
        if (row == "app")
            hiddenValues.erase(std::remove(hiddenValues.begin(), hiddenValues.end(), kHomeTile),
                               hiddenValues.end());
        hidden.set(row, strings(hiddenValues));
    }
    std::vector<std::string> channels = stringList(raw["channels"], kMaximumChannels);
    const std::vector<std::string> channelOrder = rowValues(raw["order"], "ch");
    const std::vector<std::string> hiddenChannels = rowValues(raw["hidden"], "ch");
    channels = ordered(channelOrder, channels);
    channels.erase(std::remove_if(channels.begin(), channels.end(),
        [&hiddenChannels](const std::string &id) { return contains(hiddenChannels, id); }),
        channels.end());
    order.set("ch", Json::array());
    hidden.set("ch", Json::array());
    result.set("order", order);
    result.set("sort", sort);
    result.set("hidden", hidden);
    result.set("channels", strings(channels));

    result.set("enabled", Json::flag(raw["enabled"].boolean()));
    result.set("restorePending", Json::flag(raw["restorePending"].boolean()));
    result.set("picture", Json::flag(raw["picture"].boolean()));
    result.set("appNames", Json::flag(!raw["appNames"].valid() || raw["appNames"].boolean(true)));
    result.set("clockFormat", Json::text(raw["clockFormat"].string() == "12h" ? "12h" : "24h"));
    result.set("showInputs", Json::flag(!raw["showInputs"].valid() || raw["showInputs"].boolean(true)));
    result.set("showLgHome", Json::flag(!raw["showLgHome"].valid() || raw["showLgHome"].boolean(true)));
    result.set("showLive", Json::flag(raw["showLive"].boolean()));
    result.set("openOnPowerOn", Json::flag(raw["openOnPowerOn"].boolean()));
    result.set("colours", validColours(raw["colours"]));

    Json profiles = Json::array();
    if (raw["profiles"].isArray()) {
        for (const Json &profile : raw["profiles"].items()) {
            if (profiles.size() >= kMaximumProfiles || !profile.isObject()) continue;
            const std::string id = profile["id"].string();
            const std::string name = normalizeName(profile["name"]);
            if (!validProfileId(id) || name.empty() || profileExists(profiles, id)) continue;
            Json normalizedProfile = Json::object();
            normalizedProfile.set("id", Json::text(id));
            normalizedProfile.set("name", Json::text(name));
            std::string color = profileColor(profile["color"]);
            normalizedProfile.set("color", Json::text(color.empty() ? kColors[0] : color));
            if (profile["layout"].isObject()) {
                const Json normalizedLayout = normalizeHomeDocument(layoutKeys(profile["layout"]));
                normalizedProfile.set("layout", homeLayout(normalizedLayout));
            } else {
                normalizedProfile.set("layout", Json::parse("null"));
            }
            profiles.add(normalizedProfile);
        }
    }
    result.set("profiles", profiles);
    const std::string active = raw["profile"].string();
    result.set("profile", Json::text(profileExists(profiles, active)
        ? active : profiles.size() ? profiles[0]["id"].string() : kMainProfile));
    if (raw["hideLgHomeAtStart"].isBoolean())
        result.set("hideLgHomeAtStart", raw["hideLgHomeAtStart"]);
    if (raw["lgAutoLaunch"].isString() && !raw["lgAutoLaunch"].string().empty())
        result.set("lgAutoLaunch", raw["lgAutoLaunch"]);
    if (raw["lgAutoLaunchCheck"].isString() && !raw["lgAutoLaunchCheck"].string().empty())
        result.set("lgAutoLaunchCheck", raw["lgAutoLaunchCheck"]);
    const Json snapshot = raw["snapshot"];
    if (snapshot.isObject() && snapshot["settings"].isObject() &&
        snapshot["takenAt"].isNumber())
        result.set("snapshot", snapshot);
    return result;
}

Json homeLayout(const Json &document) {
    Json result = Json::object();
    for (size_t i = 0; i < kLayoutKeyCount; ++i)
        result.set(kLayoutKeys[i], document[kLayoutKeys[i]]);
    return result;
}

Json homeProfileList(const Json &input, const std::string &mainName) {
    const Json document = normalizeHomeDocument(input);
    const Json profiles = document["profiles"];
    Json result = Json::array();
    if (profiles.size() == 0) {
        Json main = Json::object();
        main.set("id", Json::text(kMainProfile));
        main.set("name", Json::text(mainName));
        main.set("color", Json::text(kColors[0]));
        main.set("active", Json::flag(true));
        result.add(main);
        return result;
    }
    for (const Json &profile : profiles.items()) {
        Json item = Json::object();
        item.set("id", profile["id"]);
        item.set("name", profile["name"]);
        item.set("color", profile["color"]);
        item.set("active", Json::flag(profile["id"].string() == document["profile"].string()));
        result.add(item);
    }
    return result;
}

HomeDocumentUpdate updateHomeLayout(const Json &input, const Json &body) {
    HomeDocumentUpdate update;
    const Json current = normalizeHomeDocument(input);
    update.document = current;
    if (!body.isObject()) {
        update.response = errorResponse("layout expected");
        return update;
    }
    Json proposed = Json::object();
    const char *preserved[] = {"enabled", "restorePending", "snapshot",
        "hideLgHomeAtStart", "openOnPowerOn", "lgAutoLaunch", "lgAutoLaunchCheck",
        "profiles", "profile"};
    for (const char *key : preserved)
        if (current.has(key)) proposed.set(key, current[key]);
    proposed.set("rows", body.has("rows") ? body["rows"] : current["rows"]);
    Json nextOrder = Json::object();
    Json nextSort = Json::object();
    Json nextHidden = Json::object();
    for (const char *row : kRows) {
        const std::string name = row;
        nextOrder.set(name, body["order"].isObject() && body["order"].has(name)
            ? body["order"][name] : current["order"][name]);
        nextSort.set(name, body["sort"].isObject() && body["sort"].has(name)
            ? body["sort"][name] : current["sort"][name]);
        nextHidden.set(name, body["hidden"].isObject() && body["hidden"].has(name)
            ? body["hidden"][name] : current["hidden"][name]);
    }
    proposed.set("order", nextOrder);
    proposed.set("sort", nextSort);
    proposed.set("hidden", nextHidden);
    const Json channelOrder = body["order"].isObject() && body["order"]["ch"].isArray()
        ? body["order"]["ch"] : Json::array();
    const Json availableChannels = body.has("channels") ? body["channels"] : current["channels"];
    proposed.set("channels", strings(ordered(stringList(channelOrder),
        stringList(availableChannels, kMaximumChannels))));
    const char *booleanKeys[] = {"picture", "appNames", "showInputs", "showLgHome", "showLive"};
    for (const char *key : booleanKeys)
        proposed.set(key, body[key].isBoolean() ? body[key] : current[key]);
    proposed.set("clockFormat", Json::text(body["clockFormat"].string() == "12h" ||
        body["clockFormat"].string() == "24h" ? body["clockFormat"].string()
                                               : current["clockFormat"].string()));
    proposed.set("colours", body.has("colours") ? body["colours"] : current["colours"]);
    Json next = normalizeHomeDocument(proposed);
    if (body["reset"].boolean()) {
        const Json defaults = defaultDocument();
        const char *resetKeys[] = {"rows", "order", "sort", "hidden", "showInputs",
                                   "showLgHome", "showLive"};
        for (const char *key : resetKeys) next.set(key, defaults[key]);
    }
    update.document = next;
    update.response = layoutResponse(next);
    update.changed = next.dump() != current.dump();
    return update;
}

HomeDocumentUpdate updateHomeProfile(const Json &input, const Json &body,
                                     const std::string &mainName) {
    HomeDocumentUpdate update;
    const Json current = normalizeHomeDocument(input);
    update.document = current;
    if (!body.isObject()) {
        update.response = errorResponse("a profile action is expected");
        return update;
    }

    Json profiles = current["profiles"];
    const std::string activeId = profiles.size() ? current["profile"].string() : kMainProfile;
    if (profiles.size() == 0) {
        Json main = Json::object();
        main.set("id", Json::text(kMainProfile));
        main.set("name", Json::text(mainName));
        main.set("color", Json::text(kColors[0]));
        main.set("layout", Json::parse("null"));
        profiles.add(main);
    }
    Json list = copyJson(profiles);
    Json now = findProfile(list, activeId);
    if (!now.valid()) now = list[0];
    Json target = now;
    Json nowUpdated = copyJson(now);
    nowUpdated.set("layout", homeLayout(current));
    Json updatedProfiles = Json::array();
    for (const Json &profile : list.items())
        updatedProfiles.add(profile["id"].string() == now["id"].string() ? nowUpdated : profile);
    list = updatedProfiles;
    now = nowUpdated;

    const std::string action = body["action"].string();
    std::string error;
    if (action == "switch") {
        target = findProfile(list, body["id"].string());
        if (!target.valid()) error = "no such profile";
    } else if (action == "add") {
        if (list.size() >= kMaximumProfiles) {
            error = "there can be 5 profiles at most";
        } else {
            unsigned next = 1;
            while (profileExists(list, "p" + std::to_string(next))) ++next;
            std::string color = profileColor(body["color"]);
            if (color.empty()) {
                for (size_t i = 0; i < kColorCount; ++i) {
                    bool used = false;
                    for (const Json &profile : list.items())
                        if (profile["color"].string() == kColors[i]) used = true;
                    if (!used) { color = kColors[i]; break; }
                }
                if (color.empty()) color = kColors[0];
            }
            Json added = Json::object();
            added.set("id", Json::text("p" + std::to_string(next)));
            const std::string name = normalizeName(body["name"]);
            added.set("name", Json::text(name.empty()
                ? "Profile " + std::to_string(list.size() + 1) : name));
            added.set("color", Json::text(color));
            added.set("layout", homeLayout(defaultDocument()));
            list.add(added);
            target = body["switch"].isBoolean() && body["switch"].boolean()
                ? added : now;
        }
    } else if (action == "rename") {
        const std::string id = body["id"].string();
        Json found = findProfile(list, id);
        if (!found.valid()) {
            error = "no such profile";
        } else {
            const std::string name = normalizeName(body["name"]);
            const std::string color = profileColor(body["color"]);
            if (name.empty() && color.empty()) {
                error = "a name of up to 20 characters or one of the colours is expected";
            } else {
                Json replaced = Json::array();
                for (Json profile : list.items()) {
                    profile = copyJson(profile);
                    if (profile["id"].string() == id) {
                        if (!name.empty()) profile.set("name", Json::text(name));
                        if (!color.empty()) profile.set("color", Json::text(color));
                    }
                    replaced.add(profile);
                }
                list = replaced;
                target = findProfile(list, now["id"].string());
            }
        }
    } else if (action == "remove") {
        const std::string id = body["id"].string();
        Json found = findProfile(list, id);
        if (!found.valid()) {
            error = "no such profile";
        } else if (list.size() < 2) {
            error = "the last profile cannot be removed";
        } else {
            Json remaining = Json::array();
            bool removedActive = id == now["id"].string();
            for (const Json &profile : list.items())
                if (profile["id"].string() != id) remaining.add(profile);
            list = remaining;
            if (removedActive) target = list[0];
        }
    } else {
        error = "a profile action is expected";
    }
    if (!error.empty()) {
        update.response = errorResponse(error);
        return update;
    }

    Json next = copyJson(current);
    next.set("profiles", list);
    next.set("profile", target["id"]);
    if (target["id"].string() != now["id"].string()) {
        Json layout = target["layout"].isObject()
            ? target["layout"] : homeLayout(defaultDocument());
        for (size_t i = 0; i < kLayoutKeyCount; ++i)
            next.set(kLayoutKeys[i], layout[kLayoutKeys[i]]);
    }
    next = normalizeHomeDocument(next);
    update.document = next;
    Json response = Json::object();
    response.set("ok", Json::flag(true));
    response.set("profiles", homeProfileList(next, mainName));
    response.set("profile", Json::text(next["profile"].string()));
    response.set("layout", homeLayout(next));
    update.response = response;
    update.changed = next.dump() != current.dump();
    return update;
}

HomeDocumentUpdate updateHomeBooleanSetting(const Json &input,
                                            const std::string &key,
                                            const Json &body) {
    HomeDocumentUpdate update;
    const Json current = normalizeHomeDocument(input);
    update.document = current;
    if (key != "hideLgHomeAtStart" && key != "openOnPowerOn" &&
        key != "enabled" && key != "restorePending" && key != "lifecycle") {
        update.response = errorResponse("unknown Home setting");
        return update;
    }
    if (key == "lifecycle" && body.isObject() &&
        body["enabled"].isBoolean() && body["restorePending"].isBoolean()) {
        Json next = copyJson(current);
        next.set("enabled", body["enabled"]);
        next.set("restorePending", body["restorePending"]);
        update.document = normalizeHomeDocument(next);
        update.response = Json::object();
        update.response.set("ok", Json::flag(true));
        update.changed = update.document.dump() != current.dump();
        return update;
    }
    if (!body.isObject() || !body[key].isBoolean()) {
        if (key == "lifecycle") {
            update.response = errorResponse("Home lifecycle state expected");
            return update;
        }
        update.response = errorResponse(key + " expected");
        return update;
    }
    Json next = copyJson(current);
    next.set(key, body[key]);
    update.document = normalizeHomeDocument(next);
    update.response = Json::object();
    update.response.set("ok", Json::flag(true));
    update.changed = update.document.dump() != current.dump();
    return update;
}

} // namespace service
} // namespace nativetv
