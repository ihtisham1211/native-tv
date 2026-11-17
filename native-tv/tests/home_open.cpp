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
    assert(!plan.launchParams.valid()); // no id from the TV: the port opens alone

    plan = planHomeOpen("dev", "hdmi2", apps, devices, channels);
    assert(plan.handled && plan.ok);
    assert(plan.appId == "com.webos.app.hdmi2");
    assert(plan.rowId == "hdmi2");

    plan = planHomeOpen("dev", "hdmi3", apps, devices, channels);
    assert(plan.handled && !plan.ok);

    // A device with a CEC id has LG's HDMI app select it, as LG's own input picker does: a
    // receiver and a console on one port each get their own. Without a usable id (none,
    // -1 for none, 0 the TV, 15 broadcast, beyond that, not a whole number) the port opens
    // alone.
    const struct { const char *id; double uniqueId; const char *selected; } cecCases[] = {
        {"hdmi2:av-receiver", 5, "5"}, {"hdmi2:game-console", 8, "8"}, {"hdmi2:game-console", 14, "14"},
        {"hdmi2:game-console", -1, nullptr}, {"hdmi2:game-console", 0, nullptr},
        {"hdmi2:game-console", 15, nullptr}, {"hdmi2:game-console", 255, nullptr},
        {"hdmi2:game-console", 4.5, nullptr}};
    for (const auto &cecCase : cecCases) {
        Json withId = Json::array();
        Json entry = device(cecCase.id, "hdmi2");
        entry.set("uniqueId", Json::numeric(cecCase.uniqueId));
        withId.add(entry);
        plan = planHomeOpen("dev", cecCase.id, apps, withId, channels);
        assert(plan.handled && plan.ok && plan.appId == "com.webos.app.hdmi2");
        if (cecCase.selected) {
            assert(plan.launchParams.isObject());
            assert(plan.launchParams["id"].string() == "uniqueId");
            assert(plan.launchParams["value"].string() == cecCase.selected);
        } else {
            assert(!plan.launchParams.valid());
        }
    }
    Json notANumber = Json::array();
    Json textual = device("hdmi2:game-console", "hdmi2");
    textual.set("uniqueId", Json::text("8"));
    notANumber.add(textual);
    plan = planHomeOpen("dev", "hdmi2:game-console", apps, notANumber, channels);
    assert(plan.ok && !plan.launchParams.valid());

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

    std::cout << "Native Home open passed.\n";
    return 0;
}
