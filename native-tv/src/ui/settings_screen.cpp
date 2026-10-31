// Settings: the ten sections the TV dashboard had, as Google TV lays settings out: the list of
// sections on the left (30%) and the section's page on the right (70%), both always in view. A
// section with many rows is split into subsections, shown as a row of tabs over its rows; a strip
// under the rows says what the focused row is and what OK does. The rows, and the writes OK makes,
// are the ones dashboardRows() builds from the same daemon routes the dashboard page used.
//
// Keys: Up and Down in the list move the section and the page follows; Right or OK moves the focus
// into the page, Left or Back returns it to the list, and Back in the list closes Settings. Left
// and Right never change the section.
#include "domain/say.hpp"
#include "ui/application.hpp"
#include "ui/dashboard_screens.hpp"
#include "ui/kit/tuning.hpp"
#include "ui/optimistic.hpp"
#include "ui/overlay_routes.hpp"
#include "ui/settings_list.hpp"
#include <algorithm>
#include <map>
#include <set>

namespace nativetv {
namespace {
namespace ms = kit::motion::ms;
constexpr double kPollMs = 10000;   // Metrics and System refresh while open, as the dashboard did
constexpr double kConfirmMs = 5000; // the second OK has to come within this
constexpr double kSaverDelayMs =
    800; // the screen saver ends on the key-up of the OK that starts it
constexpr double kServerWaitMs = 1000; // between asks while the server restarts
constexpr double kSettleMs = 180;      // the page waits this long on a section before reading it
constexpr int kServerTries = 30;

// The page, as the design's 30/70 split of the 1760 px between the margins.
constexpr float kListX = 80, kListW = 520, kPaneX = 640, kPaneW = 1200;
constexpr float kTop = 288, kBottom = 1028;
constexpr float kPillH = 52, kPillGap = 12, kPillPad = 22;
constexpr float kStripH = 168, kStripGap = 12;
constexpr float kRowReach = 24; // what a focused row grows by on its right, and its ring

Rect listViewport() {
    return {kListX - 16, kTop, kListW + 16 + 20, kBottom - kTop};
}
Rect rowsViewport(bool tabs) {
    float top = tabs ? kTop + kPillH + 16 : kTop;
    return {kPaneX - 16, top, kPaneW + 16 + 40, kBottom - kStripH - kStripGap - top};
}
Rect stripBox() {
    return {kPaneX, kBottom - kStripH, kPaneW, kStripH};
}

// Sections that may not exist on this TV: the dashboard dropped the tab when its data said so.
bool optionalGroup(const std::string &key) {
    return key == "oled" || key == "game" || key == "saver" || key == "home";
}

class SettingsScreen final : public ScreenView {
  public:
    // `group` is the section the route names, or empty for the first one.
    explicit SettingsScreen(const std::string &group)
        : groups_(settingGroups()), sections_("sec", {listViewport(), kListX, kListW - 16, true}),
          rows_("set", {rowsViewport(false), kPaneX, kPaneW - kRowReach, false}) {
        for (size_t i = 0; i < groups_.size(); ++i)
            if (groups_[i].key == group) {
                section_ = i;
                zone_ = Zone::Rows;
                forced_ = group;
            }
    }
    // The names the older scripts and tests know the dashboard by: the list, or a section.
    std::string name() const override {
        return zone_ == Zone::Sections ? "dashboard" : "dashboard:" + groups_[section_].key;
    }
    // Settings is a page of its own: the Home bar (Home, Apps, Inputs, Quick settings) belongs to Home, and
    // Back leaves for where Settings was opened from.
    bool hasBar() const override {
        return false;
    }
    int barTab() const override {
        return -1;
    }

    void enter(ScreenHost &host) override {
        host.motion().set("ss:head", 1);
        host.motion().tween("ss:head", 0, ms::base, kit::easing(kit::motion::ease::out),
                            kit::motion::stagger::pageBase);
        barWas_ = host.barFocused();
        probe(host);
        rebuildSections(host, SettingsList::Reset::Stagger);
        selectSection(host, section_, SettingsList::Reset::Stagger);
        wantRows_ = zone_ == Zone::Rows;       // a route that names a section opens it for use
        readAt_ = std::max(1.0, host.nowMs()); // 0 means nothing is waiting to be read
        syncFocus(host, true);
    }
    void leave(ScreenHost &host) override {
        host.services().cancel("settings:wait");
        for (const auto &group : groups_)
            host.services().cancel(dataChannel(group.key));
        sections_.clear(host);
        rows_.clear(host);
        host.motion().erasePrefix("ss:");
    }
    void focus(ScreenHost &host) override {
        syncFocus(host, true);
    }

