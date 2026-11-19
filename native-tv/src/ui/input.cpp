#include "domain/say.hpp"
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
        } else if (home.appMenu) {
            homeAppMenuKey(key);
        } else if (home.editing) {
            homeEditKey(key);
        } else if (home.rows.empty()) {
            if (key == Key::Up)
                home.navigation = true; // nothing to move on: Up reaches the bar
        } else {
            if (key == Key::Up) {
                if (home.row)
                    --home.row;
                else
                    home.navigation = true;
            }
            if (key == Key::Down && home.row + 1 < home.rows.size())
                ++home.row;
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
// The kit's top bar names its hits itself; the controllers know them by tab and tool.
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
                if (state_.home.appMenu)
                    homeAppMenuClose();
                if (state_.home.editing)
                    homeEditDone();
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
                if (state_.home.appMenu)
                    homeAppMenuClose();
                if (state_.home.editing)
                    homeEditDone();
                route(index == 0   ? "profiles"
                      : index == 1 ? "quick"
                      : index == 2 ? "search"
                                   : "dashboard");
            } else if (type == "arrange") {
                if (state_.home.arranging) {
                    if (argument == "done")
                        homeArrangeDone();
                    else if (!state_.home.carrying) { // as with the keys, these wait until an app is put down
                        if (argument == "hide")
                            homeArrangeHide();
                        else if (argument == "move") {
                            state_.home.carryingKind = true;
                            state_.home.arrangeFocus = 2;
                            homeFocus();
                        }
                    }
                }
            } else if (type == "appmenu") {
                if (state_.home.appMenu && index < state_.home.appMenuItems.size()) {
                    homeAppMenuSelect(index);
                    homeAppMenuAct();
                    if (state_.screen == Screen::Home && !screen_)
                        homeFocus();
                }
            } else if (type == "card") {
                if (state_.home.appMenu)
                    homeAppMenuClose(); // a click beside the menu closes it, then acts as ever
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
    // A theme with a dock rests Home's bar as a pill, so there are no tabs to click: a click in the
    // bar's band brings the bar in, and the next click acts on it.
    if (state_.screen == Screen::Home && !screen_ && !state_.home.tabs.empty() &&
        home::onDock(state_.home.tabs[state_.home.tab].key) && !state_.home.navigation &&
        !state_.home.editing && !state_.home.appMenu && !state_.home.arranging &&
        y < kit::look::barTop + kit::look::barH + 40) {
        state_.home.navigation = true;
        state_.home.nav = state_.home.tab;
        homeFocus();
        dirty_ = true;
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
    if (home.arranging || home.appMenu)
        return; // the hold that lifted the row or opened the menu is not a second request
    if (home.editing) {
        homeEditDone(); // a hold while an app is moved puts it down
        return;
    }
    // On the Apps tab with the apps sorted by kind, holding OK lifts the focused row as a card whose
    // apps are edited and which can be moved among the kinds; on any other app it opens the app's
    // menu; anywhere else it does nothing (the recent apps are Back held).
    if (!home.navigation && !home.tabs.empty() &&
        home.tabs[home.tab].key == "apps" && home.rows.size() > 1) {
        homeKindEditStart();
        return;
    }
    // Search and Settings among a dock's apps are not apps: they have no menu.
    if (!home.navigation && home.row < home.rows.size() && home::isApps(home.rows[home.row]) &&
        home.column < home.rows[home.row].cards.size() &&
        home.rows[home.row].cards[home.column].id.compare(0, 5, "tool:") != 0) {
        homeAppMenuOpen();
    }
}
void Application::longBack() {
    if (state_.screen != Screen::Home || state_.home.arranging || state_.home.editing ||
        state_.home.appMenu)
        return; // the hold is not a request for the recent apps while rows or apps are moved
    route("recents");
}

// ---- Editing the apps ---------------------------------------------------------------------------

// The hidden list that editing works on: the Apps page's own, or Home's for the row on Home.
std::string Application::editedList() const {
    return state_.home.tabs[state_.home.tab].key == "apps" ? "appsTab" : "app";
}
// Opens the focused app's menu: Move, Hide (not for LG Home, which has its own switch) and, while
// this list hides any app, Hidden apps, which opens the settings where they are shown again.
void Application::homeAppMenuOpen() {
    auto &home = state_.home;
    const Card &card = home.rows[home.row].cards[home.column];
    home.appMenuItems = {"move"};
    if (card.id != "lg-home")
        home.appMenuItems.push_back("hide");
    bool anyHidden = false;
    for (const Card &other : appsRow(state_.catalogue, state_.guide, true, editedList()).cards)
        anyHidden = anyHidden || other.hidden;
    if (anyHidden)
        home.appMenuItems.push_back("hidden");
    home.appMenu = true;
    home.appMenuApp = card.id;
    for (size_t i = 0; i < home.appMenuItems.size(); ++i)
        motion_.set(home::id("f:mi", i), i == 0 ? 1 : 0);
    home.appMenuItem = 0;
    motion_.play("appmenu", Tween(0, 1, kit::motion::ms::fast, Easing::out()));
    dirty_ = true;
}
void Application::homeAppMenuKey(Key key) {
    auto &home = state_.home;
    // The menu is the app's it was opened on; a refresh that took that app from under it closes it.
    if (home.row >= home.rows.size() || home.column >= home.rows[home.row].cards.size() ||
        home.rows[home.row].cards[home.column].id != home.appMenuApp) {
        homeAppMenuClose();
        return;
    }
    if (key == Key::Up && home.appMenuItem > 0)
        homeAppMenuSelect(home.appMenuItem - 1);
    else if (key == Key::Down && home.appMenuItem + 1 < home.appMenuItems.size())
        homeAppMenuSelect(home.appMenuItem + 1);
    else if (key == Key::Ok)
        homeAppMenuAct();
    else if (key == Key::Red || key == Key::Delete) {
        // A shortcut for the remotes that have it; Hide in the menu does the same.
        homeAppMenuClose();
        homeHideApp();
    } else if (key == Key::Left || key == Key::Right)
        homeAppMenuClose();
}
void Application::homeAppMenuSelect(size_t item) {
    auto &home = state_.home;
    if (item >= home.appMenuItems.size() || item == home.appMenuItem)
        return;
    motion_.tween(home::id("f:mi", home.appMenuItem), 0, kit::motion::ms::focus, Easing::out());
    motion_.tween(home::id("f:mi", item), 1, kit::motion::ms::focus, Easing::out());
    home.appMenuItem = item;
    dirty_ = true;
}
void Application::homeAppMenuAct() {
    auto &home = state_.home;
    if (home.appMenuItem >= home.appMenuItems.size()) {
        homeAppMenuClose();
        return;
    }
    const std::string item = home.appMenuItems[home.appMenuItem];
    homeAppMenuClose();
    if (item == "move")
        homeEditStart();
    else if (item == "hide")
        homeHideApp();
    else if (item == "hidden") // the settings' Home section, on its Apps page, shows them again
        route("dashboard:home/" + say::msg("home.row.apps", "Apps"));
}
void Application::homeAppMenuClose() {
    auto &home = state_.home;
    home.appMenu = false;
    motion_.set("appmenu", 0);
    for (size_t i = 0; i < home.appMenuItems.size(); ++i)
        motion_.set(home::id("f:mi", i), 0);
    dirty_ = true;
}
// Hides the focused app from this list and saves at once. The focus keeps its place, so it lands
// on the app that takes it; the toast says where the app is shown again.
void Application::homeHideApp() {
    auto &home = state_.home;
    if (home.row >= home.rows.size() || home.column >= home.rows[home.row].cards.size())
        return;
    const Card card = home.rows[home.row].cards[home.column];
    if (card.id.compare(0, 5, "tool:") == 0)
        return;
    if (card.id == "lg-home") {
        toast("LG Home is switched off with Show LG Home in the Home settings.");
        return;
    }
    // A request already out would make this one wait in vain (execute drops it): the hide would show
    // and then come back on the next refresh.
    if (state_.busy) {
        toast("Still saving the last change. Try again in a moment.");
        return;
    }
    const std::string list = editedList();
    Json hiddenNow = Json::array();
    for (const auto &id : hiddenIds(state_.catalogue, list).items())
        if (id.string() != card.id)
            hiddenNow.add(id);
    hiddenNow.add(Json::text(card.id));
    toast(card.title + " hidden. Settings, Home, " + say::msg("home.row.apps", "Apps") +
          " shows it again.");
    saveApps(list, std::vector<std::string>(), false, true, hiddenNow, Json());
    saveToastShown_ = state_.busy; // the request went out: its answer keeps this toast
}
// Move in an app's menu: the app is carried along its row until OK or Back puts it down.
void Application::homeEditStart() {
    auto &home = state_.home;
    if (home.row >= home.rows.size() || home.column >= home.rows[home.row].cards.size())
        return;
    home.editing = true;
    const std::string focused = home.rows[home.row].cards[home.column].id;
    if (home::onDock(home.tabs[home.tab].key)) {
        // The dock's rows of six are edited as the one row they came from; Search and Settings are
        // not apps, so they leave it until the save.
        size_t first = home.row, last = home.row;
        while (first > 0 && home::isApps(home.rows[first - 1]))
            --first;
        while (last + 1 < home.rows.size() && home::isApps(home.rows[last + 1]))
            ++last;
        home.rows.erase(home.rows.begin() + static_cast<std::ptrdiff_t>(first) + 1,
                        home.rows.begin() + static_cast<std::ptrdiff_t>(last) + 1);
        home.row = first;
    }
    CardRow &row = home.rows[home.row];
    row.cards = appsRow(state_.catalogue, state_.guide, false, editedList()).cards;
    home.column = 0;
    for (size_t i = 0; i < row.cards.size(); ++i)
        if (row.cards[i].id == focused)
            home.column = i;
    homeFocus();
    dirty_ = true;
}
// Left and Right swap the carried app with its neighbour of the same kind; OK puts it down.
void Application::homeEditKey(Key key) {
    auto &home = state_.home;
    if (home.row >= home.rows.size() || home.rows[home.row].cards.empty()) {
        homeEditDone();
        return;
    }
    auto &cards = home.rows[home.row].cards;
    home.column = clampIndex(home.column, cards.size());
    if (key == Key::Ok) {
        homeEditDone();
        return;
    }
    if (key != Key::Left && key != Key::Right)
        return;
    const long target = static_cast<long>(home.column) + (key == Key::Left ? -1 : 1);
    if (target < 0 || target >= static_cast<long>(cards.size()))
        return;
    // An app stays among its kind, so the apps sorted by kind stay sorted.
    if (kindOf(cards[home.column]) != kindOf(cards[static_cast<size_t>(target)]))
        return;
    std::swap(cards[home.column], cards[static_cast<size_t>(target)]);
    home.column = static_cast<size_t>(target);
}
// Puts the carried app down and saves the order in one request, if it changed. The rows are rebuilt
// from the catalogue as the save leaves it.
void Application::homeEditDone() {
    auto &home = state_.home;
    home.editing = false;
    dirty_ = true;
    if (home.row >= home.rows.size()) {
        rebuildHome();
        return;
    }
    std::vector<std::string> shown, expected;
    for (const Card &card : home.rows[home.row].cards)
        shown.push_back(card.id);
    const std::string list = editedList();
    for (const Card &card : appsRow(state_.catalogue, state_.guide, false, list).cards)
        expected.push_back(card.id);
    if (shown == expected) {
        rebuildHome(); // takes any refresh meanwhile, and the dock's rows of six again
        return;
    }
    saveApps(list, shown, true, false, Json(), Json());
}
// Saves the apps' order (`shown`: every app on the list in its new order), the hidden apps and, when
// given, the kinds' order, in one request. The catalogue shows the save at once; the server's answer
// replaces it on the next read.
void Application::saveApps(const std::string &list, const std::vector<std::string> &shown, bool moved,
                           bool hiddenChanged, const Json &hiddenNow, const Json &kinds) {
    Json order = Json::array();
    for (const auto &id : orderAfterMove(state_.catalogue["order"]["app"], shown))
        order.add(Json::text(id));
    Json body = Json::object();
    if (moved)
        body.set("order", fields({{"app", order}}));
    if (hiddenChanged)
        body.set("hidden", fields({{list, hiddenNow}}));
    if (kinds.isArray())
        body.set("kinds", kinds);
    auto object = [](const Json &value) { return value.isObject() ? value : Json::object(); };
    Json orderAll = object(state_.catalogue["order"]), layout = object(state_.catalogue["layout"]);
    if (moved)
        orderAll.set("app", order);
    if (hiddenChanged) {
        Json hiddenAll = object(layout["hidden"]);
        if (!hiddenAll["appsTab"].isArray()) // the server's migration: the Apps page starts as Home's
            hiddenAll.set("appsTab", hiddenAll["app"].isArray() ? hiddenAll["app"] : Json::array());
        hiddenAll.set(list, hiddenNow);
        layout.set("hidden", hiddenAll);
    }
    if (kinds.isArray()) {
        layout.set("kinds", kinds);
        Json ordered = Json::array();
        for (const Json &key : kinds.items())
            for (const Json &kind : state_.catalogue["kinds"].items())
                if (kind["key"].string() == key.string())
                    ordered.add(kind);
        state_.catalogue.set("kinds", ordered);
    }
    state_.catalogue.set("order", orderAll);
    state_.catalogue.set("layout", layout);
    rebuildHome();
    execute(post(Operation::SetHomeLayout, body));
}

// ---- Editing a kind's card on the Apps page -------------------------------------------------------

// Holding OK on an app of the Apps page sorted by kind lifts its kind's row as a card: the kind's apps
// (the hidden ones last, dimmed, to be shown again) are edited on it and Up and Down move the card.
void Application::homeKindEditStart() {
    auto &home = state_.home;
    CardRow &row = home.rows[home.row];
    const std::string focused = row.cards[home.column].id;
    row.cards.clear();
    for (const Card &card : appsRow(state_.catalogue, state_.guide, true, editedList()).cards)
        if (kindOf(card) == row.group)
            row.cards.push_back(card);
    home.column = 0;
    for (size_t i = 0; i < row.cards.size(); ++i)
        if (row.cards[i].id == focused)
            home.column = i;
    home.arranging = true;
    home.carrying = home.carryingKind = false;
    home.arrangeFocus = 0;
    homeFocus();
    dirty_ = true;
}
// Ends the card's editing: the kinds' order, this kind's apps in their order and the ones hidden or
// shown are saved in one request, if any changed.
void Application::homeArrangeDone() {
    auto &home = state_.home;
    home.arranging = home.carrying = home.carryingKind = false;
    home.arrangeFocus = 0;
    dirty_ = true;
    if (home.row >= home.rows.size()) {
        rebuildHome();
        return;
    }
    auto contains = [](const std::vector<std::string> &ids, const std::string &id) {
        return std::find(ids.begin(), ids.end(), id) != ids.end();
    };
    const std::string list = editedList();
    std::vector<std::string> hiddenBefore;
    for (const auto &id : hiddenIds(state_.catalogue, list).items())
        hiddenBefore.push_back(id.string());
    // The kind's apps still shown keep the order they have on the card; the others are the changes.
    std::vector<std::string> kept, newlyShown, newlyHidden;
    for (const Card &card : home.rows[home.row].cards) {
        const bool was = contains(hiddenBefore, card.id);
        if (card.hidden && !was)
            newlyHidden.push_back(card.id);
        else if (!card.hidden && was)
            newlyShown.push_back(card.id);
        else if (!card.hidden)
            kept.push_back(card.id);
    }
    // Every app on the list in its saved order: the kind's apps take the places the kind has, those
    // hidden now leave and those shown now join last.
    std::vector<std::string> shown, expected;
    size_t next = 0;
    for (const Card &card : appsRow(state_.catalogue, state_.guide, false, list).cards) {
        if (contains(newlyHidden, card.id))
            continue;
        expected.push_back(card.id);
        shown.push_back(contains(kept, card.id) ? kept[next++] : card.id);
    }
    for (const std::string &id : newlyShown) {
        shown.push_back(id);
        expected.push_back(id);
    }
    Json hiddenNow = Json::array();
    for (const std::string &id : hiddenBefore)
        if (!contains(newlyShown, id))
            hiddenNow.add(Json::text(id));
    for (const std::string &id : newlyHidden)
        hiddenNow.add(Json::text(id));
    // The server keeps every kind: one with no app has no row here and holds its place, the rows on
    // screen take the order they were left in.
    Json kinds = Json::array();
    size_t nextRow = 0;
    bool kindsMoved = false;
    for (const Json &kind : state_.catalogue["kinds"].items()) {
        bool onScreen = false;
        for (const CardRow &other : home.rows)
            onScreen = onScreen || other.group == kind["key"].string();
        const std::string key =
            onScreen && nextRow < home.rows.size() ? home.rows[nextRow++].group : kind["key"].string();
        kindsMoved = kindsMoved || key != kind["key"].string();
        kinds.add(Json::text(key));
    }
    const bool changed = !newlyShown.empty() || !newlyHidden.empty();
    if (shown == expected && !changed && !kindsMoved) {
        rebuildHome();
        return;
    }
    saveApps(list, shown, shown != expected, changed, hiddenNow, kindsMoved ? kinds : Json());
}
} // namespace nativetv
