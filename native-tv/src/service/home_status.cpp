#include "service/home_status.hpp"
#include "domain/say.hpp"
#include "service/home_profiles.hpp"

namespace nativetv {
namespace service {
namespace {
Json colourPresets() {
    return Json::parse(R"([
      {"id":"warm","text":"#ece7df","accent":"#e9ad55"},
      {"id":"nativetv","text":"#ece7df","accent":"#30de61"},
      {"id":"white","text":"#f4f4f4","accent":"#8fb8ff"},
      {"id":"ice","text":"#d4e6f4","accent":"#5fc4ee"},
      {"id":"rose","text":"#f4dcdc","accent":"#ec7f8c"},
      {"id":"lilac","text":"#e0d9f6","accent":"#a98ff0"}
    ])");
}

Json profileColours() {
    return Json::parse(R"(["#a8c7fa","#f2b8b5","#6dd58c","#ffb74d","#7fcfff","#d0bcff"])");
}
} // namespace

Json homeStatusSupported(const std::string &version, bool allowExperimental) {
    Json result = Json::object();
    result.set("webos", Json::text(version));
    if (version.compare(0, 2, "9.") == 0) {
        result.set("ok", Json::flag(true));
        return result;
    }
    const bool on10 = version.compare(0, 3, "10.") == 0;
    const bool on11 = version.compare(0, 3, "11.") == 0;
    if (on10 || (on11 && allowExperimental)) {
        result.set("ok", Json::flag(true));
        result.set("experimental", Json::flag(true));
        return result;
    }

    result.set("ok", Json::flag(false));
    Json variables = Json::object();
    variables.set("version", Json::text(version.empty() ? "?" : version));
    result.set("webos", version.empty() ? Json() : Json::text(version));
    result.set("reason", Json::text(say::msg(
        "srv.home.untested",
        "Native Tv Home has been tried only on webOS 9 and 10. This TV runs webOS {version}.",
        variables)));
    return result;
}

Json homeStatusProjection(const Json &rawDocument,
                          const std::string &webosVersion,
                          bool allowExperimental, bool active,
                          bool lgHomeOpen, bool historyEnabled,
                          const Json &wallpapers) {
    const Json document = normalizeHomeDocument(rawDocument);
    const Json supported = homeStatusSupported(webosVersion, allowExperimental);
    Json result = Json::object();
    result.set("ok", Json::flag(true));
    result.set("available", Json::flag(supported["ok"].boolean() ||
                                         document["enabled"].boolean() ||
                                         document["restorePending"].boolean()));
    result.set("supported", supported);
    result.set("enabled", document["enabled"]);
    result.set("openOnPowerOn", document["openOnPowerOn"]);
    result.set("restorePending", document["restorePending"]);
    result.set("active", Json::flag(active));
    result.set("lgHomeOpen", Json::flag(lgHomeOpen));
    result.set("hideLgHomeAtStart",
               Json::flag(document["hideLgHomeAtStart"].isBoolean() &&
                          document["hideLgHomeAtStart"].boolean()));
    result.set("hideLgHomeAtStartAvailable",
               Json::flag(webosVersion.compare(0, 2, "9.") == 0));
    result.set("history", Json::flag(historyEnabled));
    result.set("wallpapers", wallpapers);
    result.set("colourPresets", colourPresets());
    result.set("profiles", homeProfileList(document));
    result.set("profile", document["profile"]);
    result.set("profileColours", profileColours());
    result.set("layout", homeLayout(document));
    return result;
}

} // namespace service
} // namespace nativetv
