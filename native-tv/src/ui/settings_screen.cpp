// Settings: the ten sections the TV dashboard had, as a list of sections and then a list of rows
// with a panel that says what the focused one does (spec 3.5). The rows, and the writes OK makes,
// are the ones dashboardRows() builds from the same daemon routes the dashboard page used.
#include "domain/say.hpp"
#include "ui/application.hpp"
#include "ui/dashboard_screens.hpp"
#include "ui/kit/tuning.hpp"
#include "ui/settings_list.hpp"
#include <algorithm>
#include <map>
#include <set>

namespace nativetv {
namespace {
constexpr double kPollMs = 10000;   // Metrics and System refresh while open, as the dashboard did
constexpr double kConfirmMs = 5000; // the second OK has to come within this
constexpr double kSaverDelayMs =
    800; // the screen saver ends on the key-up of the OK that starts it
constexpr double kServerWaitMs = 1000; // between asks while the server restarts
constexpr int kServerTries = 30;

Json vars(std::initializer_list<std::pair<const char *, std::string>> values) {
    Json result = Json::object();
    for (const auto &value : values)
        result.set(value.first, Json::text(value.second));
    return result;
}
std::string signature(const Command &command) {
    return std::to_string(static_cast<int>(command.operation)) + command.route +
           command.body.dump();
}
// Groups that may not exist on this TV: the dashboard dropped the tab when its data said so.
bool optionalGroup(const std::string &key) {
    return key == "oled" || key == "game" || key == "saver" || key == "home";
}

class SettingsScreen final : public ScreenView {
  public:
    // `group` is the section the route names, or empty for the list of sections.
    explicit SettingsScreen(const std::string &group) : groups_(settingGroups()), list_("set") {
        for (size_t i = 0; i < groups_.size(); ++i)
            if (groups_[i].key == group) {
                group_ = i;
                level_ = Level::Rows;
                direct_ = true;
            }
    }
    // The names the older scripts and tests know the dashboard by.
    std::string name() const override {
        return level_ == Level::Rows ? "dashboard:" + groups_[group_].key : "dashboard";
    }
    int barTab() const override {
        return -1; // the gear owns the page
    }

    void enter(ScreenHost &host) override {
        host.motion().set("set:E:head", 1);
        host.motion().tween("set:E:head", 0, kit::motion::ms::base,
                            kit::easing(kit::motion::ease::out), kit::motion::stagger::pageBase);
        barWas_ = host.barFocused();
        probe(host);
        if (level_ == Level::Rows)
            openGroup(host, group_);
        else
            showGroups(host, true);
    }
    void leave(ScreenHost &host) override {
        for (const char *channel : {"settings:data", "settings:write", "settings:wait"})
            host.services().cancel(channel);
        for (const auto &group : groups_)
            if (optionalGroup(group.key))
                host.services().cancel("settings:probe:" + group.key);
        list_.clear(host);
        host.motion().erasePrefix("set:");
    }
    void focus(ScreenHost &host) override {
        syncFocus(host);
    }

    bool key(ScreenHost &host, Key key) override {
        if (key == Key::Up) {
            if (list_.move(host, -1))
                return true;
            return false; // the top row: the bar's
        }
        if (key == Key::Down || key == Key::PageDown || key == Key::PageUp) {
            list_.move(host, key == Key::PageUp ? -1 : 1, key == Key::Down ? 1 : 6);
            return true;
        }
        if (key == Key::Ok) {
            activate(host);
            return true;
        }
        // Left leaves a list inside a section, as the dashboard's Left did.
        if (key == Key::Left && level_ == Level::Rows && !sub_.empty()) {
            back(host);
            return true;
        }
        // Left and Right step through the sections, as they did through the dashboard's tabs.
        if ((key == Key::Left || key == Key::Right) && level_ == Level::Rows && sub_.empty()) {
            std::vector<size_t> shown = visible();
            auto at = std::find(shown.begin(), shown.end(), group_);
            if (at != shown.end()) {
                size_t position = static_cast<size_t>(at - shown.begin());
                position = (position + shown.size() + (key == Key::Right ? 1 : -1)) % shown.size();
                disarm();
                sub_.clear();
                openGroup(host, shown[position]);
            }
            return true;
        }
        return key == Key::Left || key == Key::Right;
    }
    bool back(ScreenHost &host) override {
        if (armed_) {
            disarm();
            host.toast(say::msg("tv.settings.cancelled", "Cancelled"));
            rebuild(host, false);
            return true;
        }
        if (level_ == Level::Rows && !sub_.empty()) {
            sub_.clear();
            rebuild(host, true);
            return true;
        }
        if (level_ == Level::Rows && !direct_) {
            showGroups(host, true);
            return true;
        }
        return false;
    }
    void click(ScreenHost &host, const std::string &hit) override {
        size_t index = 0;
        if (!list_.parseHit(hit, index))
            return;
        if (index == list_.focus())
            activate(host);
        else
            list_.select(host, index);
    }

