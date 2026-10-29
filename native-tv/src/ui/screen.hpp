#pragma once
#include "core/json.hpp"
#include "core/services.hpp"
#include "ui/kit/widgets.hpp"
#include "ui/motion/motion.hpp"
#include <functional>
#include <memory>
#include <string>
#include <vector>

// The extension point for screens that are not Home or Apps. A screen lives in its own file, keeps
// its own state and animated values, and is registered in screen_registry.cpp with one line;
// nothing in Home changes when one is added. The application owns the frame loop, the motion
// engine, the top bar and the wall: a screen draws its page content and answers keys.
namespace nativetv {
struct ViewState;

enum class Key { Left, Right, Up, Down, Ok, Back, Red, PageUp, PageDown, Delete };

// What the application offers a screen while it runs.
class ScreenHost {
  public:
    virtual ~ScreenHost() = default;
    virtual Services &services() = 0;
    // One engine for the whole application; prefix every id with the screen's name
    // ("live:card:3") so leave() can drop them with Motion::erasePrefix.
    virtual Motion &motion() = 0;
    // The data the application keeps for every screen: catalogue, stats, caps, setup, ...
    virtual const ViewState &view() const = 0;
    virtual double nowMs() const = 0; // the frame's clock: the same value in --time captures
    // Follows a route as a key press would ("home", "dashboard:apps", "quick", "search").
    virtual void open(const std::string &route) = 0;
    virtual void toast(const std::string &text) = 0;
    // True while the focus is on the top bar, so the page draws no focus of its own.
    virtual bool barFocused() const = 0;
    virtual void invalidate() = 0; // draw the next frame
    // The route the screen was opened from: "home" when Home was showing, a registered screen's
    // route, or empty. Closing goes back there ("home:resume" keeps Home as it was).
    virtual std::string cameFrom() const {
        return std::string();
    }
};

class ScreenView {
  public:
    virtual ~ScreenView() = default;
    // The name tests and `--script` see in screenName() ("live", "settings").
    virtual std::string name() const = 0;
    // The top bar over the page. -1 keeps the pill off every tab (the gear owns the page);
    // otherwise the bar's tab that this page belongs to. Setup answers false: it has no bar.
    virtual bool hasBar() const {
        return true;
    }
    virtual int barTab() const {
        return -1;
    }
    // True for a screen that paints its own wall (art, or the page it sits over, like the Quick
    // drawer over Home): the application then draws no ambient wall under it.
    virtual bool paintsWall() const {
        return false;
    }
    virtual void enter(ScreenHost &) {}
    virtual void leave(ScreenHost &) {}
    // True when the key was used. An unused Up with focus at the page's top row moves the focus
    // to the bar; Back falls through to back().
    virtual bool key(ScreenHost &, Key) {
        return false;
    }
    // True when Back was used (it closed a level of its own); false goes back to Home.
    virtual bool back(ScreenHost &) {
        return false;
    }
    // Characters from a hardware keyboard or the TV's text input; true when the page took them.
    virtual bool text(ScreenHost &, const std::string &) {
        return false;
    }
    // The bar handed the focus back (Down or OK on a tab): focus the page's first item.
    virtual void focus(ScreenHost &) {}
    // A pointer click on one of the hit ids this screen's build() added.
    virtual void click(ScreenHost &, const std::string &) {}
    // Replies on channels the shell does not use itself (everything but catalogue, discover,
    // guide, caps, recents and the Home picture channels).
    virtual void response(ScreenHost &, const Response &) {}
    // Called once per frame after the motion engine; true while the screen still needs frames
    // (a timer, a chart sliding). Animations made with Motion keep the loop running by themselves.
    virtual bool update(ScreenHost &) {
        return false;
    }
    // The page content, drawn over the wall and under the top bar.
    virtual void build(ScreenHost &, kit::Paint) = 0;
    virtual Json snapshot() const {
        return Json::object();
    }
};

// A factory answers null for a route it does not own. Routes are the strings the Command model
// uses ("live", "dashboard:oled", "setup:welcome", "details").
using ScreenFactory = std::function<std::unique_ptr<ScreenView>(const std::string &route)>;

class ScreenRegistry {
  public:
    void add(ScreenFactory factory) {
        factories_.push_back(std::move(factory));
    }
    std::unique_ptr<ScreenView> create(const std::string &route) const {
        for (const auto &factory : factories_)
            if (std::unique_ptr<ScreenView> screen = factory(route))
                return screen;
        return nullptr;
    }

  private:
    std::vector<ScreenFactory> factories_;
};

// One line per screen, in screen_registry.cpp.
void registerScreens(ScreenRegistry &registry);
} // namespace nativetv