    bool key(ScreenHost &host, Key key) override {
        switch (zone_) {
        case Zone::Sections:
            return sectionKey(host, key);
        case Zone::Tabs:
            return tabKey(host, key);
        case Zone::Rows:
            return rowKey(host, key);
        }
        return false;
    }
    bool back(ScreenHost &host) override {
        if (armed_) {
            disarm();
            host.toast(say::msg("tv.settings.cancelled", "Cancelled"));
            rebuildRows(host, SettingsList::Reset::Keep);
            return true;
        }
        if (zone_ == Zone::Sections) {
            // Where Settings was opened from; Home as it was when that was Home.
            std::string from = host.cameFrom();
            bool sibling = from.empty() || from.compare(0, 9, "dashboard") == 0 ||
                           from == "settings" || from == "catalog" || from == "live" ||
                           from.compare(0, 4, "tab:") == 0;
            host.open(sibling ? "home" : returnRoute(from));
            return true;
        }
        if (!sub_.empty()) {
            sub_.clear();
            buildPages(host, SettingsList::Reset::Fade);
            return true;
        }
        setZone(host, Zone::Sections);
        return true;
    }
    void click(ScreenHost &host, const std::string &hit) override {
        size_t index = 0;
        if (sections_.parseHit(hit, index)) {
            if (index == sections_.focus()) {
                enterPage(host);
            } else {
                sections_.select(host, index);
                setZone(host, Zone::Sections);
                sectionMoved(host);
            }
        } else if (rows_.parseHit(hit, index)) {
            setZone(host, Zone::Rows);
            if (index == rows_.focus())
                activate(host);
            else
                rows_.select(host, index);
        } else if (hit.compare(0, 9, "set:pill:") == 0) {
            size_t pill = static_cast<size_t>(std::atoi(hit.c_str() + 9));
            if (pill < pages_.size()) {
                setZone(host, Zone::Tabs);
                selectPage(host, pill);
            }
        }
    }

    void response(ScreenHost &host, const Response &response) override {
        const std::string &channel = response.channel;
        if (channel.compare(0, 14, "settings:data:") == 0)
            arrived(host, channel.substr(14), response);
        else if (channel == "settings:wait")
            waited(host, response);
        else if (channel == "caps")
            capsChanged_ =
                true; // the shell keeps them after this call: the rows follow at the next frame
    }

    bool update(ScreenHost &host) override {
        const double now = host.nowMs();
        syncFocus(host, false);
        sections_.update(host);
        rows_.update(host);
        if (capsChanged_) {
            capsChanged_ = false;
            buildPages(host, SettingsList::Reset::Keep);
        }
        if (armed_ && now >= armedUntil_) {
            disarm();
            rebuildRows(host, SettingsList::Reset::Keep);
        }
        if (saverAt_ && now >= saverAt_) {
            saverAt_ = 0;
            Write start;
            start.operation = saverCommand_.operation;
            Json body = saverCommand_.body;
            start.body = [body](const Json &) { return body; };
            host.optimistic().run(saverKey_, start);
        }
        auto refused = host.memory().find("settings.switchFailed");
        if (refused != host.memory().end()) {
            host.memory().erase(refused);
            switching_ = false;
            rebuildRows(host, SettingsList::Reset::Keep);
        }
        // The server is restarting to apply the network switch: ask until it answers again.
        auto restart = host.memory().find("settings.restart");
        if (restart != host.memory().end()) {
            host.memory().erase(restart);
            host.toast(say::msg("tv.net.restarting", "Applying it, the server restarts…"));
            waitAt_ = now + 1500;
            waitTries_ = kServerTries;
            waiting_ = false;
        }
        if (waitAt_ && now >= waitAt_ && !waiting_) {
            waiting_ = host.services().read("settings:wait", Operation::Setup) != 0;
            if (!waiting_)
                waitAt_ = now + kServerWaitMs;
        }
        if (readAt_ && now >= readAt_ && !waitAt_) {
            readAt_ = 0;
            readSection(host);
        }
        if (!readAt_ && pollAt_ && now >= pollAt_ && !waitAt_) {
            pollAt_ = 0;
            readSection(host);
        }
        // A change the layer holds, answers or takes back moves the rows without any key.
        if (page_ < pages_.size() && (host.optimistic().active() || held_) &&
            stateSignature(host) != signature_)
            rebuildRows(host, SettingsList::Reset::Keep);
        return host.optimistic().active();
    }

