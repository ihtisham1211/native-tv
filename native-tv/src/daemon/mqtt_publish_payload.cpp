#include "daemon/mqtt_publish_payload.hpp"
#include "service/preload.hpp"

namespace nativetv {
namespace daemon {

Json mqttPublishPayload(const Json &stats, const MqttPublishOptions &options) {
    Json copy = Json::parse(stats.dump());
    Json lgs = Json::object();
    for (const Json &row : options.lgRows.items()) {
        const Json value = row["type"].string() == "switch" ? row["on"] : row["value"];
        if (value.valid()) lgs.set(row["id"].string(), value);
    }
    copy.set("lgs", lgs);
    copy.set("tvOff", Json::flag(options.tvOff));
    copy.set("watch", options.watch.valid() ? options.watch : Json());
    if (options.piccapState.isObject()) {
        Json piccap = Json::object();
        piccap.set("power", options.piccapState["isRunning"]);
        copy.set("piccap", piccap);
    }
    if (options.tvOff) {
        if (!service::javascriptTruthy(copy["app"]) && !options.lastApp.empty())
            copy.set("app", Json::text(options.lastApp));
        if (!service::javascriptTruthy(copy["app_id"]) && !options.lastAppId.empty())
            copy.set("app_id", Json::text(options.lastAppId));
    }
    Json published = Json::object();
    for (const std::string &key : copy.keys())
        if (key != "apps" && key != "temps" && key != "cpuTimes")
            published.set(key, copy[key]);
    return published;
}

} // namespace daemon
} // namespace nativetv