    void response(ScreenHost &host, const Response &response) override {
        const std::string &channel = response.channel;
        if (channel == "settings:data")
            arrived(host, readingKey_, response, false);
        else if (channel.compare(0, 15, "settings:probe:") == 0)
            arrived(host, channel.substr(15), response, true);
        else if (channel == "settings:write")
            written(host, response);
        else if (channel == "settings:wait")
            waited(host, response);
        else if (channel == "caps")
            capsChanged_ =
                true; // the shell keeps them after this call: the rows follow at the next frame
    }

    bool update(ScreenHost &host) override {
        const double now = host.nowMs();
        syncFocus(host);
        list_.update(host);
        if (capsChanged_) {
            capsChanged_ = false;
            rebuild(host, false);
        }
        if (armed_ && now >= armedUntil_) {
            disarm();
            rebuild(host, false);
        }
        if (saverAt_ && now >= saverAt_) {
            saverAt_ = 0;
            write(host, saverCommand_);
        }
        if (waitAt_ && now >= waitAt_ && !waiting_) {
            waiting_ = host.services().read("settings:wait", Operation::Setup) != 0;
            if (!waiting_)
                waitAt_ = now + kServerWaitMs;
        }
        if (level_ == Level::Rows && !busy_ && !waitAt_ && pollAt_ && now >= pollAt_) {
            pollAt_ = 0;
            readGroup(host);
        }
        return false;
    }

    void build(ScreenHost &host, kit::Paint paint) override {
        double head = host.motion().value("set:E:head");
        kit::PageHead page;
        if (level_ == Level::Groups) {
            page.title = say::msg("tv.settings.title", "Settings");
            page.subtitle =
                say::msg("tv.settings.subtitle", "Changes apply as soon as they are made.");
        } else {
            const SettingGroup &group = groups_[group_];
            page.title = sub_.empty() ? group.title : subTitle();
            page.subtitle = sub_.empty()
                                ? group.description
                                : say::msg("tv.settings.backTo", "Back returns to {group}.",
                                           vars({{"group", group.title}}));
        }
        page.enter = {static_cast<float>(1 - head), static_cast<float>(28 * head)};
        kit::pageHead(paint, page);
        list_.build(host, paint, "");
    }

    Json snapshot() const override {
        Json value = Json::object();
        value.set("level", Json::text(level_ == Level::Groups ? "groups" : "rows"));
        value.set("group", Json::text(level_ == Level::Rows ? groups_[group_].key : ""));
        value.set("sub", Json::text(sub_));
        const SettingItem *item = list_.current();
        value.set("focus", Json::text(item ? item->title : ""));
        value.set("rows", Json::numeric(static_cast<double>(list_.items().size())));
        value.set("busy", Json::flag(busy_ || waitAt_ != 0));
        value.set("confirming", Json::flag(armed_));
        return value;
    }

  private:
    enum class Level { Groups, Rows };

    std::vector<size_t> visible() const {
        std::vector<size_t> shown;
        for (size_t i = 0; i < groups_.size(); ++i)
            if (!hidden_.count(groups_[i].key))
                shown.push_back(i);
        return shown;
    }
    std::string subTitle() const {
        if (sub_ == "consents")
            return say::msg("tv.privacy.head.dataConsents", "Data consents");
        if (sub_ == "channels")
            return say::msg("home.ch.addTitle", "Add channels");
        return sub_;
    }
    Operation operationOf(size_t group) const {
        for (const Tab &tab : dashboardTabs())
            if (tab.key == groups_[group].key)
                return tab.operation;
        return Operation::None;
    }

    // ---- data -------------------------------------------------------------------------------