    void build(ScreenHost &host, kit::Paint paint) override {
        Motion &motion = host.motion();
        double head = motion.value("ss:head");
        kit::PageHead page;
        page.title = say::msg("tv.settings.title", "Settings");
        page.subtitle = say::msg("tv.settings.subtitle", "Changes apply as soon as they are made.");
        page.enter = {static_cast<float>(1 - head), static_cast<float>(28 * head)};
        kit::pageHead(paint, page);
        sections_.build(host, paint);
        if (loading()) {
            kit::Loading spinner;
            spinner.area = {kPaneX, kTop, kPaneW, kBottom - kStripH - kStripGap - kTop};
            spinner.label = say::msg("tv.loading", "Loading…");
            spinner.phase = kit::spinnerPhase(motion, "ss:spin");
            kit::loadingIndicator(paint, spinner);
        } else {
            if (showTabs())
                buildTabs(host, paint);
            rows_.build(host, paint);
        }
        // The strip names the focused section while the focus is in the list, else the focused row.
        if (zone_ == Zone::Sections || !loading())
            (zone_ == Zone::Sections ? sections_ : rows_).buildStrip(host, paint, stripBox());
    }

    Json snapshot() const override {
        Json value = Json::object();
        static const char *const zones[] = {"sections", "tabs", "rows"};
        value.set("zone", Json::text(zones[static_cast<int>(zone_)]));
        value.set("level", Json::text(zone_ == Zone::Sections ? "groups" : "rows"));
        value.set("section", Json::text(groups_[section_].key));
        value.set("group", Json::text(groups_[section_].key));
        value.set("page", Json::text(pages_.size() > page_ ? pages_[page_].title : ""));
        value.set("pages", Json::numeric(static_cast<double>(pages_.size())));
        value.set("sub", Json::text(sub_));
        const SettingItem *item = zone_ == Zone::Sections ? sections_.current() : rows_.current();
        value.set("focus", Json::text(item ? item->title : ""));
        value.set("focusIndex", Json::numeric(static_cast<double>(zone_ == Zone::Sections ? sections_.focus() : rows_.focus())));
        value.set("focusId", Json::text(item ? item->id : ""));
        static const char *const kinds[] = {"heading", "info", "toggle", "choice", "action", "link"};
        value.set("focusKind", Json::text(item ? kinds[static_cast<int>(item->kind)] : ""));
        value.set("focusOn", Json::flag(item && item->on));
        value.set("focusValue", Json::text(item ? item->value : ""));
        value.set("focusPending", Json::flag(item && item->pending));
        value.set("rows", Json::numeric(static_cast<double>(rows_.items().size())));
        value.set("focusLast",
                  Json::flag(!rows_.items().empty() && rows_.focus() + 1 == rows_.items().size()));
        value.set("sections", Json::numeric(static_cast<double>(sections_.items().size())));
        value.set("loading", Json::flag(loading()));
        value.set("busy", Json::flag(waitAt_ != 0 || switching_));
        value.set("confirming", Json::flag(armed_));
        value.set("barFocus", Json::flag(barWas_));
        return value;
    }

  private:
    enum class Zone { Sections, Tabs, Rows };
    using Reset = SettingsList::Reset;

    // ---- keys --------------------------------------------------------------------------------

