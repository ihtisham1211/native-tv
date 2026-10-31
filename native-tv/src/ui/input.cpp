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
        } else if (home.arranging) {
            homeArrange(key);
        } else if (home.editing) {
            homeEditKey(key);
        } else if (home.rows.empty()) {
            if (key == Key::Up)
                home.navigation = true; // nothing to move on: Up reaches the bar
        } else {
            bool grid = home.grid;
            if (key == Key::Up) {
                if (grid && home.column >= 6)
                    home.column -= 6;
                else if (home.row)
                    --home.row;
                else
                    home.navigation = true;
            }
            if (key == Key::Down) {
                if (grid && home.column + 6 < home.rows[home.row].cards.size())
                    home.column += 6;
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
                    else // Red: straight to Netflix, without the details
                        execute(post(Operation::OpenHomeItem, fields({{"kind", Json::text("netflix")},
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
        return state.catalogue["profiles"].size() ? "tool:0" : "";
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
                if (state_.home.editing)
                    homeEditDone(false);
                if (screen_) {
                    barFocus_ = state_.home.navigation = true;
                    state_.home.nav = index;
                    key(Key::Ok);
                    return;
                }
                state_.home.navigation = true;
                state_.home.nav = index;
                key(Key::Ok);
            } else if (type == "tool") {
                if (state_.home.editing)
                    homeEditDone(false);
                route(index == 0   ? "profiles"
                      : index == 1 ? "quick"
                      : index == 2 ? "search"
                                   : "dashboard");
            } else if (type == "edit") {
                if (state_.home.editing)
                    homeEditDone(true);
            } else if (type == "card") {
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
    if (state_.screen != Screen::Home)
        return;
    auto &home = state_.home;
    if (flow_.pendingTab >= 0)
        return; // the page is about to be swapped: a hold now would start on the one that is leaving
    if (home.arranging)
        return; // the hold that lifted the row is not a second request for the recent apps
    if (home.editing) {
        homeEditDone(false); // the hold that started the edit is not a second request for it
        return;
    }
    // On the Apps tab with the apps sorted by kind, holding OK lifts the focused row to be moved
    // among the kinds; on any other app it starts editing the apps; anywhere else it does nothing
    // (the recent apps are Back held).
    if (!home.grid && !home.navigation && !home.tabs.empty() &&
        home.tabs[home.tab].key == "apps" && home.rows.size() > 1) {
        home.arranging = true;
        homeFocus();
        dirty_ = true;
        return;
    }
    if (!home.navigation && home.row < home.rows.size() && home::isApps(home.rows[home.row]) &&
        home.column < home.rows[home.row].cards.size()) {
        homeEditStart();
    }
}
void Application::longBack() {
    if (state_.screen != Screen::Home || state_.home.arranging || state_.home.editing)
        return; // the hold is not a request for the recent apps while rows or apps are moved
    route("recents");
}

// ---- Editing the apps ---------------------------------------------------------------------------

// The hidden list that editing works on: the Apps page's own, or Home's for the row on Home.
std::string Application::editedList() const {
    return state_.home.tabs[state_.home.tab].key == "apps" ? "appsTab" : "app";
}
void Application::homeEditStart() {
    auto &home = state_.home;
    home.editing = true;
    home.editHide = false;
    // The row also lists the apps hidden from it, last and dimmed, so that one can be shown again:
    // Home's row those hidden on Home, the Apps page those hidden on the Apps page.
    CardRow &row = home.rows[home.row];
    const std::string focused = row.cards[home.column].id;
    row.cards = appsRow(state_.catalogue, state_.guide, true, editedList()).cards;
    home.column = 0;
    for (size_t i = 0; i < row.cards.size(); ++i)
        if (row.cards[i].id == focused)
            home.column = i;
    motion_.tween("lift", 1, kit::motion::ms::fast, Easing::out());
    homeFocus();
    dirty_ = true;
}
void Application::homeEditKey(Key key) {
    auto &home = state_.home;
    if (home.row >= home.rows.size()) {
        homeEditDone(false);
        return;
    }
    auto &cards = home.rows[home.row].cards;
    const long columns = static_cast<long>(home::kGridColumns);
    if (key == Key::Red || key == Key::Delete) {
        homeEditDone(true);
        return;
    }
    if (home.editHide) {
        // The button above the page: OK hides (or shows) the app, Down goes back to moving it.
        if (key == Key::Ok)
            homeEditDone(true);
        else if (key == Key::Down)
            home.editHide = false;
        return;
    }
    home.column = clampIndex(home.column, cards.size());
    if (key == Key::Ok) {
        // OK on an app that was hidden shows it; on any other it saves the order.
        homeEditDone(cards[home.column].hidden);
        return;
    }
    if (key == Key::Up && (!home.grid || home.column < static_cast<size_t>(columns))) {
        home.editHide = true;
        return;
    }
    long target = static_cast<long>(home.column);
    if (key == Key::Left)
        target -= 1;
    else if (key == Key::Right)
        target += 1;
    else if (home.grid && key == Key::Up)
        target -= columns;
    else if (home.grid && key == Key::Down)
        target += columns;
    if (target < 0 || target >= static_cast<long>(cards.size()) ||
        target == static_cast<long>(home.column))
        return;
    // Hidden apps only take the focus, and so do apps of another kind while the apps are sorted
    // by kind (an app stays among its kind); the others swap places.
    const Card &from = cards[home.column], &to = cards[static_cast<size_t>(target)];
    const bool byKind = state_.catalogue["layout"]["sort"]["app"].string() == "kind";
    if (!from.hidden && !to.hidden && !(byKind && kindOf(from) != kindOf(to)))
        std::swap(cards[home.column], cards[static_cast<size_t>(target)]);
    home.column = static_cast<size_t>(target);
}
// Ends editing and saves in one request. `toggle` hides the focused app, or shows it again when it
// is one of the hidden ones. The rows are rebuilt from the catalogue as the save leaves it.
void Application::homeEditDone(bool toggle) {
    auto &home = state_.home;
    home.editing = home.editHide = false;
    motion_.tween("lift", 0, kit::motion::ms::fast, Easing::out());
    dirty_ = true;
    if (home.row >= home.rows.size() || home.column >= home.rows[home.row].cards.size()) {
        rebuildHome();
        return;
    }
    const auto &cards = home.rows[home.row].cards;
    const std::string focused = cards[home.column].id;
    const bool focusedHidden = cards[home.column].hidden;
    if (toggle && !focusedHidden && focused == "lg-home") {
        toast("LG Home is switched off with Show LG Home in the Home settings.");
        toggle = false;
    }
    // The ids on screen in their new order; the app being shown joins them, last.
    std::vector<std::string> shown, expected;
    for (const Card &card : cards)
        if (!card.hidden && !(toggle && card.id == focused))
            shown.push_back(card.id);
    const std::string list = editedList();
    for (const Card &card : appsRow(state_.catalogue, state_.guide, false, list).cards)
        if (!(toggle && card.id == focused))
            expected.push_back(card.id);
    if (toggle && focusedHidden) {
        shown.push_back(focused);
        expected.push_back(focused);
    }
    const bool moved = shown != expected;
    if (!moved && !toggle) {
        rebuildHome(); // drops the hidden apps that were listed, and takes any refresh meanwhile
        return;
    }
    Json hiddenNow = Json::array();
    for (const auto &id : hiddenIds(state_.catalogue, list).items())
        if (!(toggle && focusedHidden && id.string() == focused))
            hiddenNow.add(id);
    if (toggle && !focusedHidden)
        hiddenNow.add(Json::text(focused));
    Json order = Json::array();
    for (const auto &id : orderAfterMove(state_.catalogue["order"]["app"], shown))
        order.add(Json::text(id));
    const bool byKind = state_.catalogue["layout"]["sort"]["app"].string() == "kind";
    Json body = Json::object();
    if (moved) {
        // A move among the apps of a kind keeps the apps sorted by kind; any other makes the
        // order the person's own.
        body.set("order", fields({{"app", order}}));
        if (!byKind)
            body.set("sort", fields({{"app", Json::text("custom")}}));
    }
    if (toggle)
        body.set("hidden", fields({{list, hiddenNow}}));
    // The catalogue shows the save at once; the server's answer replaces it on the next read.
    auto object = [](const Json &value) { return value.isObject() ? value : Json::object(); };
    Json orderAll = object(state_.catalogue["order"]), layout = object(state_.catalogue["layout"]);
    if (moved) {
        orderAll.set("app", order);
        Json sort = object(layout["sort"]);
        if (!byKind)
            sort.set("app", Json::text("custom"));
        layout.set("sort", sort);
    }
    if (toggle) {
        Json hiddenAll = object(layout["hidden"]);
        if (!hiddenAll["appsTab"].isArray()) // the server's migration: the Apps page starts as Home's
            hiddenAll.set("appsTab", hiddenAll["app"].isArray() ? hiddenAll["app"] : Json::array());
        hiddenAll.set(list, hiddenNow);
        layout.set("hidden", hiddenAll);
    }
    state_.catalogue.set("order", orderAll);
    state_.catalogue.set("layout", layout);
    rebuildHome();
    execute(post(Operation::SetHomeLayout, body));
}
} // namespace nativetv
