#include "service/home_now.hpp"
#include <vector>

namespace nativetv {
namespace service {
namespace {
const char *LIVE_TV_APP = "com.webos.app.livetv";

Json sourceResponse(const std::string &kind, const std::string &appId,
                    int64_t since) {
    Json response = Json::object();
    response.set("kind", Json::text(kind));
    response.set("appId", Json::text(appId));
    response.set("since", Json::numeric(static_cast<double>(since)));
    return response;
}

} // namespace

bool homeNowInputPort(const std::string &appId, unsigned &port) {
    static const std::string prefix = "com.webos.app.hdmi";
    if (appId.compare(0, prefix.size(), prefix) != 0 ||
        appId.size() != prefix.size() + 1 || appId[prefix.size()] < '0' ||
        appId[prefix.size()] > '9')
        return false;
    port = static_cast<unsigned>(appId[prefix.size()] - '0');
    return true;
}

bool homeNowIsLiveTv(const std::string &appId) {
    return appId == LIVE_TV_APP;
}

Json homeNowEmpty() {
    Json response = Json::object();
    response.set("kind", Json());
    return response;
}

Json homeNowInput(const std::string &appId, int64_t since,
                  const Json &devices) {
    unsigned port = 0;
    if (!homeNowInputPort(appId, port)) return homeNowEmpty();
    Json response = sourceResponse("input", appId, since);
    response.set("port", Json::numeric(port));
    std::string title;
    Json ids = Json::array();
    if (devices.isArray()) {
        for (const Json &device : devices.items()) {
            if (device["port"].number() != port) continue;
            const std::string name = device["name"].string();
            if (!name.empty()) {
                if (!title.empty()) title += " / ";
                title += name;
            }
            const std::string id = device["id"].string();
            if (!id.empty()) ids.add(Json::text(id));
        }
    }
    response.set("title", title.empty() ? Json() : Json::text(title));
    response.set("devices", ids);
    return response;
}

Json homeNowChannel(const std::string &appId, int64_t since,
                    const Json &channels, const Json &lastChannel,
                    const Json &programme) {
    if (!homeNowIsLiveTv(appId)) return homeNowEmpty();
    Json response = sourceResponse("channel", appId, since);
    const std::string channelId = lastChannel["channelId"].string();
    Json channel;
    if (!channelId.empty() && channels.isArray()) {
        for (const Json &candidate : channels.items())
            if (candidate["id"].string() == channelId) channel = candidate;
    }
    response.set("channel", channel);
    response.set("programme", channel.valid()
                                   ? programme
                                   : Json());
    return response;
}

Json homeNowApp(const std::string &appId, int64_t since, const Json &apps) {
    Json response = sourceResponse("app", appId, since);
    Json title;
    if (apps.isArray()) {
        for (const Json &app : apps.items()) {
            if (app["id"].string() == appId) {
                title = app["title"];
                break;
            }
        }
    }
    response.set("title", title);
    return response;
}

} // namespace service
} // namespace nativetv
