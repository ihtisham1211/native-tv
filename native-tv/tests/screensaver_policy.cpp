#include "service/screensaver_policy.hpp"
#include <cstdio>
#include <stdexcept>

using namespace nativetv;
using namespace nativetv::service;

namespace {
void check(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}

ScreensaverState flutterTv(int major) {
    ScreensaverState state;
    state.stockType = "flutter";
    state.webosMajor = major;
    state.assets = {"clock", "starfield"};
    return state;
}

Json modeById(const Json &list, const std::string &id) {
    for (const Json &mode : list["modes"].items())
        if (mode["id"].string() == id) return mode;
    return Json();
}
} // namespace

int main() {
    try {
        // QML stock runner: nothing is held back and a switch needs no reread.
        ScreensaverState qml;
        qml.stockType = "qml";
        qml.webosMajor = 9;
        qml.assets = {"clock"};
        check(!screensaverSlowSwitch(qml), "qml stock is a fast switch");
        check(!screensaverHeldBack(qml), "qml stock is never held back");
        check(!screensaverExperimental(qml), "qml stock is not experimental");

        // Unknown stock type (manifest never seen) behaves like a fast switch.
        check(!screensaverSlowSwitch(ScreensaverState()), "unknown stock type");

        // Flutter on webOS 10 and 11 is on by default; 12 stays held back.
        check(screensaverOnByDefault(flutterTv(10)), "webOS 10 on by default");
        check(screensaverOnByDefault(flutterTv(11)), "webOS 11 on by default");
        check(!screensaverHeldBack(flutterTv(10)), "webOS 10 not held");
        check(screensaverHeldBack(flutterTv(12)), "webOS 12 held back");
        check(screensaverExperimental(flutterTv(12)), "webOS 12 experimental");
        ScreensaverState allowed = flutterTv(12);
        allowed.allowedAnyway = true;
        check(!screensaverHeldBack(allowed), "allowed anyway lifts the hold");

        // The list projection.
        const Json held = screensaverList(flutterTv(12));
        check(held["held"].boolean() && held["available"].boolean(), "held list");
        check(!held["heldOverridden"].boolean(), "not overridden");
        check(modeById(held, "stock")["available"].boolean(), "stock always available");
        check(!modeById(held, "clock")["available"].boolean(), "held hides customs");
        const Json overridden = screensaverList(allowed);
        check(overridden["heldOverridden"].boolean(), "overridden list");
        check(modeById(overridden, "clock")["available"].boolean(), "asset present");
        check(!modeById(overridden, "bokeh")["available"].boolean(), "asset missing");

        // Mode and level come from the markers; junk falls back.
        ScreensaverState marked = qml;
        marked.markerMode = "starfield";
        marked.markerLevel = "bright";
        marked.allowControl = true;
        marked.externalActive = true;
        const Json list = screensaverList(marked);
        check(list["current"].string() == "starfield", "mode from marker");
        check(list["level"].string() == "bright", "level from marker");
        check(modeById(list, "starfield")["active"].boolean(), "active flag");
        check(list["writable"].boolean(), "writable follows allowControl");
        check(list["external"]["active"].boolean() && !list["external"]["hook"].boolean(),
              "external detection");
        check(list["modes"].size() == 8, "eight entries in dashboard order");
        check(list["modes"][0]["id"].string() == "stock", "stock first");
        marked.markerMode = "bogus";
        marked.markerLevel = "x";
        check(screensaverMode(marked) == "stock", "unknown marker is stock");
        check(screensaverLevel(marked) == "dim", "unknown level is dim");
        marked.markerMode = "stock";
        check(screensaverMode(marked) == "stock", "stock marker is stock");
    } catch (const std::exception &error) {
        fprintf(stderr, "screensaver policy test failed: %s\n", error.what());
        return 1;
    }
    return 0;
}
