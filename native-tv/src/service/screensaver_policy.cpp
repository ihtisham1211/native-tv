#include "service/screensaver_policy.hpp"
#include "domain/say.hpp"
#include <vector>

namespace nativetv {
namespace service {
namespace {
struct Saver {
    const char *id;
    const char *labelKey;
    const char *label;
    const char *descriptionKey;
    const char *description;
};

// The order is the order the dashboards list them in.
const Saver kSavers[] = {
    {"stock", "srv.saver.stock", "LG default", "srv.saver.stock.desc",
     "The screen saver the TV shipped with."},
    {"clock", "srv.saver.clock", "Clock", "srv.saver.clock.desc",
     "A digital clock on black, moving to a new position every minute."},
    {"black", "srv.saver.black", "Black screen", "srv.saver.black.desc",
     "Nothing on screen. On an OLED the pixels are off."},
    {"starfield", "srv.saver.starfield", "Starfield", "srv.saver.starfield.desc",
     "A drifting cosmic starscape with occasional shooting stars."},
    {"fireworks", "srv.saver.fireworks", "Fireworks", "srv.saver.fireworks.desc",
     "Bursts of colour on black, a few seconds apart."},
    {"bokeh", "srv.saver.bokeh", "Bokeh", "srv.saver.bokeh.desc",
     "Soft circles of light drifting in and out on black."},
    {"harmonograph", "srv.saver.harmonograph", "Harmonograph", "srv.saver.harmonograph.desc",
     "Pendulum ribbons of light weaving and fading on black."},
    {"vitals", "srv.saver.vitals", "Panel vitals", "srv.saver.vitals.desc",
     "The TV's own readings - panel hours, pixel refresher countdown, temperature."},
};

bool knownSaver(const std::string &id) {
    for (const Saver &saver : kSavers)
        if (id == saver.id) return true;
    return false;
}
} // namespace

bool screensaverSlowSwitch(const ScreensaverState &state) {
    return !state.stockType.empty() && state.stockType != "qml";
}

bool screensaverOnByDefault(const ScreensaverState &state) {
    return screensaverSlowSwitch(state) &&
           (state.webosMajor == 10 || state.webosMajor == 11);
}

bool screensaverHeldBack(const ScreensaverState &state) {
    return screensaverSlowSwitch(state) && !screensaverOnByDefault(state) &&
           !state.allowedAnyway;
}

bool screensaverExperimental(const ScreensaverState &state) {
    return screensaverSlowSwitch(state) && !screensaverOnByDefault(state);
}

std::string screensaverMode(const ScreensaverState &state) {
    return knownSaver(state.markerMode) && state.markerMode != "stock"
        ? state.markerMode : "stock";
}

std::string screensaverLevel(const ScreensaverState &state) {
    return state.markerLevel == "bright" ? "bright" : "dim";
}

Json screensaverList(const ScreensaverState &state) {
    const std::string current = screensaverMode(state);
    const bool held = screensaverHeldBack(state);
    Json modes = Json::array();
    for (const Saver &saver : kSavers) {
        Json entry = Json::object();
        const std::string id = saver.id;
        entry.set("id", Json::text(id));
        entry.set("label", Json::text(say::msg(saver.labelKey, saver.label)));
        entry.set("description",
                  Json::text(say::msg(saver.descriptionKey, saver.description)));
        entry.set("active", Json::flag(id == current));
        entry.set("available", Json::flag(
            id == "stock" || (!held && state.assets.count(id) != 0)));
        modes.add(entry);
    }
    Json external = Json::object();
    external.set("active", Json::flag(state.externalActive));
    external.set("hook", Json::flag(state.externalHook));

    Json response = Json::object();
    response.set("ok", Json::flag(true));
    response.set("current", Json::text(current));
    response.set("level", Json::text(screensaverLevel(state)));
    response.set("modes", modes);
    response.set("writable", Json::flag(state.allowControl));
    response.set("slowSwitch", Json::flag(screensaverSlowSwitch(state)));
    // Always offered: held back, the tab shows the switch that turns custom
    // screen savers on, with its warning (a hidden tab read as broken).
    response.set("available", Json::flag(true));
    response.set("held", Json::flag(held));
    response.set("heldOverridden", Json::flag(
        screensaverExperimental(state) && state.allowedAnyway));
    // The Node implementation never sets its switching flag: sam is no longer
    // restarted for a switch, so the field stays false.
    response.set("switching", Json::flag(false));
    response.set("external", external);
    return response;
}

} // namespace service
} // namespace nativetv
