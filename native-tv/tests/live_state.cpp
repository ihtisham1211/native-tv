#include "domain/live_state.hpp"
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using nativetv::Json;
using nativetv::state::Cache;
using nativetv::state::Event;
using nativetv::state::MappingOptions;

namespace {
void require(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}

Json parse(const std::string &value) {
    Json result = Json::parse(value);
    require(result.valid(), "invalid test JSON: " + value);
    return result;
}

void testCache() {
    Cache cache;
    std::vector<Event> changes;
    cache.onChange([&changes](const Event &event) { changes.push_back(event); });
    cache.onChange([](const Event &) { throw std::runtime_error("consumer failure"); });

    Event first{"application", "app_id", Json::text("netflix"), 100, true, false};
    require(cache.update(first), "first value should be accepted");
    require(changes.size() == 1 && changes[0].snapshot,
            "listeners receive first snapshot metadata");

    Event same{"application", "app_id", Json::text("netflix"), 200, false, true};
    require(!cache.update(same), "same scalar should not produce a change");
    Event stale{"application", "app_id", Json::text("hdmi1"), 199, false, true};
    require(!cache.update(stale), "stale source time should be rejected");
    Event changed{"application", "app_id", Json::text("hdmi2"), 201, false, true};
    require(cache.update(changed), "newer changed scalar should be accepted");
    require(changes.size() == 2 && changes[1].tvReported && !changes[1].snapshot,
            "change metadata should be preserved");

    Event numericInt{"picture", "backlight", Json::numeric(50), 300, false, false};
    Event numericFloat{"picture", "backlight", parse("50.0"), 301, false, false};
    require(cache.update(numericInt), "first numeric value should be accepted");
    const size_t beforeSameNumber = changes.size();
    require(!cache.update(numericFloat), "equivalent JSON numbers should compare equally");
    require(changes.size() == beforeSameNumber, "equal values should not notify listeners");

    Json snapshot = cache.snapshot();
    require(snapshot["application"]["app_id"].string() == "hdmi2",
            "snapshot should contain the latest application state");
    require(snapshot["picture"]["backlight"].number() == 50,
            "snapshot should include values from other groups");
}

void testPowerAndApplicationMapping() {
    Json power = nativetv::state::mapSubscription(
        "power", parse(R"({"returnValue":true,"state":"Screen Off"})"));
    require(power["state"].string() == "Screen Off" &&
                !power["screenOn"].boolean() && power["systemOn"].boolean(),
            "power state should preserve panel and system status");
    require(!nativetv::state::mapSubscription(
                 "power", parse(R"({"returnValue":true,"subscribed":true})"))
                 .valid(),
            "a power snapshot without a state must not imply standby");
    require(!nativetv::state::mapSubscription(
                 "power", parse(R"({"returnValue":false,"state":"Active"})"))
                 .valid(),
            "refused power replies must be ignored");

    MappingOptions options;
    options.inputAliases = parse(R"({"hdmi1_alias":"hdmi1"})");
    options.inputNames = parse(R"({"hdmi1":"Apple TV"})");
    Json app = nativetv::state::mapSubscription(
        "application", parse(R"({"returnValue":true,"appId":"com.webos.app.hdmi1_alias"})"),
        options);
    require(app["app_id"].string() == "com.webos.app.hdmi1_alias" &&
                app["app"].string() == "hdmi1" &&
                app["display_title"].string() == "Apple TV (HDMI1)",
            "application mapping should keep the full id and resolve input alias and label");
}

void testSurfaceAndFocusMapping() {
    Json surface = nativetv::state::mapSubscription(
        "surface", parse(R"({"returnValue":true,"foregroundAppInfo":[
          {"appId":"com.webos.app.hdmi1","displayId":0,"windowType":"_WEBOS_WINDOW_TYPE_CARD"},
          {"appId":"com.palm.app.settings","displayId":0,"windowType":"_WEBOS_WINDOW_TYPE_OVERLAY"},
          {"appId":"com.palm.app.secondary","displayId":1,"windowType":"_WEBOS_WINDOW_TYPE_OVERLAY"},
          {"appId":"com.palm.app.null-display","displayId":null,"windowType":"_WEBOS_WINDOW_TYPE_OVERLAY"},
          {"appId":"com.webos.app.other","displayId":1,"windowType":"_WEBOS_WINDOW_TYPE_OVERLAY"}]})"));
    require(surface["control_app_id"].string() == "com.palm.app.settings",
            "primary display overlay should take control precedence");