    bool sectionKey(ScreenHost &host, Key key) {
        if (key == Key::Up || key == Key::Down || key == Key::PageUp || key == Key::PageDown) {
            int direction = key == Key::Up || key == Key::PageUp ? -1 : 1;
            bool moved =
                sections_.move(host, direction, key == Key::Up || key == Key::Down ? 1 : 6);
            if (moved)
                sectionMoved(host);
            return moved || key != Key::Up; // the top section: the bar's
        }
        if (key == Key::Right || key == Key::Ok) {
            enterPage(host);
            return true;
        }
        return key == Key::Left; // never changes the section
    }
    bool tabKey(ScreenHost &host, Key key) {
        if (key == Key::Left) {
            if (page_ > 0)
                selectPage(host, page_ - 1);
            else
                setZone(host, Zone::Sections);
            return true;
        }
        if (key == Key::Right) {
            if (page_ + 1 < pages_.size())
                selectPage(host, page_ + 1);
            return true;
        }
        if (key == Key::Down || key == Key::Ok) {
            if (!rows_.items().empty())
                setZone(host, Zone::Rows);
            return true;
        }
        return key != Key::Up; // Up from the tabs is the bar's
    }
    bool rowKey(ScreenHost &host, Key key) {
        if (key == Key::Up || key == Key::Down || key == Key::PageUp || key == Key::PageDown) {
            int direction = key == Key::Up || key == Key::PageUp ? -1 : 1;
            bool moved = rows_.move(host, direction, key == Key::Up || key == Key::Down ? 1 : 6);
            if (!moved && key == Key::Up) {
                // The first row: the tabs above it, if there are any, else the bar.
                if (showTabs()) {
                    setZone(host, Zone::Tabs);
                    return true;
                }
                return false;
            }
            return true;
        }
        if (key == Key::Ok) {
            activate(host);
            return true;
        }
        // On a row that steps through values, Left and Right step through them (OK steps forward); the way
        // back to the sections is Back.
        const SettingItem *shown = rows_.current();
        const bool steps = shown && shown->kind == SettingItem::Kind::Choice && !shown->choices.empty() &&
                           !shown->command.empty();
        if (steps && (key == Key::Left || key == Key::Right)) {
            activate(host, key == Key::Right ? 1 : -1);
            return true;
        }
        if (key == Key::Left) {
            if (!sub_.empty())
                back(host);
            else
                setZone(host, Zone::Sections);
            return true;
        }
        return key == Key::Right;
    }

    // ---- zones, sections and pages --------------------------------------------------------------

    bool loading() const {
        return !data_.count(groups_[section_].key);
    }
    bool showTabs() const {
        return pages_.size() > 1 && sub_.empty();
    }
    std::vector<size_t> visible() const {
        std::vector<size_t> shown;
        for (size_t i = 0; i < groups_.size(); ++i)
            if (!hidden_.count(groups_[i].key) || groups_[i].key == forced_)
                shown.push_back(i);
        return shown;
    }
    Operation operationOf(size_t group) const {
        for (const Tab &tab : dashboardTabs())
            if (tab.key == groups_[group].key)
                return tab.operation;
        return Operation::None;
    }
    void setZone(ScreenHost &host, Zone zone) {
        if (zone == zone_)
            return;
        zone_ = zone;
        syncFocus(host, true);
        // The strip now speaks of the other list's focus.
        (zone_ == Zone::Sections ? sections_ : rows_).refreshDetail(host);
        host.invalidate();
    }
    // Right or OK on a section: the focus goes into its page, or waits for it to arrive.
    void enterPage(ScreenHost &host) {
        if (loading() || rows_.items().empty()) {
            wantRows_ = true;
            setZone(host, Zone::Rows);
            return;
        }
        setZone(host, Zone::Rows);
        rows_.focusFirst(host);
    }
    // The focus moved to another section: the page follows, and reads after the focus settles.
    void sectionMoved(ScreenHost &host) {
        std::vector<size_t> shown = visible();
        if (sections_.focus() >= shown.size())
            return;
        size_t group = shown[sections_.focus()];
        if (group == section_)
            return;
        selectSection(host, group, Reset::Fade);
        readAt_ = host.nowMs() + kSettleMs;
    }
    void selectSection(ScreenHost &host, size_t group, Reset reset) {
        section_ = group;
        sub_.clear();
        disarm();
        pollAt_ = 0;
        wantRows_ = false;
        pages_.clear();
        page_ = 0;
        // The section list's focus is where the section is.
        size_t row = 0;
        for (size_t index : visible()) {
            if (index == group)
                break;
            ++row;
        }
        sections_.select(host, row);
        buildPages(host, reset);
    }
    void selectPage(ScreenHost &host, size_t page) {
        if (page >= pages_.size() || page == page_)
            return;
        page_ = page;
        lastPage_[groups_[section_].key] = pages_[page_].title;
        rebuildRows(host, Reset::Fade);
        syncFocus(host, true);
    }

    // ---- data -------------------------------------------------------------------------------

