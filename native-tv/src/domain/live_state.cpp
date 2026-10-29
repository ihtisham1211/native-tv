#include "domain/live_state.hpp"
#include "domain/names.hpp"
#include <algorithm>
#include <cmath>
#include <utility>

namespace nativetv {
namespace state {
namespace {
bool sameScalar(const Json &left, const Json &right) {
    if (left.isNumber() && right.isNumber())
        return left.number() == right.number();
    if (left.isString() && right.isString())
        return left.string() == right.string();
    if (left.isBoolean() && right.isBoolean())
        return left.boolean() == right.boolean();
    return false;
}

bool hasField(const Json &object, const std::string &key) {
    const std::vector<std::string> keys = object.keys();
    return std::find(keys.begin(), keys.end(), key) != keys.end();
}

bool primaryDisplay(const Json &window) {
    const Json display = window["displayId"];
    return !hasField(window, "displayId") ||
           (display.isNumber() && display.number() == 0);
}

bool truthy(const Json &value) {
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

Json applicationValues(const std::string &appId, const MappingOptions &options) {
    const std::string shortId = names::shortAppId(appId, options.inputAliases);
    Json values = Json::object();
    values.set("app_id", Json::text(appId));
    values.set("app", Json::text(shortId));
    values.set("display_title",
               Json::text(names::inputTitle(shortId, options.inputNames[shortId])));
    return values;
}

bool validLevel(const Json &value) {
    return value.isNumber() && value.number() >= 0 && std::isfinite(value.number());
}

std::string scalarString(const Json &value) {
    return value.isString() ? value.string() : value.dump();
}

Json mediaValue(const std::string &state, const Json &playerType, bool fullScreen) {
    Json media = Json::object();
    media.set("state", Json::text(state));
    if (truthy(playerType) && playerType.isString())
        media.set("playerType", playerType);
    else
        media.set("playerType", Json());
    media.set("fullScreen", Json::flag(fullScreen));
    Json values = Json::object();
    // A media state is a compound public value. Encode it into one scalar so
    // cache changes and the Node event bridge cannot expose a partial snapshot.
    values.set("snapshot", Json::text(media.dump()));
    return values;
}

Json mapMedia(const Json &response) {
    const Json acbs = response["acbs"];
    if (acbs.isArray()) {
        const Json pipe = acbs.size() ? acbs[0] : Json();
        const Json playState = pipe["playStateNow"];
        if (truthy(playState)) {
            return mediaValue(scalarString(playState), pipe["playerType"],
                              !pipe["isFullScreen"].isBoolean() ||
                                  pipe["isFullScreen"].boolean());
        }
        return mediaValue("idle", Json(), false);
    }

    const Json windows = response["foregroundAppInfo"];
    if (!windows.isArray())
        return Json();
    for (const Json &app : windows.items()) {
        const Json playState = app["playState"];
        if (truthy(playState))
            return mediaValue(scalarString(playState), app["type"], true);
    }
    return mediaValue("idle", Json(), false);
}

void addAudioValues(Json &values, const Json &response, bool nested) {
    const Json volume = response["volume"];
    const Json muted = response[nested ? "muteStatus" : "muted"];
    if (validLevel(volume))
        values.set("volume", volume);
    if ((nested && muted.isBoolean()) || (!nested && hasField(response, "muted")))
        values.set("muted", Json::flag(truthy(muted)));

    const Json outputValue = nested ? response["soundOutput"] : response["scenario"];
    std::string output = truthy(outputValue) ? outputValue.string() : "";
    if (!nested) {
        const std::string prefix = "mastervolume_";
        if (output.compare(0, prefix.size(), prefix) == 0)
            output.erase(0, prefix.size());
    }
    if (!output.empty())
        values.set("output", names::soundOutput(output)["display"]);
}

Json mapSurface(const Json &response) {
    const Json windows = response["foregroundAppInfo"];
    if (!windows.isArray())
        return Json();
    std::string card, home, overlay;
    for (const Json &window : windows.items()) {
        if (!primaryDisplay(window))
            continue;
        const std::string appId = window["appId"].string();
        const std::string type = window["windowType"].string();
        if (type == "_WEBOS_WINDOW_TYPE_CARD")
            card = appId;
        if (type == "_WEBOS_WINDOW_TYPE_HOME" && appId == "com.webos.app.home")
            home = appId;
        if (type == "_WEBOS_WINDOW_TYPE_OVERLAY" &&
            (appId.compare(0, 13, "com.palm.app.") == 0 ||
             appId.compare(0, 14, "com.webos.app.") == 0))
            overlay = appId;
    }
    Json values = Json::object();
    values.set("control_app_id", Json::text(!overlay.empty() ? overlay
                                                   : !home.empty() ? home : card));
    return values;
}

Json mapFocus(const Json &response) {
    const Json windows = response["focusedWindowInfo"];
    if (!windows.isArray())
        return Json();
    std::string appId;
    for (const Json &window : windows.items()) {
        if (primaryDisplay(window))
            appId = window["appId"].string();
    }
    Json values = Json::object();
    values.set("control_app_id", Json::text(appId));
    return values;
}
} // namespace

void Cache::onChange(Listener listener) {
    if (listener) {
        std::lock_guard<std::mutex> lock(mutex_);
        listeners_.push_back(std::move(listener));
    }
}

bool Cache::update(const Event &event) {
    if (event.group.empty() || event.key.empty() || !event.value.valid())
        return false;
    std::vector<Listener> listeners;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto &group = values_[event.group];
        auto found = group.find(event.key);
        if (found != group.end() && event.sourceTime < found->second.sourceTime)
            return false;

        const bool changed = found == group.end() || !sameScalar(found->second.value, event.value);
        if (found == group.end()) {
            Entry entry;
            entry.value = event.value;
            entry.sourceTime = event.sourceTime;
            group.emplace(event.key, std::move(entry));
        } else {
            found->second.sourceTime = event.sourceTime;
            if (changed)
                found->second.value = event.value;
        }
        if (!changed)
            return false;
        listeners = listeners_;
    }

