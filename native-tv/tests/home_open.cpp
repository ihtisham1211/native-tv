#include "service/home_open.hpp"
#include <cassert>
#include <iostream>

using namespace nativetv;
using namespace nativetv::service;

namespace {

Json app(const std::string &id) {
    Json item = Json::object();
    item.set("id", Json::text(id));
    item.set("title", Json::text(id));
    return item;
}

Json device(const std::string &id, const std::string &input, bool unused = false) {
    Json item = Json::object();
    item.set("id", Json::text(id));
    item.set("name", Json::text(id));
    item.set("input", Json::text(input));
    item.set("port", Json::numeric(2));
    if (unused) item.set("unused", Json::flag(true));
    return item;
}

Json channel(const std::string &id) {
    Json item = Json::object();
    item.set("id", Json::text(id));
    item.set("number", Json::text("1"));
    item.set("name", Json::text(id));
    return item;
}

} // namespace

int main() {
    Json apps = Json::array();
    apps.add(app("com.webos.app.netflix"));
    apps.add(app("netflix"));
    apps.add(app("youtube.leanback.v4"));
    apps.add(app("com.animemania.app"));
    apps.add(app("com.webos.app.notificationcenter"));
    Json devices = Json::array();
    devices.add(device("hdmi2", "hdmi2"));
    devices.add(device("hdmi2:game-console", "hdmi2"));
    Json channels = Json::array();
    channels.add(channel("channel-1"));

    // An app tile launches its launch point and stamps it as opened.
    HomeOpenPlan plan = planHomeOpen("app", "com.webos.app.netflix", apps,
                                     devices, channels);
    assert(plan.handled && plan.ok);
    assert(plan.action == "launch");
    assert(plan.appId == "com.webos.app.netflix");
    assert(plan.row == "app" && plan.rowId == "com.webos.app.netflix");
    assert(plan.error.empty());

    // Only what the catalogue lists can be opened.
    plan = planHomeOpen("app", "com.example.unknown", apps, devices, channels);
    assert(plan.handled && !plan.ok);
    assert(plan.error == "that is not on Native Tv Home");

    // An app tile for LG's Home escape stays on Node.
    plan = planHomeOpen("app", "lg-home", apps, devices, channels);
    assert(!plan.handled);

    // A device opens its HDMI input, stamping the tile's own id.
    plan = planHomeOpen("dev", "hdmi2:game-console", apps, devices, channels);
    assert(plan.handled && plan.ok);
    assert(plan.action == "launch");
    assert(plan.appId == "com.webos.app.hdmi2");
    assert(plan.row == "dev" && plan.rowId == "hdmi2:game-console");

    plan = planHomeOpen("dev", "hdmi2", apps, devices, channels);
    assert(plan.handled && plan.ok);
    assert(plan.appId == "com.webos.app.hdmi2");
    assert(plan.rowId == "hdmi2");

    plan = planHomeOpen("dev", "hdmi3", apps, devices, channels);
    assert(plan.handled && !plan.ok);

    // A channel tunes.
    plan = planHomeOpen("ch", "channel-1", apps, devices, channels);
    assert(plan.handled && plan.ok);
    assert(plan.action == "channel");
    assert(plan.channelId == "channel-1");
    assert(plan.row == "ch" && plan.rowId == "channel-1");

    plan = planHomeOpen("ch", "channel-9", apps, devices, channels);
    assert(plan.handled && !plan.ok);

    // A feed title is not opened in any other app: only YouTube and Netflix have deep links.
    plan = planHomeOpen("title", "42", apps, devices, channels);
    assert(plan.handled && !plan.ok && plan.action.empty());
    assert(plan.error == "kind must be app, dev, ch or lg");

    // YouTube IDs are constrained before constructing the player URL.
    plan = planHomeOpen("youtube", "abcdefghijk", apps, devices, channels);
    assert(plan.handled && plan.ok && plan.appId == "youtube.leanback.v4");
    assert(plan.launchParams["contentTarget"].string() ==
           "https://www.youtube.com/tv?v=abcdefghijk");
    plan = planHomeOpen("youtube", "bad/video!!", apps, devices, channels);
    assert(plan.handled && !plan.ok);

    // Netflix resolves the native Wikidata/P1874 result before it launches.
    plan = planHomeOpen("netflix", "42", apps, devices, channels);
    assert(plan.handled && plan.ok && plan.action == "netflix");
    Json netflixLookup = Json::object();
    netflixLookup.set("ok", Json::flag(true));
    netflixLookup.set("enabled", Json::flag(true));
    netflixLookup.set("netflixId", Json::text("80123456"));
    resolveHomeNetflixOpen(plan, netflixLookup);
    assert(plan.ok && plan.action == "launch");
    assert(plan.appId == "netflix");
    assert(plan.launchParams["contentTarget"].string() ==
           "m=https://www.netflix.com/title/80123456&source_type=4");
    netflixLookup.set("netflixId", Json::text("not-an-id"));
    plan = planHomeOpen("netflix", "42", apps, devices, channels);
    resolveHomeNetflixOpen(plan, netflixLookup);
    assert(!plan.ok && plan.error ==
           "No Netflix link is available for this title");

    // LG's Home escape stays on Node. Unknown kinds are refused natively.
    const char *const forwarded[] = {"lg"};
    for (const char *kind : forwarded) {
        plan = planHomeOpen(kind, "anything", apps, devices, channels);
        assert(!plan.handled);
        assert(!plan.ok);
    }
    plan = planHomeOpen("app", "lg-home", apps, devices, channels);
    assert(!plan.handled);
    plan = planHomeOpen("tv", "anything", apps, devices, channels);
    assert(plan.handled && !plan.ok);
    assert(plan.error == "kind must be app, dev, ch or lg");

    // Missing or unusable sources refuse like an empty list does.
    plan = planHomeOpen("app", "com.webos.app.netflix", Json(), devices, channels);
    assert(plan.handled && !plan.ok);
    plan = planHomeOpen("dev", "hdmi2", apps, Json::parse("null"), channels);
    assert(plan.handled && !plan.ok);
    plan = planHomeOpen("ch", "channel-1", apps, devices, Json());
    assert(plan.handled && !plan.ok);

    // An opened row is recorded only while Native Tv Home is on and the row
    // sorts by Last opened.
    Json document = Json::object();
    document.set("enabled", Json::flag(true));
    Json sort = Json::object();
    sort.set("app", Json::text("recent"));
    sort.set("dev", Json::text("manual"));
    sort.set("ch", Json::text("recent"));
    document.set("sort", sort);
    assert(homeOpenStampWanted(document, "app"));
    assert(!homeOpenStampWanted(document, "dev"));
    assert(homeOpenStampWanted(document, "ch"));
    document.set("enabled", Json::flag(false));
    assert(!homeOpenStampWanted(document, "app"));

    // The input arriving in front within the window is a device tile, not the
    // remote.
    const int64_t window = homeTileOpenWindowMilliseconds();
    assert(homeTileInputFresh(5000, 0) == false);
    assert(homeTileInputFresh(5000, 5000));
    assert(homeTileInputFresh(5000 + window - 1, 5000));
    assert(!homeTileInputFresh(5000 + window, 5000));
    assert(!homeTileInputFresh(4000, 5000));

    std::cout << "Native Home open passed.\n";
    return 0;
}
