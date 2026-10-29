#include "ui/application.hpp"
#include "ui/controller_helpers.hpp"
#include <algorithm>

namespace nativetv {
void Application::route(const std::string &path) {
    confirmId_.clear();
    services_.cancel("menu");
    state_.qr.clear();
    clearPicture();
    bool fromRecents = isRecents();
    const std::string origin = screen_ ? currentRoute_ : state_.screen == Screen::Home ? "home" : "";
    if (screen_) {
        screen_->leave(*this);
        screen_.reset();
        barFocus_ = false;
        // An overlay closing over Home hands the focus back to what opened it (the gear).
        if (path != "home:resume")
            state_.home.navigation = false;
    }
    if (path == "search" && !state_.discover["titlesEnabled"].boolean())
        return;
    if (path != "refresh" && path.compare(0, 8, "install-") != 0 && path != "theme" &&
        path.compare(0, 4, "sub:") != 0) {
        if (std::unique_ptr<ScreenView> screen = registry_.create(path)) {
            screen_ = std::move(screen);
            currentRoute_ = path;
            cameFrom_ = origin;
            state_.screen = Screen::Kit; // no older view draws while a registered screen does
            state_.kitComponent = path;
            screen_->enter(*this);
            homeFocus(true);
            refresh();
            dirty_ = true;
            return;
        }
    }
    if (path == "install-root" || path == "install-replace") {
        if (path == "install-root")
            state_.installRoot = !state_.installRoot;
        else
            state_.installReplace = !state_.installReplace;
        rebuildMenu();
        return;
    }
    if (path == "refresh") {
        refresh();
        return;
    }
    if (path == "theme") {
        state_.light = !state_.light;
        rebuildMenu();
        return;
    }
    if (path.compare(0, 4, "kit:") == 0) {
        state_.screen = Screen::Kit;
        state_.kitComponent = path.substr(4);
    } else if (path == "home" || path == "home:resume") {
        state_.screen = Screen::Home;
        // Coming back from the recents or an overlay keeps the page as it was; any other screen
        // starts it again (so does a Home whose entrance never played).
        if (!fromRecents && (path == "home" || !flow_.entered))
            homeStart();
        rebuildHome();
        if (fromRecents)
            recentsFocus(true);
    } else if (path.compare(0, 10, "dashboard:") == 0 || path == "dashboard") {
        state_.screen = Screen::Dashboard;
        state_.menu = MenuState();
        state_.menuData = Json();
        auto tabs = dashboardTabs();
        std::string key = path == "dashboard" ? "system" : path.substr(10);
        for (size_t i = 0; i < tabs.size(); ++i)
            if (tabs[i].key == key)
                state_.menu.tab = i;
    } else if (path.compare(0, 6, "setup:") == 0) {
        if (state_.screen == Screen::Setup && state_.menu.level == "handoff")
            services_.write("handoff-stop", Operation::Setup,
                            fields({{"action", Json::text("handoffStop")}}));
        state_.screen = Screen::Setup;
        state_.menu = MenuState();
        state_.menu.level = path.substr(6);
        if (state_.menu.level == "handoff")
            execute(post(Operation::Setup, fields({{"action", Json::text("handoff")}})));
    } else if (path.compare(0, 4, "sub:") == 0) {
        state_.menu.sub = path.substr(4);
        state_.menu.selected = 0;
    } else if (path == "catalog") {
        state_.screen = Screen::Catalog;
        state_.menu = MenuState();
    } else if (path == "recents") {
        state_.screen = Screen::Quick;
        state_.menu = MenuState();
        state_.menu.level = "recents";
        state_.recents = RecentState();
        state_.recents.status = "Loading recent apps…";
        motion_.erasePrefix("rf");
        motion_.set("rcx", 0);
        motion_.play("rec", Tween(0, 1, kit::motion::ms::fast, Easing::out()));
    } else if (path == "qr:address") {
        state_.screen = Screen::Qr;
        state_.qr = state_.caps["origin"].string() + "/";
        if (state_.caps.has("key"))
            state_.qr += "?k=" + encode(state_.caps["key"].string());
    }
    rebuildMenu();
    refresh();
    dirty_ = true;
}
void Application::execute(const Command &command) {
    if (!command.route.empty()) {
        route(command.route);
        return;
    }
    if (command.operation == Operation::None || state_.busy)
        return;
    std::string id = std::to_string(static_cast<int>(command.operation)) + command.body.dump();
    if (command.confirm && (confirmId_ != id || static_cast<int32_t>(now_ - confirmUntil_) >= 0)) {
        confirmId_ = id;
        confirming_ = command;
        confirmUntil_ = now_ + 5000;
        toast("Press OK again to confirm. Back cancels.");
        return;
    }
    confirmId_.clear();
    pending_ = command;
    state_.busy = true;
    if (!services_.write("action", command.operation, command.body)) {
        state_.busy = false;
        toast("Too many pending requests. Try again.");
    }
    dirty_ = true;
}
// Title details are a registered screen; Home hands it the title by type and id.
void Application::openDetails(const Json &item) {
    route("details:" + item["type"].string() + ":" + item["id"].string());
}
void Application::back() {
    Screen previous = state_.screen;
    if (!confirmId_.empty()) {
        confirmId_.clear();
        toast("Cancelled");
        return;
    }
    if (state_.screen == Screen::Catalog)
        route("dashboard:apps");
    else if (state_.screen == Screen::Dashboard && !state_.menu.sub.empty()) {
        state_.menu.sub.clear();
        state_.menu.selected = 0;
        rebuildMenu();
    } else if (state_.screen == Screen::Qr)
        route("dashboard:settings");
    else if (state_.screen == Screen::Setup) {
        if (state_.menu.level == "handoff")
            services_.write("handoff-stop", Operation::Setup,
                            fields({{"action", Json::text("handoffStop")}}));
        route("dashboard:settings");
    } else if (state_.screen == Screen::Home) {
        flow_.tabPending = false;
        if (state_.home.navigation)
            route("recents");
        else if (state_.home.row)
            state_.home.row = 0;
        else
            state_.home.navigation = true;
        if (state_.screen == Screen::Home) {
            state_.home.nav = state_.home.navigation ? state_.home.tab : state_.home.nav;
            homeFocus();
        }
    } else
        route("home");
    if (previous != Screen::Home && state_.screen == Screen::Home)
        refresh();
    dirty_ = true;
}
// Keys for a registered screen: the page first, then the bar when the page lets Up go.
void Application::screenKey(Key key) {
    auto &home = state_.home;
    dirty_ = true;
    if (key == Key::Back) {
        if (barFocus_) {
            barFocus_ = home.navigation = false;
            screen_->focus(*this);
        } else if (!screen_->back(*this))
            route("home");
        homeFocus();
        return;
    }
    if (!barFocus_) {
        if (!screen_->key(*this, key) && key == Key::Up && screen_->hasBar()) {
            barFocus_ = home.navigation = true;
            int tab = screen_->barTab();
            home.nav = tab >= 0 ? static_cast<size_t>(tab) : home.tabs.size() + 1;
            homeFocus();
        }
        return;
    }
    auto stops = navigationStops(state_);
    auto current = std::find(stops.begin(), stops.end(), home.nav);
    size_t position = current == stops.end() ? 0 : static_cast<size_t>(current - stops.begin());
    if (key == Key::Left && position)
        --position;
    if (key == Key::Right && position + 1 < stops.size())
        ++position;
    home.nav = stops[position];
    if (key == Key::Down) {
        barFocus_ = home.navigation = false;
        screen_->focus(*this);
    } else if (key == Key::Ok) {
        barFocus_ = home.navigation = false;
        if (home.nav < home.tabs.size()) {
            size_t tab = home.nav;
            std::string name = home.tabs[tab].key;
            if (registry_.create("tab:" + name)) {
                route("tab:" + name);
                return;
            }
            route("home");
            home.tab = tab;
            rebuildHome();
            homeStart();
            homeData();
        } else
            route(home.nav == home.tabs.size()       ? "profiles"
                  : home.nav == home.tabs.size() + 1 ? "quick"
                  : home.nav == home.tabs.size() + 2 ? "search"
                                                     : "dashboard");
        return;
    }
    homeFocus();
}
} // namespace nativetv
