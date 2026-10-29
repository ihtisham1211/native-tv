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
