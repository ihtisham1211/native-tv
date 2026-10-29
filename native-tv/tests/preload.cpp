#include "service/preload.hpp"
#include <cstdlib>
#include <iostream>
#include <string>

using namespace nativetv;
using namespace nativetv::service;

namespace {
void check(bool condition, const std::string &message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

Json entry(const std::string &id, bool enabled, int memory) {
    Json result = Json::object();
    result.set("id", Json::text(id));
    result.set("isEnabled", Json::flag(enabled));
    result.set("requiredMemory", Json::numeric(memory));
    return result;
}

Json list() {
    Json result = Json::array();
    result.add(entry("com.webos.app.home", true, 40));
    result.add(entry("com.webos.app.hdmi1", true, 40));
    result.add(entry("netflix", true, 100));
    result.add(entry("airplay", false, 50));
    result.add(entry("com.webos.app.browser", true, 72));
    result.add(entry("com.webos.app.tips", true, 55));
    return result;
}

void testEssentialAppsAndJavaScriptValues() {
    check(preloadEssentialApp("com.webos.app.home"), "Home is essential");
    check(preloadEssentialApp("com.webos.app.hdmi"), "un-numbered HDMI is essential");
    check(preloadEssentialApp("com.webos.app.hdmi12"), "numbered HDMI is essential");
    check(preloadEssentialApp("com.webos.app.miracast-overlay"),
          "screen sharing overlay is essential");
    check(preloadEssentialApp("airplay"), "AirPlay is essential");
    check(!preloadEssentialApp("com.webos.app.hdmiX"),
          "non-numeric HDMI suffix is not essential");
    check(!preloadEssentialApp("netflix"), "third-party app is not essential");

    check(!javascriptTruthy(Json()), "missing and null values are false");
    check(!javascriptTruthy(Json::flag(false)), "false is false");
    check(!javascriptTruthy(Json::numeric(0)), "zero is false");
    check(!javascriptTruthy(Json::text("")), "empty string is false");
    check(javascriptTruthy(Json::array()), "arrays are true");
    check(javascriptTruthy(Json::object()), "objects are true");
    check(javascriptString(Json::numeric(12)) == "12", "numbers use JS string values");
    check(javascriptString(Json::object()) == "[object Object]",
          "objects use the JS object string value");
}

void testChangeAndRestore() {
    Json updatedState;
    Json updatedList;
    bool enabled = true;
    Json initialState = Json::object();
    Json legacyOff = Json::array();
    legacyOff.add(Json::numeric(7));
    legacyOff.add(Json::text("other-app"));
    initialState.set("off", legacyOff);
    check(preloadChange(initialState, list(), "netflix", true, updatedState,
                        updatedList, enabled),
          "an installed preload entry can be changed");
    check(!enabled, "turning preload off sends false to the live policy");
    check(updatedState["off"].size() == 3 &&
              updatedState["off"][0].number() == 7 &&
              updatedState["off"][1].string() == "other-app" &&
              updatedState["off"][2].string() == "netflix",
          "the owner's off list preserves old entries and records the app");
    check(updatedState["lg"].size() == list().size(),
          "the first change preserves LG's original list");
    check(updatedList[2]["isEnabled"].isBoolean() &&
              !updatedList[2]["isEnabled"].boolean(),
          "configd receives the app disabled");
    check(list()[2]["isEnabled"].boolean() &&
              updatedState["lg"][2]["isEnabled"].boolean(),
          "changing the config list leaves LG's saved baseline unchanged");
    check(!updatedList[3]["isEnabled"].boolean(),
          "LG's existing disabled value remains disabled");
    check(updatedList[4]["isEnabled"].boolean(),
          "other apps keep their original settings");

    Json restoredState;
    Json restoredList;
    check(preloadChange(updatedState, updatedList, "netflix", false,
                        restoredState, restoredList, enabled),
          "an owner-disabled app can be restored");
    check(enabled, "restoring uses LG's original enabled value");
    check(restoredState["off"].size() == 2 &&
              restoredState["off"][0].number() == 7 &&
              restoredState["off"][1].string() == "other-app",
          "restoring removes the owner override and preserves old entries");
    check(restoredList[2]["isEnabled"].boolean(),
          "configd receives LG's original setting");

    Json lgDisabled = Json::array();
    lgDisabled.add(entry("com.webos.app.home", true, 40));
    lgDisabled.add(entry("com.webos.app.hdmi1", true, 40));
    lgDisabled.add(entry("netflix", false, 100));
    lgDisabled.add(entry("airplay", false, 50));
    lgDisabled.add(entry("com.webos.app.browser", true, 72));
    lgDisabled.add(entry("com.webos.app.tips", true, 55));
    Json baseline = Json::object();
    baseline.set("off", Json::array());
    baseline.set("lg", lgDisabled);
    Json offState;
    Json offList;
    check(preloadChange(baseline, lgDisabled, "netflix", true,
                        offState, offList, enabled),
          "an app disabled by LG may be temporarily owned");
    Json onState;
    Json onList;
    check(preloadChange(offState, offList, "netflix", false,
                        onState, onList, enabled),
          "an owner override may be removed");
    check(!enabled && !onList[2]["isEnabled"].boolean(),
          "removing the override restores LG's disabled setting");
}

void testOfferProjection() {
    Json state = Json::object();
    Json off = Json::array();
    off.add(Json::text("netflix"));
    state.set("off", off);
    state.set("lg", list());

    Json installed = Json::array();
    const char *ids[] = {"com.webos.app.home", "netflix", "airplay",
                         "com.webos.app.browser", "com.webos.app.tips"};
    for (const char *id : ids) {
        Json app = Json::object();
        app.set("id", Json::text(id));
        app.set("title", Json::text(std::string(id) == "netflix"
                                        ? "Netflix"
                                        : std::string(id) == "com.webos.app.browser"
                                              ? "Browser"
                                              : std::string(id) == "com.webos.app.tips"
                                                    ? "Tips" : id));
        installed.add(app);
    }
    Json running = Json::array();
    Json netflix = Json::object();
    netflix.set("id", Json::text("netflix"));
    running.add(netflix);

    const Json apps = preloadApps(list(), installed, running, state);
    check(apps.size() == 3, "only installed, LG-preloaded, non-essential apps show");
    check(apps[0]["id"].string() == "com.webos.app.browser" &&
              apps[1]["id"].string() == "netflix" &&
              apps[2]["id"].string() == "com.webos.app.tips",
          "offered apps sort by title");
    check(apps[1]["off"].boolean() && apps[1]["loaded"].boolean(),
          "owner and live state are projected");
    check(apps[1]["memoryMb"].number() == 100,
          "LG requiredMemory is exposed as memoryMb");
    check(preloadApps(list(), Json::array(), running, state).size() == 0,
          "uninstalled apps are never offered");
}

void testLunaPayloads() {
    const Json configs = preloadConfigsRequest(list());
    check(configs["configs"][preloadConfigKey()].size() == list().size(),
          "setConfigs uses LG's existing config key and list");
    const Json policy = preloadPolicyRequest("netflix", false);
    check(policy["application"]["id"].string() == "netflix" &&
              !policy["application"]["isEnabled"].boolean(),
          "live policy uses the application request shape");
}
} // namespace

int main() {
    testEssentialAppsAndJavaScriptValues();
    testChangeAndRestore();
    testOfferProjection();
    testLunaPayloads();
    std::cout << "native-preload: all tests passed\n";
    return 0;
}