    // The sections that may be missing are asked once, so the list can leave them out.
    void probe(ScreenHost &host) {
        for (size_t i = 0; i < groups_.size(); ++i)
            if (optionalGroup(groups_[i].key) && !(level_ == Level::Rows && i == group_) &&
                !data_.count(groups_[i].key))
                host.services().read("settings:probe:" + groups_[i].key, operationOf(i));
    }
    void readGroup(ScreenHost &host) {
        readingKey_ = groups_[group_].key;
        host.services().read("settings:data", operationOf(group_));
    }
    void arrived(ScreenHost &host, const std::string &key, const Response &response, bool probing) {
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
        if (good && data["available"].valid() && !data["available"].boolean())
            hidden_.insert(key);
        if (probing || level_ == Level::Groups) {
            if (level_ == Level::Groups)
                showGroups(host, false);
            return;
        }
        if (key != groups_[group_].key)
            return;
        pollAt_ = host.nowMs() + (key == "system" || key == "options" ? kPollMs : 0);
        // A section that turned out not to exist leaves the page it was opened from.
        if (hidden_.count(key) && !direct_) {
            showGroups(host, true);
            return;
        }
        rebuild(host, !loadedOnce_.count(key));
        loadedOnce_.insert(key);
    }

    // ---- what is listed -----------------------------------------------------------------------

