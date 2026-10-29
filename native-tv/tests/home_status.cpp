#include "service/home_status.hpp"
#include <cassert>

using nativetv::Json;
using nativetv::service::homeStatusProjection;
using nativetv::service::homeStatusSupported;

int main() {
    Json support = homeStatusSupported("9.2.2", false);
    assert(support["ok"].boolean());
    assert(support["webos"].string() == "9.2.2");
    assert(!support["experimental"].valid());

    support = homeStatusSupported("10.3.1", false);
    assert(support["ok"].boolean() && support["experimental"].boolean());
    support = homeStatusSupported("11.0.0", false);
    assert(!support["ok"].boolean());
    support = homeStatusSupported("11.0.0", true);
    assert(support["ok"].boolean() && support["experimental"].boolean());
    support = homeStatusSupported("10.3.1", false);
    assert(support["ok"].boolean() && support["experimental"].boolean());
    support = homeStatusSupported("", false);
    assert(!support["webos"].valid());

    const Json document = Json::parse(R"({
      "enabled":true,
      "restorePending":true,
      "openOnPowerOn":true,
      "hideLgHomeAtStart":true,
      "profile":"family",
      "profiles":[{
        "id":"family","name":"Family","color":"#6dd58c",
        "layout":{"rows":["app","dev"]}
      }]
    })");
    const Json wallpapers = Json::parse(
        R"({"available":true,"uploading":false,"revision":"1"})");
    const Json status = homeStatusProjection(document, "9.2.2", false,
                                              true, false, true, wallpapers);
    assert(status["ok"].boolean());
    assert(status["available"].boolean());
    assert(status["supported"]["ok"].boolean());
    assert(status["enabled"].boolean());
    assert(status["restorePending"].boolean());
    assert(status["openOnPowerOn"].boolean());
    assert(status["active"].boolean());
    assert(!status["lgHomeOpen"].boolean());
    assert(status["hideLgHomeAtStart"].boolean());
    assert(status["hideLgHomeAtStartAvailable"].boolean());
    assert(status["history"].boolean());
    assert(status["wallpapers"]["available"].boolean());
    assert(status["colourPresets"].size() == 6);
    assert(status["profiles"].size() == 1);
    assert(status["profiles"][0]["active"].boolean());
    assert(status["profile"].string() == "family");
    assert(status["profileColours"].size() == 6);
    assert(status["layout"]["rows"].size() == 3);

    const Json unavailable = homeStatusProjection(
        Json::object(), "12.0.0", false, false, false, false,
        Json::object());
    assert(!unavailable["available"].boolean());
    return 0;
}