    Json home = nativetv::state::mapSubscription(
        "surface", parse(R"({"foregroundAppInfo":[
          {"appId":"com.webos.app.hdmi1","displayId":0,"windowType":"_WEBOS_WINDOW_TYPE_CARD"},
          {"appId":"com.webos.app.home","displayId":0,"windowType":"_WEBOS_WINDOW_TYPE_HOME"}]})"));
    require(home["control_app_id"].string() == "com.webos.app.home",
            "home window should take precedence over the card");
    Json empty = nativetv::state::mapSubscription(
        "surface", parse(R"({"foregroundAppInfo":[]})"));
    require(empty["control_app_id"].string().empty(),
            "an empty surface list should map to an empty control app id");

    Json focus = nativetv::state::mapSubscription(
        "focus", parse(R"({"focusedWindowInfo":[
          {"appId":"com.webos.app.quicksettings","displayId":0},
          {"appId":"com.webos.app.other","displayId":1},
          {"appId":"com.webos.app.netflix"}]})"));
    require(focus["control_app_id"].string() == "com.webos.app.netflix",
            "focus should use the last primary or unspecified display entry");
}

void testPictureAndAudioMapping() {
    Json picture = nativetv::state::mapSubscription(
        "picture", parse(R"({"returnValue":true,"settings":{"backlight":"72","pictureMode":"cinema","absent":null}})"));
    require(picture["backlight"].string() == "72" &&
                picture["pictureMode"].string() == "cinema" && !picture.has("absent"),
            "picture subscription should expose the settings values");
    require(!nativetv::state::mapSubscription(
                 "picture", parse(R"({"returnValue":true})"))
                 .valid(),
            "missing picture settings should not synthesize values");

    Json nestedAudio = nativetv::state::mapSubscription(
        "audio", parse(R"({"returnValue":true,"volumeStatus":{"volume":18,"muteStatus":false,"soundOutput":"external_arc"}})"));
    require(nestedAudio["volume"].number() == 18 &&
                !nestedAudio["muted"].boolean() &&
                nestedAudio["output"].string() == "HDMI ARC",
            "new audio service values should use the nested shape");
    Json legacyAudio = nativetv::state::mapSubscription(
        "audio", parse(R"({"returnValue":true,"volume":12,"muted":true,"scenario":"mastervolume_tv_speaker"})"));
    require(legacyAudio["volume"].number() == 12 && legacyAudio["muted"].boolean() &&
                legacyAudio["output"].string() == "TV Speaker",
            "legacy volume service values should retain their flat shape");
    Json nullMute = nativetv::state::mapSubscription(
        "audio", parse(R"({"returnValue":true,"muted":null})"));
    require(nullMute["muted"].isBoolean() && !nullMute["muted"].boolean(),
            "a present null legacy mute value should retain JavaScript truthiness");
    Json invalidLevel = nativetv::state::mapSubscription(
        "audio", parse(R"({"returnValue":true,"volumeStatus":{"volume":-1,"soundOutput":"external_arc"}})"));
    require(!invalidLevel.has("volume") && invalidLevel["output"].string() == "HDMI ARC",
            "negative volume should be omitted while other sound values remain");
}

void testMediaMapping() {
    Json acb = nativetv::state::mapSubscription(
        "media", parse(R"({"returnValue":true,"acbs":[
          {"playStateNow":"playing","playerType":"MSE","isFullScreen":false}]})"));
    Json active = parse(acb["snapshot"].string());
    require(active["state"].string() == "playing" &&
                active["playerType"].string() == "MSE" &&
                !active["fullScreen"].boolean(),
            "ACB playback state should preserve player type and fullscreen metadata");

    Json media = nativetv::state::mapSubscription(
        "media", parse(R"({"returnValue":true,"foregroundAppInfo":[
          {"appId":"youtube.leanback.v4","playState":"paused","type":"media"}]})"));
    Json paused = parse(media["snapshot"].string());
    require(paused["state"].string() == "paused" &&
                paused["playerType"].string() == "media" &&
                paused["fullScreen"].boolean(),
            "legacy media playback state should use its type and full-screen defaults");

    Json idle = nativetv::state::mapSubscription(
        "media", parse(R"({"returnValue":true,"acbs":[]})"));
    Json idleValue = parse(idle["snapshot"].string());
    require(idleValue["state"].string() == "idle" &&
                !idleValue["fullScreen"].boolean(),
            "an answered empty player list should map to idle");
    require(idle["snapshot"].string().find("\"playerType\":null") != std::string::npos,
            "an idle media snapshot should explicitly preserve the null player type");
}
} // namespace

int main() {
    try {
        testCache();
        testPowerAndApplicationMapping();
        testSurfaceAndFocusMapping();
        testPictureAndAudioMapping();
        testMediaMapping();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "native live-state tests passed\n";
    return EXIT_SUCCESS;
}