    void showGroups(ScreenHost &host, bool fresh) {
        level_ = Level::Groups;
        sub_.clear();
        std::vector<SettingItem> items;
        for (size_t index : visible()) {
            const SettingGroup &group = groups_[index];
            SettingItem item;
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
        list_.set(host, std::move(items), fresh);
        if (fresh)
            groupFocus(host);
        pollAt_ = 0;
        host.invalidate();
    }
    // The list of sections is at the section that was left.
    void groupFocus(ScreenHost &host) {
        size_t row = 0;
        for (size_t index : visible()) {
            if (index == group_)
                break;
            ++row;
        }
        list_.select(host, row);
    }
    void openGroup(ScreenHost &host, size_t group) {
        group_ = group;
        level_ = Level::Rows;
        sub_.clear();
        disarm();
        busy_ = false;
        pollAt_ = 0;
        const std::string &key = groups_[group].key;
        // What was loaded last time shows at once while the TV is asked again.
        if (data_.count(key))
            loadedOnce_.insert(key);
        else
            loadedOnce_.erase(key);
        rebuild(host, true);
        readGroup(host);
    }
    // The rows of the section from the data it last loaded.
    void rebuild(ScreenHost &host, bool fresh) {
        if (level_ != Level::Rows)
            return;
        const std::string &key = groups_[group_].key;
        auto cached = data_.find(key);
        Json data = cached == data_.end() ? Json() : cached->second;
        std::vector<MenuRow> rows = dashboardRows(key, data, host.view().caps, sub_);
        // The dashboard is dark only; its Theme row would change nothing.
        rows.erase(std::remove_if(rows.begin(), rows.end(),
                                  [](const MenuRow &row) { return row.command.route == "theme"; }),
                   rows.end());
        std::vector<SettingItem> items = settingItems(key, rows);
        // Metrics are the page Live shows as cards; the way there is the first row.
        if (key == "system" && sub_.empty()) {
            SettingItem live;
            live.kind = SettingItem::Kind::Link;
            live.icon = "speed";
            live.title = say::msg("tv.settings.liveLink", "Live");
            live.body = say::msg("tv.settings.liveLink.desc",
                                 "The same readings as cards, refreshed every few seconds.");
            live.action = say::msg("tv.settings.open", "Open");
            live.command = navigate("live");
            items.insert(items.begin(), live);
        }
        decorate(items);
        list_.set(host, std::move(items), fresh);
        host.invalidate();
    }
    // What the focused row says while it waits for a second OK or for the TV.
    void decorate(std::vector<SettingItem> &items) const {
        for (SettingItem &item : items) {
            if (!item.focusable() || item.command.empty() ||
                item.command.operation == Operation::None)
                continue;
            if (armed_ && signature(item.command) == armedSignature_) {
                item.value = say::msg("tv.confirm", "Confirm?");
                item.action = say::msg("tv.settings.pressAgain", "Press OK again to confirm");
                item.chips = {say::msg("tv.settings.backCancels", "Back cancels")};
            } else if (busy_ && signature(item.command) == writingSignature_) {
                item.value = say::msg("tv.settings.working", "Working…");
                item.action.clear();
            }
        }
    }

    // ---- doing --------------------------------------------------------------------------------

    void activate(ScreenHost &host) {
        const SettingItem *item = list_.current();
        if (!item || !item->focusable())
            return;
        if (level_ == Level::Groups) {
            std::vector<size_t> shown = visible();
            if (list_.focus() < shown.size())
                openGroup(host, shown[list_.focus()]);
            return;
        }
        if (item->command.empty())
            return;
        Command command = item->command;
        if (!command.route.empty()) {
            follow(host, command.route);
            return;
        }
        if (busy_ || waitAt_)
            return;
        // A change that is hard to undo asks twice, within a few seconds.
        if (command.confirm && !(armed_ && armedSignature_ == signature(command))) {
            armed_ = true;
            armedSignature_ = signature(command);
            armedUntil_ = host.nowMs() + kConfirmMs;
            host.toast(
                say::msg("tv.settings.confirmToast", "Press OK again to confirm. Back cancels."));
            rebuild(host, false);
            return;
        }
        disarm();
        if (command.body["action"].string() == "screensaver") {
            // Started once the OK that chose it has been let go, or it would end the saver at once.
            saverAt_ = host.nowMs() + kSaverDelayMs;
            saverCommand_ = command;
            host.toast(say::msg("tv.flash.startingSaver", "Starting the screen saver…"));
            return;
        }
        write(host, command);
    }
    void disarm() {
        armed_ = false;
        armedSignature_.clear();
    }
    void follow(ScreenHost &host, const std::string &route) {
        if (route == "refresh") {
            readGroup(host);
            return;
        }
        if (route.compare(0, 4, "sub:") == 0) {
            sub_ = route.substr(4);
            rebuild(host, true);
            return;
        }
        host.open(route);
    }
    void write(ScreenHost &host, const Command &command) {
        if (!host.services().write("settings:write", command.operation, command.body)) {
            host.toast(say::msg("tv.settings.busy", "Too many pending requests. Try again."));
            return;
        }
        busy_ = true;
        writing_ = command;
        writingSignature_ = signature(command);
        rebuild(host, false);
    }
    void written(ScreenHost &host, const Response &response) {
        busy_ = false;
        Json data = response.json();
        bool good = response.ok() && data.valid() && (!data.has("ok") || data["ok"].boolean());
        if (!good) {
            host.toast(data["error"].string(say::msg("tv.failed", "That did not work")));
            readGroup(host);
            rebuild(host, false);
            return;
        }
        const SettingItem *item = list_.current();
        bool quiet = item && item->kind == SettingItem::Kind::Toggle; // the switch itself answers
        if (!quiet)
            host.toast(say::msg("tv.settings.saved", "Saved"));
        // Opening or closing the dashboard restarts the server: ask until it answers again.
        if (writing_.operation == Operation::Setup &&
            writing_.body["action"].string() == "network") {
            host.toast(writing_.body["open"].boolean()
                           ? say::msg("tv.net.opening", "Opening it to your network…")
                           : say::msg("tv.net.closing", "Keeping it to this TV…"));
            waitAt_ = host.nowMs() + 1500;
            waitTries_ = kServerTries;
            waiting_ = false;
        } else
            readGroup(host);
        rebuild(host, false);
    }
    void waited(ScreenHost &host, const Response &response) {
        waiting_ = false;
        Json data = response.json();
        if (response.ok() && data.valid()) {
            waitAt_ = 0;
            readGroup(host);
            rebuild(host, false);
            return;
        }
        if (--waitTries_ <= 0) {
            waitAt_ = 0;
            host.toast(say::msg("tv.net.notBack", "It did not come back. Try again."));
            rebuild(host, false);
            return;
        }
        waitAt_ = host.nowMs() + kServerWaitMs;
    }

    // The page draws no focus while the top bar has it.
    void syncFocus(ScreenHost &host) {
        bool bar = host.barFocused();
        if (bar == barWas_)
            return;
        barWas_ = bar;
        list_.show(host, !bar);
    }

    std::vector<SettingGroup> groups_;
    SettingsList list_;
    Level level_ = Level::Groups;
    size_t group_ = 0;
    bool direct_ = false, barWas_ = false, capsChanged_ = false;
    std::string sub_, readingKey_;
    std::map<std::string, Json> data_;
    std::set<std::string> hidden_, loadedOnce_;
    double pollAt_ = 0;
    // A write in flight, and the second OK a risky one waits for.
    bool busy_ = false, armed_ = false;
    Command writing_, saverCommand_;
    std::string writingSignature_, armedSignature_;
    double armedUntil_ = 0, saverAt_ = 0, waitAt_ = 0;
    bool waiting_ = false;
    int waitTries_ = 0;
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