    static std::string dataChannel(const std::string &key) {
        return "settings:data:" + key;
    }
    // The sections that may be missing are asked once, so the list can leave them out. What the
    // last opening read shows at once, and is asked again behind it.
    void probe(ScreenHost &host) {
        for (size_t i = 0; i < groups_.size(); ++i) {
            const std::string &key = groups_[i].key;
            auto kept = host.memory().find("settings.data." + key);
            if (kept != host.memory().end() && !data_.count(key)) {
                data_[key] = kept->second;
                if (kept->second["available"].valid() && !kept->second["available"].boolean())
                    hidden_.insert(key);
            }
            if (optionalGroup(key) && i != section_ && !data_.count(key))
                host.services().read(dataChannel(key), operationOf(i));
        }
    }
    void readSection(ScreenHost &host) {
        host.services().read(dataChannel(groups_[section_].key), operationOf(section_));
    }
    void arrived(ScreenHost &host, const std::string &key, const Response &response) {
        const bool probing = key != groups_[section_].key;
        Json data = response.json();
        bool good = response.ok() && data.valid();
        if (!good)
            data = fields(
                {{"ok", Json::flag(false)},
                 {"error", Json::text(say::msg("tv.settings.noAnswer",
                                               "The server did not answer. Press OK to retry."))}});
        if (probing && !good)
            return; // a section stays listed until it says it is not there
        data_[key] = data;
        if (good)
            host.memory()["settings.data." + key] = data;
        bool gone = good && data["available"].valid() && !data["available"].boolean();
        if (gone)
            hidden_.insert(key);
        if (probing || gone) {
            rebuildSections(host, Reset::Keep);
            if (gone && key == groups_[section_].key && key != forced_) {
                // The section that was showing does not exist here: the next one shows.
                std::vector<size_t> shown = visible();
                if (!shown.empty())
                    selectSection(host, shown[std::min(sections_.focus(), shown.size() - 1)],
                                  Reset::Fade);
            }
            if (probing)
                return;
        }
        bool first = pages_.empty();
        pollAt_ = host.nowMs() + (key == "system" || key == "options" ? kPollMs : 0);
        buildPages(host, first ? Reset::Stagger : Reset::Keep);
        rebuildSections(host, Reset::Keep);
        // The focus was waiting for the rows (OK on a section that was still loading).
        if (wantRows_ && !rows_.items().empty()) {
            wantRows_ = false;
            rows_.focusFirst(host);
        }
    }

    // ---- what is listed -----------------------------------------------------------------------

