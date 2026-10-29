#include "service/home_channels.hpp"
#include <cassert>
#include <iostream>

using namespace nativetv;
using namespace nativetv::service;

int main() {
    Json reply = Json::object();
    reply.set("returnValue", Json::flag(true));
    Json channels = Json::array();
    Json channel = Json::object();
    channel.set("channelId", Json::text("1_1_1_0_0_0_0"));
    channel.set("channelNumber", Json::numeric(1));
    channel.set("channelName", Json::text("\u00a0BBC One HD \u2003"));
    channels.add(channel);
    Json radio = Json::object();
    radio.set("channelId", Json::text("radio-2"));
    radio.set("channelNumber", Json::text("2"));
    radio.set("channelName", Json::text("Radio 2"));
    radio.set("Radio", Json::flag(true));
    channels.add(radio);
    Json skipped = Json::object();
    skipped.set("channelId", Json::text("skipped"));
    skipped.set("skipped", Json::flag(true));
    channels.add(skipped);
    Json invisible = Json::object();
    invisible.set("channelId", Json::text("invisible"));
    invisible.set("Invisible", Json::flag(true));
    channels.add(invisible);
    reply.set("channelList", channels);

    HomeChannels result = homeChannels(reply);
    assert(result.tunerAvailable);
    assert(result.channels.size() == 2);
    assert(result.channels[0]["id"].string() == "1_1_1_0_0_0_0");
    assert(result.channels[0]["number"].string() == "1");
    assert(result.channels[0]["name"].string() == "BBC One HD");
    assert(!result.channels[0]["radio"].boolean());
    assert(result.channels[1]["radio"].boolean());

    Json empty = Json::object();
    empty.set("channelList", Json::array());
    result = homeChannels(empty);
    assert(result.tunerAvailable);
    assert(result.channels.isArray() && result.channels.size() == 0);

    reply.set("returnValue", Json::flag(false));
    result = homeChannels(reply);
    assert(!result.tunerAvailable);
    assert(!result.channels.valid());

    std::cout << "Native Home channels passed.\n";
    return 0;
}
