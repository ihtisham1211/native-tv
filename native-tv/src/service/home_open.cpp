#include "service/home_open.hpp"
#include "domain/names.hpp"
#include "domain/say.hpp"
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <sstream>

namespace nativetv {
namespace service {
namespace {

const char *const kLgHomeTile = "lg-home";
const char *const kYouTubeApp = "youtube.leanback.v4";
const char *const kNetflixApp = "netflix";

Json findListed(const Json &list, const std::string &id) {
    if (!list.isArray()) return Json();
    for (const Json &item : list.items())
        if (item.isObject() && item["id"].string() == id) return item;
    return Json();
}

bool validYouTubeId(const std::string &id) {
    if (id.size() != 11) return false;
    for (unsigned char character : id)
        if (!((character >= 'A' && character <= 'Z') ||
              (character >= 'a' && character <= 'z') ||
              (character >= '0' && character <= '9') || character == '_' ||
              character == '-'))
            return false;
    return true;
}

bool validNetflixId(const std::string &id) {
    if (id.size() < 5 || id.size() > 12 || id[0] < '1' || id[0] > '9')
        return false;
    for (size_t index = 1; index < id.size(); ++index)
        if (id[index] < '0' || id[index] > '9') return false;
    return true;
}

void setLaunch(HomeOpenPlan &plan, const std::string &appId) {
    plan.ok = true;
    plan.error.clear();
    plan.action = "launch";
    plan.appId = appId;
    plan.row = "app";
    plan.rowId = appId;
}
} // namespace

bool homeOpenHandledKind(const std::string &kind, const std::string &id) {
    return kind != "lg" && !(kind == "app" && id == kLgHomeTile);
}

HomeOpenPlan planHomeOpen(const std::string &kind, const std::string &id,
                          const Json &apps, const Json &devices,
                          const Json &channels) {
    HomeOpenPlan plan;
    plan.handled = homeOpenHandledKind(kind, id);
    if (!plan.handled) return plan;
    plan.error = say::msg("srv.home.unknownTile",
                          "that is not on Native Tv Home");

    if (kind == "app") {
        const Json hit = findListed(apps, id);
        if (!hit.valid()) return plan;
        setLaunch(plan, id);
        return plan;
    }
    if (kind == "dev") {
        const Json hit = findListed(devices, id);
        if (!hit.valid()) return plan;
        plan.ok = true;
        plan.error.clear();
        plan.action = "launch";
        plan.appId = names::inputAppId(hit["input"].string());
        plan.row = "dev";
        plan.rowId = id;
        return plan;
    }
    if (kind == "ch") {
        const Json hit = findListed(channels, id);
        if (!hit.valid()) return plan;
        plan.ok = true;
        plan.error.clear();
        plan.action = "channel";
        plan.channelId = id;
        plan.row = "ch";
        plan.rowId = id;
        return plan;
    }
    if (kind == "youtube") {
        if (!validYouTubeId(id)) return plan;
        if (!findListed(apps, kYouTubeApp).valid()) {
            plan.error = say::msg("srv.home.noYoutube",
                                  "YouTube is not installed on this TV");
            return plan;
        }
        setLaunch(plan, kYouTubeApp);
        plan.launchParams = Json::object();
        plan.launchParams.set(
            "contentTarget",
            Json::text("https://www.youtube.com/tv?v=" + id));
        return plan;
    }
    if (kind == "netflix") {
        if (!findListed(apps, kNetflixApp).valid()) {
            plan.error = say::msg("srv.home.noNetflix",
                                  "Netflix is not installed on this TV");
            return plan;
        }
        plan.ok = true;
        plan.error.clear();
        plan.action = "netflix";
        plan.appId = kNetflixApp;
        plan.row = "app";
        plan.rowId = kNetflixApp;
        return plan;
    }
    plan.error = "kind must be app, dev, ch or lg";
    return plan;
}

void resolveHomeNetflixOpen(HomeOpenPlan &plan, const Json &lookup) {
    if (!plan.handled || !plan.ok || plan.action != "netflix") return;
    const std::string netflixId = lookup["netflixId"].string();
    if (!lookup["ok"].boolean() || !lookup["enabled"].boolean() ||
        !validNetflixId(netflixId)) {
        plan.ok = false;
        plan.error = say::msg("srv.home.noNetflixLink",
                              "No Netflix link is available for this title");
        plan.action.clear();
        return;
    }
    plan.action = "launch";
    plan.launchParams = Json::object();
    plan.launchParams.set(
        "contentTarget",
        Json::text("m=https://www.netflix.com/title/" + netflixId +
                   "&source_type=4"));
}

bool homeOpenStampWanted(const Json &document, const std::string &row) {
    return document["enabled"].boolean() &&
           document["sort"][row].string() == "recent";
}

int64_t homeTileOpenWindowMilliseconds() { return 15000; }

bool homeTileInputFresh(int64_t now, int64_t tileAt) {
    return tileAt > 0 && now >= tileAt &&
           now - tileAt < homeTileOpenWindowMilliseconds();
}

} // namespace service
} // namespace nativetv
