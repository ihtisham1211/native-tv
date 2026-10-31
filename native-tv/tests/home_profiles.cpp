#include "service/home_profiles.hpp"
#include <cassert>
#include <iostream>

using namespace nativetv;
using namespace nativetv::service;

int main() {
    Json document = normalizeHomeDocument(Json::object());
    assert(document["profiles"].size() == 0);
    Json profileList = homeProfileList(document);
    assert(profileList.size() == 1);
    assert(profileList[0]["id"].string() == "main");
    assert(profileList[0]["active"].boolean());
    assert(profileList[0]["name"].string() == "Main");

    // A fresh install's main profile is named after the TV; renaming it keeps the new name.
    Json named = homeProfileList(document, "B3");
    assert(named[0]["name"].string() == "B3");
    Json nameBody = Json::object();
    nameBody.set("action", Json::text("rename"));
    nameBody.set("id", Json::text("main"));
    nameBody.set("name", Json::text("Living room"));
    HomeDocumentUpdate first = updateHomeProfile(document, nameBody, "B3");
    assert(first.response["ok"].boolean() && first.changed);
    assert(first.response["profiles"].size() == 1 &&
           first.response["profiles"][0]["name"].string() == "Living room");
    Json addBody = Json::object();
    addBody.set("action", Json::text("add"));
    HomeDocumentUpdate added2 = updateHomeProfile(document, addBody, "C3");
    assert(added2.response["profiles"][0]["name"].string() == "C3" &&
           added2.response["profiles"][1]["name"].string() == "Profile 2");

    Json layout = Json::object();
    Json hidden = Json::object();
    Json hiddenApps = Json::array();
    hiddenApps.add(Json::text("netflix"));
    hidden.set("app", hiddenApps);
    Json colours = Json::object();
    colours.set("text", Json::text("#F4F4F4"));
    colours.set("accent", Json::text("#8FB8FF"));
    layout.set("hidden", hidden);
    layout.set("appNames", Json::flag(false));
    layout.set("colours", colours);
    HomeDocumentUpdate savedLayout = updateHomeLayout(document, layout);
    assert(savedLayout.response["ok"].boolean());
    assert(savedLayout.changed);
    assert(savedLayout.document["hidden"]["app"][0].string() == "netflix");
    assert(savedLayout.document["colours"]["text"].string() == "#f4f4f4");

    // The Apps tab has its own hidden apps: hiding on the Home row leaves it alone, and the other
    // way round. A layout saved before the list existed starts it from the Home row's list (without
    // the LG Home tile, which has its own switch) and the two are independent from then on.
    assert(savedLayout.document["hidden"]["appsTab"].size() == 0);
    Json pageLayout = Json::object();
    Json pageHidden = Json::object();
    Json pageApps = Json::array();
    pageApps.add(Json::text("spotify"));
    pageHidden.set("appsTab", pageApps);
    pageLayout.set("hidden", pageHidden);
    HomeDocumentUpdate savedPage = updateHomeLayout(savedLayout.document, pageLayout);
    assert(savedPage.response["ok"].boolean() && savedPage.changed);
    assert(savedPage.document["hidden"]["appsTab"].size() == 1 &&
           savedPage.document["hidden"]["appsTab"][0].string() == "spotify");
    assert(savedPage.document["hidden"]["app"].size() == 1 &&
           savedPage.document["hidden"]["app"][0].string() == "netflix");
    assert(savedPage.response["layout"]["hidden"]["appsTab"][0].string() == "spotify");
    Json shownOnHome = Json::object();
    Json shownHidden = Json::object();
    shownHidden.set("app", Json::array());
    shownOnHome.set("hidden", shownHidden);
    HomeDocumentUpdate unhidden = updateHomeLayout(savedPage.document, shownOnHome);
    assert(unhidden.document["hidden"]["app"].size() == 0 &&
           unhidden.document["hidden"]["appsTab"][0].string() == "spotify");
    Json oldDocument = Json::parse(
        R"({"hidden":{"app":["netflix","lg-home","com.webos.app.lgchannels"],"dev":[],"ch":[]}})");
    Json migrated = normalizeHomeDocument(oldDocument);
    assert(migrated["hidden"]["appsTab"].size() == 2 &&
           migrated["hidden"]["appsTab"][0].string() == "netflix" &&
           migrated["hidden"]["appsTab"][1].string() == "com.webos.app.lgchannels" &&
           migrated["hidden"]["app"].size() == 2);
    assert(normalizeHomeDocument(migrated).dump() == migrated.dump());
    HomeDocumentUpdate afterMigration = updateHomeLayout(oldDocument, shownOnHome);
    assert(afterMigration.document["hidden"]["app"].size() == 0 &&
           afterMigration.document["hidden"]["appsTab"].size() == 2);
    Json fresh = normalizeHomeDocument(Json());
    assert(fresh["hidden"]["app"].size() == 5 && fresh["hidden"]["appsTab"].dump() ==
                                                    fresh["hidden"]["app"].dump());

    // The apps row can be grouped by kind (the other rows cannot); the kinds keep the owner's
    // order, an unknown kind drops out and the missing ones follow in their default order.
    assert(document["kinds"].size() == 7 && document["kinds"][0].string() == "video" &&
           document["kinds"][6].string() == "other");
    Json kindLayout = Json::object();
    Json kindSort = Json::object();
    kindSort.set("app", Json::text("kind"));
    kindSort.set("dev", Json::text("kind"));
    kindLayout.set("sort", kindSort);
    Json kinds = Json::array();
    kinds.add(Json::text("games"));
    kinds.add(Json::text("bogus"));
    kinds.add(Json::text("video"));
    kindLayout.set("kinds", kinds);
    HomeDocumentUpdate byKind = updateHomeLayout(document, kindLayout);
    assert(byKind.response["ok"].boolean() && byKind.changed);
    assert(byKind.document["sort"]["app"].string() == "kind");
    assert(byKind.document["sort"]["dev"].string() == "custom");
    assert(byKind.document["kinds"].size() == 7 && byKind.document["kinds"][0].string() == "games" &&
           byKind.document["kinds"][1].string() == "video" &&
           byKind.document["kinds"][2].string() == "music");
    assert(byKind.response["layout"]["kinds"][0].string() == "games");
    Json resetBody = Json::object();
    resetBody.set("reset", Json::flag(true));
    HomeDocumentUpdate afterReset = updateHomeLayout(byKind.document, resetBody);
    assert(afterReset.document["sort"]["app"].string() == "custom" &&
           afterReset.document["kinds"][0].string() == "video");
    HomeDocumentUpdate resetPage = updateHomeLayout(savedPage.document, resetBody);
    assert(resetPage.document["hidden"]["app"].size() == 5 &&
           resetPage.document["hidden"]["appsTab"].dump() == resetPage.document["hidden"]["app"].dump());

    Json add = Json::object();
    add.set("action", Json::text("add"));
    add.set("name", Json::text("  Kids\t room "));
    add.set("color", Json::text("#F2B8B5"));
    HomeDocumentUpdate added = updateHomeProfile(savedLayout.document, add);
    assert(added.response["ok"].boolean());
    assert(added.document["profiles"].size() == 2);
    assert(added.document["profiles"][1]["id"].string() == "p1");
    assert(added.document["profiles"][1]["name"].string() == "Kids room");
    assert(added.document["profiles"][1]["color"].string() == "#f2b8b5");
    assert(added.document["profile"].string() == "main");
    assert(added.document["hidden"]["app"][0].string() == "netflix");

    Json switchProfile = Json::object();
    switchProfile.set("action", Json::text("switch"));
    switchProfile.set("id", Json::text("p1"));
    HomeDocumentUpdate switched = updateHomeProfile(added.document, switchProfile);
    assert(switched.response["ok"].boolean());
    assert(switched.document["profile"].string() == "p1");
    assert(switched.document["appNames"].boolean());
    assert(switched.document["colours"].dump() == "null");
    // A fresh profile has the same defaults in both lists.
    assert(switched.document["hidden"]["app"].size() == 5 &&
           switched.document["hidden"]["appsTab"].dump() == switched.document["hidden"]["app"].dump());

    Json edit = Json::object();
    edit.set("clockFormat", Json::text("12h"));
    Json order = Json::object();
    Json channelOrder = Json::array();
    channelOrder.add(Json::text("c2"));
    channelOrder.add(Json::text("c1"));
    order.set("ch", channelOrder);
    edit.set("order", order);
    Json channels = Json::array();
    channels.add(Json::text("c1"));
    channels.add(Json::text("c2"));
    edit.set("channels", channels);
    HomeDocumentUpdate p1Layout = updateHomeLayout(switched.document, edit);
    assert(p1Layout.response["ok"].boolean());
    assert(p1Layout.document["channels"][0].string() == "c2");

    Json mainSwitch = Json::object();
    mainSwitch.set("action", Json::text("switch"));
    mainSwitch.set("id", Json::text("main"));
    HomeDocumentUpdate back = updateHomeProfile(p1Layout.document, mainSwitch);
    assert(back.response["ok"].boolean());
    assert(back.document["appNames"].boolean() == false);
    assert(back.document["hidden"]["app"][0].string() == "netflix");
    HomeDocumentUpdate forward = updateHomeProfile(back.document, switchProfile);
    assert(forward.document["clockFormat"].string() == "12h");
    assert(forward.document["channels"][0].string() == "c2");

    Json reset = Json::object();
    reset.set("reset", Json::flag(true));
    HomeDocumentUpdate resetLayout = updateHomeLayout(forward.document, reset);
    assert(resetLayout.document["rows"][0].string() == "app");
    assert(!resetLayout.document["showLive"].boolean());
    assert(resetLayout.document["clockFormat"].string() == "12h");
    assert(resetLayout.document["channels"][0].string() == "c2");
    assert(resetLayout.document["profiles"].size() == 2);

    Json rename = Json::object();
    rename.set("action", Json::text("rename"));
    rename.set("id", Json::text("p1"));
    rename.set("name", Json::text("Guest room"));
    rename.set("color", Json::text("#6DD58C"));
    HomeDocumentUpdate renamed = updateHomeProfile(forward.document, rename);
    assert(renamed.response["profiles"][1]["name"].string() == "Guest room");
    assert(renamed.response["profiles"][1]["color"].string() == "#6dd58c");

    Json invalidRename = Json::object();
    invalidRename.set("action", Json::text("rename"));
    invalidRename.set("id", Json::text("p1"));
    invalidRename.set("name", Json::text("This name is too long"));
    invalidRename.set("color", Json::text("red"));
    HomeDocumentUpdate renameRefused = updateHomeProfile(renamed.document, invalidRename);
    assert(!renameRefused.response["ok"].boolean());
    assert(renameRefused.document.dump() == renamed.document.dump());

    Json profileLimit = normalizeHomeDocument(Json::object());
    Json addProfile = Json::object();
    addProfile.set("action", Json::text("add"));
    addProfile.set("name", Json::text("First guest"));
    addProfile.set("switch", Json::text("true"));
    HomeDocumentUpdate firstGuest = updateHomeProfile(profileLimit, addProfile);
    assert(firstGuest.document["profile"].string() == "main");
    assert(firstGuest.document["profiles"][1]["name"].string() == "First guest");
    assert(firstGuest.document["profiles"][1]["id"].string() == "p1");

    Json switchOnAdd = Json::object();
    switchOnAdd.set("action", Json::text("add"));
    switchOnAdd.set("switch", Json::flag(true));
    HomeDocumentUpdate secondGuest = updateHomeProfile(firstGuest.document, switchOnAdd);
    assert(secondGuest.document["profile"].string() == "p2");
    assert(secondGuest.document["profiles"][2]["name"].string() == "Profile 3");
    Json secondLayout = Json::object();
    secondLayout.set("clockFormat", Json::text("12h"));
    HomeDocumentUpdate secondGuestLayout = updateHomeLayout(secondGuest.document, secondLayout);
    HomeDocumentUpdate thirdGuest = updateHomeProfile(secondGuestLayout.document, addProfile);
    HomeDocumentUpdate fourthGuest = updateHomeProfile(thirdGuest.document, addProfile);
    assert(fourthGuest.document["profiles"].size() == 5);
    HomeDocumentUpdate overLimit = updateHomeProfile(fourthGuest.document, addProfile);
    assert(!overLimit.response["ok"].boolean());

    Json removeProfile = Json::object();
    removeProfile.set("action", Json::text("remove"));
    removeProfile.set("id", Json::text("p4"));
    HomeDocumentUpdate removeFourth = updateHomeProfile(fourthGuest.document, removeProfile);
    HomeDocumentUpdate addAgain = updateHomeProfile(removeFourth.document, addProfile);
    assert(addAgain.document["profiles"][4]["id"].string() == "p4");
    removeProfile.set("id", Json::text("p2"));
    HomeDocumentUpdate removeActive = updateHomeProfile(secondGuestLayout.document, removeProfile);
    assert(removeActive.document["profile"].string() == "main");
    assert(removeActive.document["clockFormat"].string() == "24h");

    Json malformed = Json::object();
    Json profiles = Json::array();
    Json profile = Json::object();
    profile.set("id", Json::text("a"));
    profile.set("name", Json::text("First"));
    Json nested = Json::object();
    nested.set("profiles", Json::array());
    nested.set("rows", Json::array());
    profile.set("layout", nested);
    profiles.add(profile);
    profiles.add(profile);
    malformed.set("profile", Json::text("ghost"));
    malformed.set("profiles", profiles);
    Json normalized = normalizeHomeDocument(malformed);
    assert(normalized["profiles"].size() == 1);
    assert(normalized["profile"].string() == "a");
    assert(!normalized["profiles"][0]["layout"].has("profiles"));
    assert(normalized["profiles"][0]["layout"]["rows"][0].string() == "app");

    Json badAction = Json::object();
    badAction.set("action", Json::text("switch"));
    badAction.set("id", Json::text("missing"));
    HomeDocumentUpdate refused = updateHomeProfile(normalized, badAction);
    assert(!refused.response["ok"].boolean());
    assert(refused.document.dump() == normalized.dump());

    std::cout << "Native Home profiles and layouts passed.\n";
    return 0;
}