    void rebuildSections(ScreenHost &host, Reset reset) {
        std::vector<SettingItem> items;
        for (size_t index : visible()) {
            const SettingGroup &group = groups_[index];
            SettingItem item;
            item.id = group.key;
            item.kind = SettingItem::Kind::Link;
            item.icon = group.icon;
            item.title = group.title;
            auto cached = data_.find(group.key);
            item.value = cached == data_.end() ? "" : settingGroupStatus(group.key, cached->second);
            item.body = group.description;
            item.action = say::msg("tv.settings.open", "Open");
            if (!item.value.empty())
                item.chips.push_back(item.value);
            items.push_back(item);
        }
        sections_.set(host, std::move(items), reset);
        if (reset != Reset::Keep) {
            size_t row = 0;
            for (size_t index : visible()) {
                if (index == section_)
                    break;
                ++row;
            }
            sections_.select(host, row);
        }
    }
    // The pages of the section from the data it last loaded; the page shown stays the page shown.
    void buildPages(ScreenHost &host, Reset reset) {
        const std::string &key = groups_[section_].key;
        auto cached = data_.find(key);
        if (cached == data_.end()) {
            pages_.clear();
            rows_.set(host, {}, Reset::Fade);
            return;
        }
        std::vector<MenuRow> rows = dashboardRows(key, cached->second, host.view().caps, sub_);
        // The dashboard is dark only; its Theme row would change nothing.
        rows.erase(std::remove_if(rows.begin(), rows.end(),
                                  [](const MenuRow &row) { return row.command.route == "theme"; }),
                   rows.end());
        std::string was = pages_.size() > page_ ? pages_[page_].title : lastPage_[key];
        pages_ = sub_.empty() ? settingPages(key, rows) : std::vector<SettingPage>{SettingPage()};
        if (!sub_.empty())
            pages_[0].items = settingItems(key, rows);
        // Metrics are the page Live shows as cards; the way there is the first row.
        if (key == "system" && sub_.empty()) {
            SettingItem live;
            live.id = "live-link";
            live.kind = SettingItem::Kind::Link;
            live.icon = "speed";
            live.title = say::msg("tv.settings.liveLink", "Live");
            live.body = say::msg("tv.settings.liveLink.desc",
                                 "The same readings as cards, refreshed every few seconds.");
            live.action = say::msg("tv.settings.open", "Open");
            live.command = navigate("live");
            pages_[0].items.insert(pages_[0].items.begin(), live);
        }
        size_t at = 0;
        bool found = false;
        for (size_t i = 0; i < pages_.size(); ++i)
            if (pages_[i].title == was) {
                at = i;
                found = true;
            }
        // A refresh that no longer has the page that was showing starts over on the first.
        if (reset == Reset::Keep && !found)
            reset = Reset::Fade;
        page_ = at;
        if (sub_.empty())
            lastPage_[key] = pages_[page_].title;
        rebuildRows(host, reset);
    }
    void rebuildRows(ScreenHost &host, Reset reset) {
        if (page_ >= pages_.size()) {
            rows_.set(host, {}, Reset::Fade);
            return;
        }
        std::vector<SettingItem> items = pages_[page_].items;
        decorate(host, items);
        rows_.moveViewport(host, rowsViewport(showTabs()));
        rows_.set(host, std::move(items), reset);
        host.invalidate();
    }
    // What each row reads now: what the TV last said, with what the person has asked for on top
    // while the layer holds it, and the row's state while a second OK or the TV is awaited.
    void decorate(ScreenHost &host, std::vector<SettingItem> &items) {
        Optimistic &layer = host.optimistic();
        signature_.clear();
        held_ = false;
        for (SettingItem &item : items) {
            if (!item.focusable())
                continue;
            const std::string key = writeKey(item);
            if (item.kind == SettingItem::Kind::Toggle) {
                const bool server = item.on;
                item.on = layer.flag(key, item.on);
                held_ = held_ || item.on != server;
                item.action = item.on ? say::msg("tv.settings.turnOff", "Turn off")
                                      : say::msg("tv.settings.turnOn", "Turn on");
                signature_ += item.on ? 'T' : 'F';
            } else if (item.kind == SettingItem::Kind::Choice && !item.choices.empty()) {
                const std::string server = item.value;
                item.value = layer.text(key, item.value);
                held_ = held_ || item.value != server;
                item.chips = {item.value};
                signature_ += item.value;
            }
            item.pending = layer.pending(key);
            held_ = held_ || item.pending;
            signature_ += item.pending ? 'p' : '.';
            if (item.pending) {
                // Its words describe the state before the change.
                if (item.kind == SettingItem::Kind::Toggle)
                    item.chips.clear();
                item.action.clear();
            }
            if (switching_ && item.id == switchingId_) {
                item.value = say::msg("tv.settings.switching", "Switching…");
                item.chips = {item.value};
                item.pending = true;
            }
            if (armed_ && item.id == armedId_) {
                item.value = say::msg("tv.confirm", "Confirm?");
                item.action = say::msg("tv.settings.pressAgain", "Press OK again to confirm");
                item.chips = {say::msg("tv.settings.backCancels", "Back cancels")};
            }
        }
    }
    // What decorate() would make of the layer's state, as text, without making the rows.
    std::string stateSignature(ScreenHost &host) const {
        Optimistic &layer = host.optimistic();
        std::string text;
        for (const SettingItem &base : pages_[page_].items) {
            if (!base.focusable())
                continue;
            const std::string key = writeKey(base);
            if (base.kind == SettingItem::Kind::Toggle)
                text += layer.flag(key, base.on) ? 'T' : 'F';
            else if (base.kind == SettingItem::Kind::Choice && !base.choices.empty())
                text += layer.text(key, base.value);
            text += layer.pending(key) ? 'p' : '.';
        }
        return text;
    }
    // The rows draw from this key: the section and the row, so another section's change is not
    // mistaken for this one's.
    std::string writeKey(const SettingItem &item) const {
        return "settings/" + groups_[section_].key + "/" + item.id;
    }
    // The rows as the TV last described them (before what is asked for lies on top).
    const SettingItem *baseOf(const std::string &id) const {
        if (page_ >= pages_.size())
            return nullptr;
        for (const SettingItem &item : pages_[page_].items)
            if (item.id == id && item.focusable())
                return &item;
        return nullptr;
    }

    // ---- doing --------------------------------------------------------------------------------