    for (const Listener &listener : listeners) {
        try {
            listener(event);
        } catch (...) {
            // A state consumer must not prevent other consumers from updating.
        }
    }
    return true;
}

Json Cache::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Json result = Json::object();
    for (const auto &group : values_) {
        Json values = Json::object();
        for (const auto &entry : group.second)
            values.set(entry.first, entry.second.value);
        result.set(group.first, values);
    }
    return result;
}

Json mapSubscription(const std::string &group, const Json &response,
                     const MappingOptions &options) {
    if (!response.isObject() ||
        (response["returnValue"].isBoolean() && !response["returnValue"].boolean()))
        return Json();

    if (group == "picture") {
        const Json settings = response["settings"];
        if (!settings.isObject())
            return Json();
        Json values = Json::object();
        for (const std::string &key : settings.keys()) {
            const Json value = settings[key];
            if (value.valid())
                values.set(key, value);
        }
        return values;
    }
    if (group == "power") {
        Json raw = response["state"];
        if (!truthy(raw))
            raw = response["processing"];
        if (!truthy(raw))
            return Json();
        const Json mapped = names::powerState(raw);
        Json values = Json::object();
        values.set("state", raw);
        values.set("screenOn", mapped["screenOn"]);
        values.set("systemOn", mapped["systemOn"]);
        return values;
    }
    if (group == "application") {
        const Json appId = response["appId"];
        return appId.isString() && !appId.string().empty()
                   ? applicationValues(appId.string(), options)
                   : Json();
    }
    if (group == "surface")
        return mapSurface(response);
    if (group == "focus")
        return mapFocus(response);
    if (group == "audio") {
        Json values = Json::object();
        const Json status = response["volumeStatus"];
        if (status.isObject())
            addAudioValues(values, status, true);
        else
            addAudioValues(values, response, false);
        return values;
    }
    if (group == "media")
        return mapMedia(response);
    return Json();
}

} // namespace state
} // namespace nativetv
