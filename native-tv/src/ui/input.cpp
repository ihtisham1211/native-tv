#include "ui/application.hpp"
#include "ui/controller_helpers.hpp"
#include "ui/home_layout.hpp"
#include <algorithm>

namespace nativetv {
void Application::key(Key key) {
    if (state_.screen == Screen::Splash)
        return;
    // The first key after the screen dimmed only wakes it, on every screen.
    if (wake())
        return;
    if (screen_) {
        screenKey(key);
        return;
    }
    if (key == Key::Back) {
        back();
        return;
    }
    if (key != Key::Ok)
        confirmId_.clear();
    dirty_ = true;
    if (isRecents()) {
        recentKey(key);
        recentsFocus();
    } else if (state_.screen == Screen::Home) {
        auto &home = state_.home;
        // A key that arrives while the page is changing acts on the page that is leaving.
        if (home.navigation) {
            auto stops = navigationStops(state_);
            auto current = std::find(stops.begin(), stops.end(), home.nav);
            size_t position =
                current == stops.end() ? 0 : static_cast<size_t>(current - stops.begin());
            if (key == Key::Left && position)
                --position;
            if (key == Key::Right && position + 1 < stops.size())
                ++position;
            home.nav = stops[position];
            if (key == Key::Down) {
                // Down from a tab opens its page first; the focus lands on the page's first item.
                if (home.nav < home.tabs.size() && home.nav != flow_.shown)
                    homeSelectTab(home.nav);
                home.navigation = false;
            }
            if (key == Key::Ok) {
                if (home.nav < home.tabs.size()) {
                    homeSelectTab(home.nav);
                    home.navigation = false;
                } else
                    route(home.nav == home.tabs.size()       ? "profiles"
                          : home.nav == home.tabs.size() + 1 ? "quick"
                          : home.nav == home.tabs.size() + 2 ? "search"
                                                             : "dashboard");
            }
        } else if (!home.rows.empty()) {
            bool grid = home.tabs[home.tab].key == "apps";
            if (key == Key::Up) {
                if (grid && home.column >= home::kGridColumns)
                    home.column -= home::kGridColumns;
                else if (home.row)
                    --home.row;
                else
                    home.navigation = true;
            }
            if (key == Key::Down) {
                if (grid && home.column + home::kGridColumns < home.rows[home.row].cards.size())
                    home.column += home::kGridColumns;
                else if (!grid && home.row + 1 < home.rows.size())
                    ++home.row;
            }
            if (key == Key::Left && home.column)
                --home.column;
            if (key == Key::Right && home.column + 1 < home.rows[home.row].cards.size())
                ++home.column;
            home.column = clampIndex(home.column, home.rows[home.row].cards.size());
            const Card &card = home.rows[home.row].cards[home.column];
            if (key == Key::Ok || key == Key::Red) {
                if (card.kind == "title") {
                    if (key == Key::Ok)
                        openDetails(card.data);
                    else
                        execute(post(Operation::OpenHomeItem, fields({{"kind", Json::text("title")},
                                                                      {"type", card.data["type"]},
                                                                      {"id", card.data["id"]}})));
                } else
                    execute(card.command);
            }
        }
        homeKeyMoved(key);
    } else {
        auto &menu = state_.menu;
        if (state_.screen == Screen::Dashboard && menu.sub.empty() &&
            (key == Key::Left || key == Key::Right)) {
            size_t count = dashboardTabs().size();
            menu.tab = (menu.tab + count + (key == Key::Right ? 1 : -1)) % count;
            menu.selected = 0;
            state_.menuData = Json();
            services_.read("menu", dashboardTabs()[menu.tab].operation);
            rebuildMenu();
            return;
        }
        if (state_.screen == Screen::Dashboard && !menu.sub.empty() && key == Key::Left) {
            back();
            return;
        }
        int direction = key == Key::Up || key == Key::PageUp       ? -1
                        : key == Key::Down || key == Key::PageDown ? 1
                                                                   : 0;
        if (direction && !menu.rows.empty()) {
            int index = static_cast<int>(menu.selected),
                count = key == Key::PageUp || key == Key::PageDown ? 6 : 1;
            for (int i = 0; i < count; ++i) {
                int next = index + direction;
                while (next >= 0 && next < static_cast<int>(menu.rows.size()) &&
                       menu.rows[next].heading)
                    next += direction;
                if (next < 0 || next >= static_cast<int>(menu.rows.size()))
                    break;
                index = next;
            }
            menu.selected = static_cast<size_t>(index);
        }
        if (key == Key::Ok && menu.selected < menu.rows.size())
            execute(menu.rows[menu.selected].command);
    }
}
void Application::input(const std::string &text) {
    wake(); // typing on a dimmed screen wakes it and is still typed
    if (screen_ && screen_->text(*this, text))
        dirty_ = true;
}
namespace {
// The kit's top bar names its hits itself; the controllers know them by tab and tool. The avatar
// is a decoration, not a target, while there is only one profile.
std::string controllerHit(const ViewState &state, const std::string &id) {
    if (id == "nav:avatar")
        return state.catalogue["profiles"].size() > 1 ? "tool:0" : "";
    if (id == "nav:settings")
        return "tool:1";
    if (id.compare(0, 8, "nav:tab:") == 0)
        return "tab:" + id.substr(8);
    return id;
}
} // namespace
void Application::click(float x, float y) {
    if (wake())
        return;
    Scene visible = scene();
    for (auto it = visible.hits.rbegin(); it != visible.hits.rend(); ++it)
        if (it->rect.contains(x, y)) {
            std::string id = controllerHit(state_, it->id);
            if (id.empty())
                continue;
            if (id == "back") {
                back();
                return;
            }
            if (screen_ && id.compare(0, 4, "tab:") != 0 && id.compare(0, 5, "tool:") != 0) {
                screen_->click(*this, id);
                dirty_ = true;
                return;
            }
            auto colon = id.find(':');
            std::string type = id.substr(0, colon),
                        argument = colon == std::string::npos ? "" : id.substr(colon + 1);
            size_t index = static_cast<size_t>(std::atoi(argument.c_str()));
            if (type == "tab") {
                if (screen_) {
                    barFocus_ = state_.home.navigation = true;
                    state_.home.nav = index;
                    key(Key::Ok);
                    return;
                }
                state_.home.navigation = true;
                state_.home.nav = index;
                key(Key::Ok);
            } else if (type == "tool")
                route(index == 0   ? "profiles"
                      : index == 1 ? "quick"
                      : index == 2 ? "search"
                                   : "dashboard");
            else if (type == "card") {
                state_.home.navigation = false;
                state_.home.row = index;
                state_.home.column =
                    static_cast<size_t>(std::atoi(argument.substr(argument.find(':') + 1).c_str()));
                key(Key::Ok);
            } else if (type == "dash") {
                if (index < dashboardTabs().size())
                    route("dashboard:" + dashboardTabs()[index].key);
            } else if (type == "menu") {
                state_.menu.selected = index;
                key(Key::Ok);
            } else if (type == "recent") {
                state_.recents.selected = index;
                state_.recents.focus = RecentState::Focus::Card;
                key(Key::Ok);
            } else if (type == "recent-close") {
                state_.recents.selected = index;
                state_.recents.focus = RecentState::Focus::Close;
                key(Key::Ok);
            } else if (id == "recents-all") {
                state_.recents.focus = RecentState::Focus::All;
                key(Key::Ok);
            }
            return;
        }
}
bool Application::activate(const std::string &id) {
    for (const auto &hit : scene().hits) {
        if (controllerHit(state_, hit.id) == id) {
            click(hit.rect.x + hit.rect.w / 2, hit.rect.y + hit.rect.h / 2);
            return true;
        }
    }
    return false;
}
void Application::longPress() {
    if (state_.screen == Screen::Home)
        route("recents");
}
} // namespace nativetv