    void activate(ScreenHost &host, int direction = 1) {
        const SettingItem *shown = rows_.current();
        if (!shown || !shown->focusable() || shown->command.empty())
            return;
        const SettingItem *base = baseOf(shown->id);
        if (!base)
            return;
        Command command = base->command;
        if (!command.route.empty()) {
            follow(host, command.route);
            return;
        }
        if (waitAt_ || switching_)
            return;
        // What this press asks for, and the value it ends on.
        Json target, server;
        std::string key = writeKey(*base);
        Optimistic &layer = host.optimistic();
        if (base->kind == SettingItem::Kind::Toggle) {
            bool wanted = !layer.flag(key, base->on);
            command = wanted != base->on ? base->command : base->reverse;
            target = Json::flag(wanted);
            server = Json::flag(base->on);
        } else if (base->kind == SettingItem::Kind::Choice && !base->choices.empty()) {
            // The next value after the one shown, as OK steps through them.
            std::string current = layer.text(key, base->value);
            const size_t count = base->choices.size();
            size_t at = 0; // where the value shown is
            bool found = false;
            for (size_t i = 0; i < count; ++i)
                if (base->choices[i].first == current) {
                    at = i;
                    found = true;
                }
            const size_t chosen = direction < 0 ? (found ? (at + count - 1) % count : count - 1)
                                                : (found ? (at + 1) % count : 0);
            const auto &next = base->choices[chosen];
            command = next.second;
            target = Json::text(next.first);
            server = Json::text(base->value);
        }
        if (command.operation == Operation::None && command.route.empty())
            return; // nothing this row can do
        // A change that is hard to undo asks twice, within a few seconds; the second OK is the
        // change.
        if (command.confirm && !(armed_ && armedId_ == base->id)) {
            armed_ = true;
            armedId_ = base->id;
            armedUntil_ = host.nowMs() + kConfirmMs;
            host.toast(
                say::msg("tv.settings.confirmToast", "Press OK again to confirm. Back cancels."));
            rebuildRows(host, Reset::Keep);
            return;
        }
        disarm();
        if (command.body["action"].string() == "screensaver") {
            // Started once the OK that chose it has been let go, or it would end the saver at once.
            saverAt_ = host.nowMs() + kSaverDelayMs;
            saverCommand_ = command;
            saverKey_ = key;
            host.toast(say::msg("tv.flash.startingSaver", "Starting the screen saver…"));
            return;
        }
        submit(host, *base, key, command, target, server);
    }
    void disarm() {
        armed_ = false;
        armedId_.clear();
    }
    void follow(ScreenHost &host, const std::string &route) {
        if (route == "refresh") {
            readSection(host);
            return;
        }
        if (route.compare(0, 4, "sub:") == 0) {
            sub_ = route.substr(4);
            buildPages(host, Reset::Fade);
            return;
        }
        host.open(route);
    }
    // Every change goes through the layer: a switch or a stepper shows its new value from the next
    // frame, anything else is pending until the TV answers. The callbacks hold the host, never the
    // page, which may be gone by then.
    void submit(ScreenHost &host, const SettingItem &item, const std::string &key,
                const Command &command, const Json &target, const Json &server) {
        const std::string section = groups_[section_].key;
        const Operation read = operationOf(section_);
        const bool network =
            command.operation == Operation::Setup && command.body["action"].string() == "network";
        // A switch or a stepper shows its own answer; only the rest says it was saved.
        const bool quiet =
            item.kind == SettingItem::Kind::Toggle || item.kind == SettingItem::Kind::Choice;
        ScreenHost *world = &host;
        Write write;
        write.operation = command.operation;
        Json body = command.body;
        write.body = [body](const Json &) { return body; };
        write.failure = say::msg("tv.failed", "That did not work");
        write.readChannel = dataChannel(section);
        write.readOperation = read;
        write.done = [world, quiet, network](bool ok, const Json &) {
            if (!ok) {
                if (network) // the switch is off the screen again, and the row can be pressed
                    world->memory()["settings.switchFailed"] = Json::flag(true);
                return;
            }
            if (network) // the server restarts to apply it: the page waits for it to answer again
                world->memory()["settings.restart"] = Json::flag(true);
            else if (!quiet)
                world->toast(say::msg("tv.settings.saved", "Saved"));
        };
        if (target.valid()) {
            host.optimistic().set(key, target, server, write);
            if (network) {
                switching_ = true;
                switchingId_ = item.id;
            }
        } else {
            // Nothing to show of it but that it is on its way; the section is read again after.
            write.readOperation = Operation::None;
            write.done = [world, section, read](bool ok, const Json &) {
                if (ok) {
                    world->toast(say::msg("tv.settings.saved", "Saved"));
                    world->services().read(dataChannel(section), read);
                }
            };
            host.optimistic().run(key, write);
        }
        rebuildRows(host, Reset::Keep);
    }
    // The server came back from a restart, or did not.
    void waited(ScreenHost &host, const Response &response) {
        waiting_ = false;
        Json data = response.json();
        if (response.ok() && data.valid()) {
            waitAt_ = 0;
            switching_ = false;
            readSection(host);
            rebuildRows(host, Reset::Keep);
            return;
        }
        if (--waitTries_ <= 0) {
            waitAt_ = 0;
            switching_ = false;
            host.toast(say::msg("tv.net.notBack", "It did not come back. Try again."));
            rebuildRows(host, Reset::Keep);
            return;
        }
        waitAt_ = host.nowMs() + kServerWaitMs;
    }

