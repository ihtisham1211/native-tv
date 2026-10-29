#include "service/home_now.hpp"
#include <cassert>

using nativetv::Json;

int main() {
    unsigned port = 0;
    assert(nativetv::service::homeNowInputPort("com.webos.app.hdmi2", port));
    assert(port == 2);
    assert(!nativetv::service::homeNowInputPort("com.webos.app.hdmi22", port));
    assert(nativetv::service::homeNowIsLiveTv("com.webos.app.livetv"));

    Json devices = Json::parse(R"([
      {"id":"hdmi2:av-receiver","name":"AV Receiver","port":2},
      {"id":"hdmi2:game-console","name":"Game Console","port":2},
      {"id":"hdmi1","name":"HDMI 1","port":1}
    ])");
    Json input = nativetv::service::homeNowInput("com.webos.app.hdmi2", 1234,
                                                   devices);
    assert(input["kind"].string() == "input");
    assert(input["appId"].string() == "com.webos.app.hdmi2");
    assert(input["since"].number() == 1234);
    assert(input["port"].number() == 2);
    assert(input["title"].string() == "AV Receiver / Game Console");
    assert(input["devices"].size() == 2);
    assert(input["devices"][0].string() == "hdmi2:av-receiver");

    Json channels = Json::parse(R"([
      {"id":"channel-1","number":"1","name":"BBC One HD","radio":false}
    ])");
    Json lastChannel = Json::object();
    lastChannel.set("channelId", Json::text("channel-1"));
    Json programme = Json::parse(
        R"({"title":"News","start":1000,"end":2000})");
    Json channel = nativetv::service::homeNowChannel(
        "com.webos.app.livetv", 4567, channels, lastChannel, programme);
    assert(channel["kind"].string() == "channel");
    assert(channel["since"].number() == 4567);
    assert(channel["channel"]["id"].string() == "channel-1");
    assert(channel["programme"]["title"].string() == "News");
    lastChannel.set("channelId", Json::text("unknown"));
    channel = nativetv::service::homeNowChannel(
        "com.webos.app.livetv", 4567, channels, lastChannel, programme);
    assert(!channel["channel"].valid());
    assert(!channel["programme"].valid());

    Json apps = Json::parse(R"([
      {"id":"com.webos.app.netflix","title":"Netflix"}
    ])");
    Json app = nativetv::service::homeNowApp("com.webos.app.netflix", 8901, apps);
    assert(app["kind"].string() == "app");
    assert(app["title"].string() == "Netflix");
    assert(app["since"].number() == 8901);
    assert(!nativetv::service::homeNowEmpty()["kind"].valid());
    return 0;
}