    // ---- drawing ------------------------------------------------------------------------------

    // The page draws no focus while the top bar has it, and only one part of the page has it.
    void syncFocus(ScreenHost &host, bool force) {
        bool bar = host.barFocused();
        if (!force && bar == barWas_)
            return;
        barWas_ = bar;
        sections_.show(host, !bar && zone_ == Zone::Sections);
        rows_.show(host, !bar && zone_ == Zone::Rows);
        Motion &motion = host.motion();
        for (size_t i = 0; i < pages_.size(); ++i)
            motion.tween("ss:pf:" + std::to_string(i),
                         !bar && zone_ == Zone::Tabs && i == page_ ? 1 : 0, ms::focus,
                         kit::easing(kit::motion::ease::out));
        host.invalidate();
    }
    // The subsections as a row of pills over the rows: the shown one is filled, the one with the
    // focus is lit like the focused row.
    void buildTabs(ScreenHost &host, kit::Paint paint) {
        Motion &motion = host.motion();
        Scene &scene = paint.scene;
        // The pills' padding gives way where their titles would not fit the page.
        std::vector<float> text;
        float words = 0;
        for (const SettingPage &page : pages_) {
            text.push_back(paint.glyphs.measure(page.title, kit::type::labelL));
            words += text.back();
        }
        float gaps = kPillGap * static_cast<float>(pages_.size() - 1);
        float pad = std::max(8.0f, std::min(kPillPad, (kPaneW - gaps - words) /
                                                          (2 * static_cast<float>(pages_.size()))));
        float x = kPaneX;
        for (size_t i = 0; i < pages_.size(); ++i) {
            float width = text[i] + 2 * pad;
            Rect rest = {x, kTop, width, kPillH};
            float focus = static_cast<float>(motion.value("ss:pf:" + std::to_string(i)));
            bool shown = i == page_;
            Color fill = kit::focus::mix(shown ? kit::color::pillRest : Color{0, 0, 0, 0},
                                         kit::color::focusFill, focus);
            scene.shape(rest, fill, kPillH / 2);
            Color ink = kit::focus::inkAt(shown ? kit::color::onSurface : kit::color::onSurface2,
                                          kit::color::onFocus, focus);
            paint.glyphs.text(
                scene,
                {x, kTop + (kPillH - kit::type::labelL.line) / 2, width, kit::type::labelL.line},
                pages_[i].title, kit::type::labelL, ink, Node::Align::Center);
            scene.hits.push_back({rest, "set:pill:" + std::to_string(i)});
            x += width + kPillGap;
        }
    }

    std::vector<SettingGroup> groups_;
    SettingsList sections_, rows_;
    Zone zone_ = Zone::Sections;
    size_t section_ = 0, page_ = 0;
    std::string forced_; // the section the route named: listed even where the TV lacks it
    std::vector<SettingPage> pages_;
    std::map<std::string, std::string> lastPage_; // the page each section was left on
    bool barWas_ = false, capsChanged_ = false, wantRows_ = false;
    std::string sub_;
    std::map<std::string, Json> data_;
    std::set<std::string> hidden_;
    double pollAt_ = 0, readAt_ = 0;
    // The second OK a risky change waits for, and the network switch while the server restarts.
    bool armed_ = false, switching_ = false;
    std::string armedId_, switchingId_;
    Command saverCommand_;
    std::string saverKey_;
    double armedUntil_ = 0, saverAt_ = 0, waitAt_ = 0;
    bool waiting_ = false;
    int waitTries_ = 0;
    // What the rows showed of the layer's changes last time: when it differs they are drawn again.
    std::string signature_;
    bool held_ = false; // the last rows drew something the layer is holding
};
} // namespace

std::unique_ptr<ScreenView> makeSettingsScreen(const std::string &route) {
    if (route == "dashboard" || route == "settings" || route == "tab:settings")
        return std::unique_ptr<ScreenView>(new SettingsScreen(""));
    if (route.compare(0, 10, "dashboard:") == 0) {
        std::string key = route.substr(10);
        for (const SettingGroup &group : settingGroups())
            if (group.key == key)
                return std::unique_ptr<ScreenView>(new SettingsScreen(key));
    }
    return nullptr;
}
} // namespace nativetv
