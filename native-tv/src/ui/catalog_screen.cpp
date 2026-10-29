// Catalog: the Homebrew Channel's apps as a grid, and the install that follows a choice
// (the dashboard's install flow): the server fetches and checks the package, shows what it will
// change, and only OK on Install changes the TV. The steps are a grid, a progress card, a
// list-and-panel preview and a result.
#include "domain/say.hpp"
#include "ui/application.hpp"
#include "ui/dashboard_screens.hpp"
#include "ui/home_layout.hpp"
#include "ui/kit/tuning.hpp"
#include "ui/settings_list.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace nativetv {
namespace {
namespace ms = kit::motion::ms;
namespace layout = kit::layout;
constexpr size_t kColumns = static_cast<size_t>(layout::tileCols);
constexpr int kVisibleRows = 3;         // the strip under the grid takes the fourth
constexpr double kBusyPollMs = 1000;    // a running install
constexpr double kPreviewPollMs = 5000; // a preview expires on the server
constexpr double kConfirmMs = 4000;
constexpr float kStripY = 832, kStripH = 168;

Json vars(std::initializer_list<std::pair<const char *, std::string>> values) {
    Json result = Json::object();
    for (const auto &value : values)
        result.set(value.first, Json::text(value.second));
    return result;
}
bool installing(const std::string &state) {
    return state == "downloading" || state == "verifying" || state == "installing" ||
           state == "elevating";
}
// Server errors start in lower case.
std::string sentence(std::string text) {
    if (!text.empty() && std::islower(static_cast<unsigned char>(text[0])) &&
        text.find('.') == std::string::npos)
        text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
    return text;
}
std::string size(double bytes) {
    if (bytes <= 0)
        return "";
    if (bytes < 1048576)
        return say::msg("apps.install.kb", "{n} KB",
                        vars({{"n", std::to_string(std::lround(bytes / 1024))}}));
    double megabytes = bytes / 1048576;
    char text[32];
    std::snprintf(text, sizeof(text), megabytes >= 10 ? "%.0f" : "%.1f", megabytes);
    return say::msg("apps.install.mb", "{n} MB", vars({{"n", text}}));
}
std::string megabytes(double bytes) {
    return bytes > 0 && bytes < 524288
               ? say::msg("apps.install.mbUnder1", "under 1 MB")
               : say::msg("apps.install.mb", "{n} MB",
                          vars({{"n", std::to_string(std::lround(bytes / 1048576))}}));
}

struct App {
    std::string id, title, version, description, state, installedVersion;
    double size = 0;
    bool rootRequired = false;
};

class CatalogScreen final : public ScreenView {
  public:
    CatalogScreen() : list_("catl") {}
    std::string name() const override {
        return "catalog";
    }
    int barTab() const override {
        return -1;
    }

    void enter(ScreenHost &host) override {
        Motion &motion = host.motion();
        motion.set("cat:gy", 0);
        motion.play("cat:E:head", Tween(1, 0, ms::base, kit::easing(kit::motion::ease::out),
                                        kit::motion::stagger::pageBase));
        barWas_ = host.barFocused();
        readCatalog(host);
        readStatus(host);
        focusTile(host, 0, true);
    }
    void leave(ScreenHost &host) override {
        for (const char *channel : {"catalog:list", "catalog:status", "catalog:write"})
            host.services().cancel(channel);
        list_.clear(host);
        host.motion().erasePrefix("cat");
    }
    void focus(ScreenHost &host) override {
        syncFocus(host);
    }

    bool key(ScreenHost &host, Key key) override {
        View now = view();
        if (now == View::Busy)
            return key != Key::Up;
        if (now == View::Preview || now == View::Result)
            return listKey(host, key);
        return gridKey(host, key);
    }
    bool back(ScreenHost &host) override {
        View now = view();
        if (armed_ >= 0) {
            armed_ = -1;
            host.toast(say::msg("tv.settings.cancelled", "Cancelled"));
            return true;
        }
        if (now == View::Preview) {
            cancelInstall(host);
            return true;
        }
        if (now == View::Busy) {
            host.toast(say::msg("tv.apps.install.wait", "An install is running. Please wait."));
            return true;
        }
        if (now == View::Result) {
            dismiss(host);
            return true;
        }
        host.open("dashboard:apps");
        return true;
    }
    void click(ScreenHost &host, const std::string &hit) override {
        size_t index = 0;
        if (hit.compare(0, 9, "cat:tile:") == 0 && view() == View::List) {
            index = static_cast<size_t>(std::atoi(hit.c_str() + 9));
            if (index >= apps_.size())
                return;
            if (index == focus_)
                chooseTile(host);
            else
                focusTile(host, index, false);
        } else if (list_.parseHit(hit, index)) {
            if (index == list_.focus())
                chooseRow(host);
            else
                list_.select(host, index);
        }
    }

    void response(ScreenHost &host, const Response &response) override {
        const std::string &channel = response.channel;
        Json data = response.json();
        bool good = response.ok() && data.valid() && (!data.has("ok") || data["ok"].boolean());
        if (channel == "catalog:list") {
            catalogFailed_ = !good;
            catalog_ = good ? data : Json();
            if (good)
                sortApps(data);
            else
                error_ = data["error"].string();
            refresh(host);
        } else if (channel == "catalog:status") {
            statusReading_ = false;
            if (good)
                adopt(host, data);
            else if (owned_)
                statusAt_ = host.nowMs() + 2000; // the server may be restarting; keep asking
        } else if (channel == "catalog:write")
            written(host, response, data, good);
    }

    bool update(ScreenHost &host) override {
        const double now = host.nowMs();
        syncFocus(host);
        list_.update(host);
        if (armed_ >= 0 && now >= armedUntil_)
            armed_ = -1;
        if (statusAt_ && now >= statusAt_ && !statusReading_) {
            statusAt_ = 0;
            readStatus(host);
        }
        if (view() == View::Busy) {
            const double fraction = progress();
            host.motion().tween("cat:p", fraction, 400, kit::easing(kit::motion::ease::out));
        }
        return false;
    }

    void build(ScreenHost &host, kit::Paint paint) override {
        Motion &motion = host.motion();
        double head = motion.value("cat:E:head");
        View now = view();
        kit::PageHead page;
        page.title = say::msg("tv.catalog.title", "Homebrew apps");
        if (now == View::Preview) {
            page.title =
                say::msg("tv.catalog.install", "Install {name}", vars({{"name", previewName()}}));
            page.subtitle = say::msg("tv.apps.install.go.hint",
                                     "Nothing is installed until OK is pressed here.");
        } else if (now == View::Busy)
            page.subtitle = say::msg("tv.catalog.busy", "Installing…");
        else if (now == View::Result)
            page.subtitle = "";
        else
            page.subtitle = say::msg("tv.catalog.subtitle", "From the Homebrew Channel catalog.");
        page.enter = {static_cast<float>(1 - head), static_cast<float>(28 * head)};
        kit::pageHead(paint, page);
        if (now == View::Preview || now == View::Result)
            list_.build(host, paint, "");
        else if (now == View::Busy)
            buildBusy(host, paint);
        else
            buildGrid(host, paint);
    }

    Json snapshot() const override {
        Json value = Json::object();
        static const char *const names[] = {"list", "busy", "preview", "result"};
        value.set("view", Json::text(names[static_cast<int>(view())]));
        value.set("apps", Json::numeric(static_cast<double>(apps_.size())));
        value.set("focus",
                  Json::text(view() == View::List ? (focus_ < apps_.size() ? apps_[focus_].id : "")
                             : list_.current()    ? list_.current()->title
                                                  : ""));
        value.set("root", Json::flag(root_));
        value.set("replace", Json::flag(replace_));
        value.set("busy", Json::flag(busy_));
        return value;
    }

  private:
    enum class View { List, Busy, Preview, Result };
    enum class Write { Fetch, Confirm, Cancel, Uninstall };

    View view() const {
        if (!inst_.valid())
            return View::List;
        std::string state = inst_["state"].string();
        if (installing(state))
            return View::Busy;
        if (state == "awaiting-confirm" && inst_["preview"].valid())
            return View::Preview;
        if ((state == "installed" || state == "error" || state == "interrupted") && owned_ &&
            inst_["jobId"].dump() != dismissed_)
            return View::Result;
        return View::List;
    }

    // ---- data -------------------------------------------------------------------------------

    void readCatalog(ScreenHost &host) {
        host.services().read("catalog:list", Operation::AppCatalog);
    }
    void readStatus(ScreenHost &host) {
        statusReading_ = host.services().read("catalog:status", Operation::InstallStatus) != 0;
        if (!statusReading_)
            statusAt_ = host.nowMs() + 1000;
    }
    // Updates first, then by title.
    void sortApps(const Json &data) {
        apps_.clear();
        Json list = data["apps"];
        for (const Json &entry : list.items()) {
            App app;
            app.id = entry["id"].string();
            app.title = entry["title"].string(app.id);
            app.version = entry["version"].string();
            app.description = entry["description"].string(app.id);
            app.state = entry["state"].string();
            app.installedVersion = entry["installedVersion"].string();
            app.size = entry["size"].number(0);
            app.rootRequired = entry["rootRequired"].boolean();
            apps_.push_back(app);
        }
        auto lowered = [](std::string text) {
            std::transform(text.begin(), text.end(), text.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        };
        std::stable_sort(apps_.begin(), apps_.end(), [&](const App &a, const App &b) {
            bool left = a.state == "update", right = b.state == "update";
            return left != right ? left : lowered(a.title) < lowered(b.title);
        });
        focus_ = std::min(focus_, apps_.empty() ? 0 : apps_.size() - 1);
    }
    // A status from the server: the poll, or what a write answered with.
    void adopt(ScreenHost &host, const Json &status) {
        std::string was = inst_["state"].string(), now = status["state"].string();
        inst_ = status;
        if (installing(now) || now == "awaiting-confirm")
            owned_ = true;
        if (now == "awaiting-confirm" && status["jobId"].dump() != job_) {
            job_ = status["jobId"].dump();
            root_ = replace_ = false;
        }
        // A job that ended changes what the catalog says is installed.
        if ((installing(was) || was == "awaiting-confirm") && !installing(now) &&
            now != "awaiting-confirm")
            readCatalog(host);
        statusAt_ = installing(now)             ? host.nowMs() + kBusyPollMs
                    : now == "awaiting-confirm" ? host.nowMs() + kPreviewPollMs
                                                : 0;
        statusReading_ = false;
        refresh(host);
    }
    double progress() const {
        Json bytes = inst_["progress"];
        double total = bytes["total"].number(0);
        std::string state = inst_["state"].string();
        if (state == "downloading" && total > 0)
            return std::min(1.0, bytes["bytes"].number(0) / total);
        return state == "downloading" ? 0.05 : state == "verifying" ? 1 : 1;
    }

    // ---- the steps --------------------------------------------------------------------------

    // The screen after a change of data or step: the preview and result are lists, rebuilt.
    void refresh(ScreenHost &host) {
        View now = view();
        bool changed = now != shown_;
        shown_ = now;
        if (now == View::Preview)
            previewList(host, changed);
        else if (now == View::Result)
            resultList(host, changed);
        else if (now == View::List && changed)
            replayGrid(host);
        host.invalidate();
    }
    std::string previewName() const {
        Json first = inst_["preview"]["apps"][static_cast<size_t>(0)];
        return first["title"].string(inst_["preview"]["package"].string());
    }
    SettingItem item(SettingItem::Kind kind, const std::string &icon, const std::string &title,
                     const std::string &value, const std::string &body,
                     const std::string &route = "") {
        SettingItem result;
        result.kind = kind;
        result.icon = icon;
        result.title = title;
        result.value = value;
        result.body = body;
        if (!value.empty())
            result.chips.push_back(value);
        if (!route.empty())
            result.command = navigate(route);
        return result;
    }
    void previewList(ScreenHost &host, bool fresh) {
        Json pv = inst_["preview"];
        std::vector<SettingItem> items;
        std::string direction = pv["direction"].string(), version = pv["version"].string(),
                    from = pv["installedVersion"].string();
        std::string change =
            pv["storeInstalled"].boolean() && !from.empty()
                ? say::msg("apps.install.dir.store",
                           "v{version}, in place of the LG store's v{from}",
                           vars({{"from", from}, {"version", version}}))
            : direction == "up"
                ? say::msg("apps.install.dir.up", "upgrade from v{from} to v{version}",
                           vars({{"from", from}, {"version", version}}))
            : direction == "down"
                ? say::msg("apps.install.dir.down", "downgrade from v{from} to v{version}",
                           vars({{"from", from}, {"version", version}}))
            : direction == "same" ? say::msg("apps.install.dir.same", "reinstall of v{version}",
                                             vars({{"version", version}}))
                                  : say::msg("apps.install.dir.new", "new install of v{version}",
                                             vars({{"version", version}}));
        items.push_back(item(SettingItem::Kind::Info, "info",
                             say::msg("tv.apps.install.pv.change", "Change"), change,
                             pv["package"].string()));
        std::vector<std::string> ids, services;
        for (const Json &entry : pv["apps"].items())
            ids.push_back(entry["id"].string());
        for (const Json &entry : pv["services"].items())
            services.push_back(entry.string());
        auto joined = [](const std::vector<std::string> &values) {
            std::string text;
            for (const std::string &value : values)
                text += (text.empty() ? "" : ", ") + value;
            return text;
        };
        if (ids.size() > 1)
            items.push_back(
                item(SettingItem::Kind::Info, "apps", say::msg("tv.apps.install.pv.apps", "Apps"),
                     std::to_string(ids.size()),
                     say::msg("apps.install.apps", "Apps: {list}", vars({{"list", joined(ids)}}))));
        if (!services.empty()) {
            items.push_back(item(SettingItem::Kind::Info, "power",
                                 say::msg("tv.apps.install.pv.services", "Services"),
                                 std::to_string(services.size()),
                                 say::msg("apps.install.services", "Background services: {list}",
                                          vars({{"list", joined(services)}}))));
            // Root for a package the Homebrew Channel has not vetted is offered only where it can
            // be confirmed by name, which the remote cannot do.
            if (pv["vetted"].boolean()) {
                SettingItem root = item(SettingItem::Kind::Toggle, "lock",
                                        say::msg("tv.catalog.root", "Run services as root"), "",
                                        pv["rootRequired"].boolean()
                                            ? say::msg("tv.apps.install.root.needed",
                                                       "This app asks for its services to run as "
                                                       "root, and may not work without it.")
                                            : say::msg("tv.apps.install.root.hint",
                                                       "Off unless the app needs it. Root access "
                                                       "is set up through the Homebrew Channel."),
                                        "cat:root");
                root.on = root_;
                root.action = root_ ? say::msg("tv.settings.turnOff", "Turn off")
                                    : say::msg("tv.settings.turnOn", "Turn on");
                items.push_back(root);
            }
            if (pv["reelevate"].size())
                items.push_back(item(
                    SettingItem::Kind::Info, "lock", say::msg("tv.apps.install.pv.root", "Root"),
                    say::msg("tv.apps.install.pv.rootBack", "Given back"),
                    say::msg("apps.install.root.restored",
                             "Root is given back to {list} after this update, as before.",
                             vars({{"list", joined([&] {
                                        std::vector<std::string> names;
                                        for (const Json &name : pv["reelevate"].items())
                                            names.push_back(name.string());
                                        return names;
                                    }())}}))));
        }
        if (pv["cpuMismatch"].boolean())
            items.push_back(item(
                SettingItem::Kind::Info, "cpu", say::msg("tv.apps.install.pv.cpu", "Processor"),
                say::msg("tv.apps.install.pv.mismatch", "Mismatch"),
                say::msg("apps.install.cpu", "Its programs are built for a different processor "
                                             "than this TV's and may not run.")));
        if (pv["isScreensaver"].boolean())
            items.push_back(item(SettingItem::Kind::Info, "eye",
                                 say::msg("tv.apps.install.pv.screensaver", "Screen saver"),
                                 say::msg("common.replaces", "Replaces ours"),
                                 say::msg("apps.install.screensaver.warn",
                                          "This app installs a custom screen saver, which will "
                                          "replace nativeTv’s own screen savers.")));
        if (pv["freeBytes"].isNumber())
            items.push_back(item(
                SettingItem::Kind::Info, "disk", say::msg("tv.apps.install.pv.space", "Free space"),
                megabytes(pv["freeBytes"].number()),
                say::msg("apps.install.space", "Free space: {free}. Needed: about {need}.",
                         vars({{"free", megabytes(pv["freeBytes"].number())},
                               {"need", megabytes(pv["needBytes"].number())}}))));
        if (pv["storeInstalled"].boolean()) {
            SettingItem store = item(
                SettingItem::Kind::Toggle, "refresh",
                say::msg("apps.install.store.replace", "Replace the store version"), "",
                say::msg("apps.install.store.warn", "This app was installed from the LG store. "
                                                    "Replacing it stops store updates for it."),
                "cat:replace");
            store.on = replace_;
            store.action = replace_ ? say::msg("tv.settings.turnOff", "Turn off")
                                    : say::msg("tv.settings.turnOn", "Turn on");
            items.push_back(store);
        }
        SettingItem go = item(
            SettingItem::Kind::Action, "check", say::msg("apps.install.confirm", "Install"), "",
            say::msg("tv.apps.install.go.hint", "Nothing is installed until OK is pressed here."),
            "cat:install");
        go.action = say::msg("tv.catalog.installNow", "Install now");
        items.push_back(go);
        SettingItem cancel =
            item(SettingItem::Kind::Action, "close", say::msg("common.cancel", "Cancel"), "",
                 say::msg("tv.apps.install.cancel.hint", "The downloaded package is deleted."),
                 "cat:cancel");
        cancel.action = say::msg("tv.catalog.discard", "Discard the download");
        items.push_back(cancel);
        list_.set(host, std::move(items), fresh);
        if (fresh)
            for (size_t i = 0; i < list_.items().size(); ++i)
                if (list_.items()[i].command.route == "cat:install")
                    list_.select(host, i);
    }
    void resultList(ScreenHost &host, bool fresh) {
        Json result = inst_["result"];
        bool done = inst_["state"].string() == "installed";
        SettingItem row =
            done ? item(SettingItem::Kind::Action, "check",
                        say::msg("apps.install.done", "Installed {name} v{version}.",
                                 vars({{"name", result["title"].string(result["package"].string())},
                                       {"version", result["version"].string()}})),
                        say::msg("tv.apps.install.dismiss.done", "Done"),
                        say::msg("tv.apps.install.done.hint", "The app is on the TV now."),
                        "cat:dismiss")
                 : item(SettingItem::Kind::Action, "info",
                        say::msg("tv.apps.install.error", "The install failed"),
                        say::msg("tv.apps.install.dismiss.error", "Dismiss"),
                        sentence(inst_["error"].string(
                            say::msg("common.unknownError", "unknown error"))),
                        "cat:dismiss");
        row.action = say::msg("tv.catalog.returnToCatalog", "Return to the catalog");
        list_.set(host, {row}, fresh);
    }
    void replayGrid(ScreenHost &host) {
        Timeline timeline;
        for (size_t i = 0; i < apps_.size() && i < 30; ++i)
            timeline.add(gridId("E", i),
                         Tween(1, 0, ms::base, kit::easing(kit::motion::ease::out),
                               kit::motion::stagger::pageBase +
                                   kit::motion::stagger::pageItem *
                                       std::min<int>(static_cast<int>(i),
                                                     kit::motion::stagger::pageItemMax)));
        timeline.play(host.motion());
        focusTile(host, focus_, true);
    }

    // ---- doing --------------------------------------------------------------------------------

    bool listKey(ScreenHost &host, Key key) {
        if (key == Key::Up)
            return list_.move(host, -1);
        if (key == Key::Down || key == Key::PageDown || key == Key::PageUp) {
            list_.move(host, key == Key::PageUp ? -1 : 1, key == Key::Down ? 1 : 6);
            return true;
        }
        if (key == Key::Ok)
            chooseRow(host);
        return true;
    }
    void chooseRow(ScreenHost &host) {
        const SettingItem *row = list_.current();
        if (!row || row->command.route.empty())
            return;
        const std::string &route = row->command.route;
        if (route == "cat:root") {
            root_ = !root_;
            refresh(host);
        } else if (route == "cat:replace") {
            replace_ = !replace_;
            refresh(host);
        } else if (route == "cat:install")
            confirmInstall(host);
        else if (route == "cat:cancel")
            cancelInstall(host);
        else if (route == "cat:dismiss")
            dismiss(host);
    }
    bool gridKey(ScreenHost &host, Key key) {
        size_t count = apps_.size();
        if (key == Key::Ok) {
            chooseTile(host);
            return true;
        }
        if (!count)
            return key != Key::Up;
        size_t next = focus_;
        if (key == Key::Left && focus_ % kColumns)
            next = focus_ - 1;
        else if (key == Key::Right && focus_ % kColumns + 1 < kColumns && focus_ + 1 < count)
            next = focus_ + 1;
        else if (key == Key::Up && focus_ >= kColumns)
            next = focus_ - kColumns;
        else if (key == Key::Down && focus_ + kColumns < count)
            next = focus_ + kColumns;
        else if (key == Key::Down && focus_ / kColumns < (count - 1) / kColumns)
            next = count - 1; // the last, shorter row
        else if (key == Key::PageDown)
            next = std::min(count - 1, focus_ + kColumns * kVisibleRows);
        else if (key == Key::PageUp)
            next = focus_ >= kColumns * kVisibleRows ? focus_ - kColumns * kVisibleRows : 0;
        if (next != focus_) {
            armed_ = -1;
            focusTile(host, next, false);
        }
        return key != Key::Up || focus_ >= kColumns;
    }
    void chooseTile(ScreenHost &host) {
        if (catalogFailed_) {
            catalog_ = Json();
            catalogFailed_ = false;
            readCatalog(host);
            return;
        }
        if (focus_ >= apps_.size() || busy_)
            return;
        const App &app = apps_[focus_];
        if (app.state == "hbc") {
            host.toast(say::msg("apps.install.viaHbc", "Updates through Homebrew Channel"));
        } else if (app.state == "installed") {
            // Removing an app asks twice.
            if (armed_ != static_cast<int>(focus_)) {
                armed_ = static_cast<int>(focus_);
                armedUntil_ = host.nowMs() + kConfirmMs;
                host.toast(say::msg("tv.apps.uninstall.again", "Press OK again to uninstall {name}",
                                    vars({{"name", app.title}})));
                return;
            }
            armed_ = -1;
            writeKind_ = Write::Uninstall;
            send(host, post(Operation::UninstallApp, fields({{"id", Json::text(app.id)}})));
        } else {
            owned_ = true;
            dismissed_.clear();
            writeKind_ = Write::Fetch;
            send(host, post(Operation::FetchPackage,
                            fields({{"id", Json::text(app.id)}, {"auto", Json::flag(true)}})));
        }
    }
    void confirmInstall(ScreenHost &host) {
        Json pv = inst_["preview"];
        if (busy_ || inst_["state"].string() != "awaiting-confirm" || !pv.valid())
            return;
        if (pv["storeInstalled"].boolean() && !replace_) {
            host.toast(say::msg("tv.apps.install.store.needOn",
                                "Switch on Replace the store version first."));
            return;
        }
        writeKind_ = Write::Confirm;
        send(host, post(Operation::ConfirmPackage,
                        fields({{"jobId", inst_["jobId"]},
                                {"elevate", Json::flag(root_ && pv["vetted"].boolean())},
                                {"replaceStore", Json::flag(replace_)}})));
    }
    void cancelInstall(ScreenHost &host) {
        if (busy_ || !inst_.valid())
            return;
        writeKind_ = Write::Cancel;
        send(host, post(Operation::CancelPackage, fields({{"jobId", inst_["jobId"]}})));
    }
    void dismiss(ScreenHost &host) {
        dismissed_ = inst_["jobId"].dump();
        owned_ = false;
        refresh(host);
    }
    void send(ScreenHost &host, const Command &command) {
        if (!host.services().write("catalog:write", command.operation, command.body)) {
            host.toast(say::msg("tv.settings.busy", "Too many pending requests. Try again."));
            return;
        }
        busy_ = true;
    }
    void written(ScreenHost &host, const Response &, const Json &data, bool good) {
        busy_ = false;
        Write kind = writeKind_;
        if (!good) {
            if (kind == Write::Fetch)
                owned_ = false;
            host.toast(sentence(data["error"].string(
                kind == Write::Fetch
                    ? say::msg("apps.install.failed", "The install could not be started")
                    : say::msg("tv.failed", "That did not work"))));
            readStatus(host);
            return;
        }
        if (kind == Write::Uninstall) {
            host.toast(
                say::msg("apps.uninstall.done", "Uninstalled {name}.",
                         vars({{"name", focus_ < apps_.size() ? apps_[focus_].title : ""}})));
            readCatalog(host);
            return;
        }
        if (kind == Write::Cancel) {
            owned_ = false;
            inst_ = Json();
            refresh(host);
            readStatus(host);
            return;
        }
        if (data["install"].valid())
            adopt(host, data["install"]);
        else
            readStatus(host);
    }

    // ---- drawing ------------------------------------------------------------------------------

    static std::string gridId(const char *name, size_t index) {
        return std::string("cat:") + name + ":" + std::to_string(index);
    }
    void focusTile(ScreenHost &host, size_t index, bool jump) {
        Motion &motion = host.motion();
        if (!jump && index == focus_)
            return;
        if (!jump)
            motion.tween(gridId("f", focus_), 0, ms::focus, kit::easing(kit::motion::ease::out));
        focus_ = index;
        bool visible = !host.barFocused();
        if (jump)
            motion.set(gridId("f", focus_), visible ? 1 : 0);
        else
            motion.tween(gridId("f", focus_), visible ? 1 : 0, ms::focus,
                         kit::easing(kit::motion::ease::out));
        // The grid follows once focus passes its second row.
        size_t row = focus_ / kColumns;
        double target =
            -static_cast<double>(row > 1 ? row - 1 : 0) * (layout::tileH + layout::tileGapY);
        if (jump)
            motion.set("cat:gy", target);
        else
            motion.spring("cat:gy", target, kit::springParams(kit::motion::spring::list));
        host.invalidate();
    }
    void syncFocus(ScreenHost &host) {
        bool bar = host.barFocused();
        if (bar == barWas_)
            return;
        barWas_ = bar;
        list_.show(host, !bar);
        host.motion().tween(gridId("f", focus_), bar ? 0 : 1, ms::focus,
                            kit::easing(kit::motion::ease::out));
        host.invalidate();
    }

    void buildGrid(ScreenHost &host, kit::Paint paint) {
        Motion &motion = host.motion();
        Scene &scene = paint.scene;
        if (apps_.empty()) {
            std::string title =
                catalogFailed_     ? say::msg("tv.apps.install.unavailable", "Catalog unavailable")
                : catalog_.valid() ? say::msg("tv.catalog.none", "No applications available")
                                   : say::msg("apps.install.loading", "Loading the catalog…");
            std::string hint =
                catalogFailed_ ? say::msg("apps.install.unavailable",
                                          "The catalog could not be loaded. The TV needs internet "
                                          "access to reach it.") +
                                     (error_.empty() ? "" : " " + sentence(error_))
                : catalog_.valid()
                    ? ""
                    : say::msg("tv.apps.install.loading.hint",
                               "The TV is fetching the list. This needs internet access.");
            paint.glyphs.text(
                scene, {kit::canvas::safeX, layout::gridTop + 8, 1400, kit::type::headline.line},
                title, kit::type::headline, kit::color::onSurface);
            paint.glyphs.text(
                scene, {kit::canvas::safeX, layout::gridTop + 72, 1400, kit::type::body.line * 2},
                hint, kit::type::body, kit::color::onSurface2, Node::Align::Left, 2);
            if (catalogFailed_)
                paint.glyphs.text(
                    scene,
                    {kit::canvas::safeX, layout::gridTop + 160, 1400, kit::type::labelM.line},
                    say::msg("tv.apps.install.press.retry", "Press OK to try again"),
                    kit::type::labelM, kit::color::onSurface3);
            return;
        }
        const float scroll = static_cast<float>(motion.value("cat:gy"));
        scene.pushClip({0, layout::gridTop - 28, kit::canvas::width,
                        kVisibleRows * (layout::tileH + layout::tileGapY) + 28});
        for (size_t i = 0; i < apps_.size(); ++i) {
            const App &app = apps_[i];
            Rect rest = kit::geometry::appTile(static_cast<int>(i));
            rest.y += scroll;
            if (rest.y + rest.h < layout::gridTop - 60 || rest.y > kStripY)
                continue;
            kit::AppTile tile;
            tile.rest = rest;
            tile.id = "cat:tile:" + std::to_string(i);
            tile.name = app.title;
            tile.icon = "apps";
            tile.color = home::hsl(static_cast<float>(home::hashOf(app.id) % 360), .5f, .36f);
            tile.focus = static_cast<float>(motion.value(gridId("f", i)));
            double hidden = motion.value(gridId("E", i));
            tile.enter = {static_cast<float>(1 - hidden), static_cast<float>(28 * hidden)};
            kit::appTile(paint, tile);
            std::string badge = app.state == "update" ? say::msg("apps.install.update", "Update")
                                : app.state == "installed"
                                    ? say::msg("tv.catalog.installed", "Installed")
                                : app.state == "hbc" ? "Homebrew Channel"
                                                     : "";
            if (!badge.empty())
                badgeOn(paint, tile, badge, app.state == "update");
        }
        scene.popClip();
        if (focus_ < apps_.size())
            strip(host, paint, apps_[focus_]);
    }
    // A small label at a tile's top right, moving with the tile.
    void badgeOn(kit::Paint paint, const kit::AppTile &tile, const std::string &label,
                 bool update) {
        Scene &scene = paint.scene;
        kit::focus::Look look = kit::focus::lookAt(tile.rest, kit::motion::scale::tile, tile.focus);
        scene.pushOpacity(tile.enter.alpha);
        scene.pushTransform(Transform::translate(0, tile.enter.dy));
        scene.pushTransform(kit::focus::transformAt(tile.rest, look.scale));
        float width = paint.glyphs.measure(label, kit::type::labelS) + 20;
        Rect box = {tile.rest.x + tile.rest.w - 14 - width, tile.rest.y + 14, width, 28};
        scene.shape(box, update ? kit::color::warn : Color{0, 0, 0, .36f}, 14);
        paint.glyphs.text(
            scene,
            {box.x, box.y + (28 - kit::type::labelS.line) / 2, width, kit::type::labelS.line},
            label, kit::type::labelS, update ? kit::color::onFocus : kit::color::onSurface,
            Node::Align::Center);
        scene.popTransform();
        scene.popTransform();
        scene.popOpacity();
    }
    // What the focused app is, under the grid.
    void strip(ScreenHost &, kit::Paint paint, const App &app) {
        Scene &scene = paint.scene;
        Rect box = {kit::canvas::safeX, kStripY, kit::canvas::width - 2 * kit::canvas::safeX,
                    kStripH};
        Node &base = scene.shape(box, kit::color::surface1, kit::radius::lg);
        base.ring = {1, 0, kit::alpha::outline};
        float left = box.x + 28, inner = box.w - 56;
        paint.glyphs.text(scene, {left, box.y + 18, inner - 360, kit::type::title.line}, app.title,
                          kit::type::title, kit::color::onSurface);
        std::string meta = app.version.empty() ? "" : "v" + app.version;
        std::string weight = size(app.size);
        if (!weight.empty())
            meta += (meta.empty() ? "" : " · ") + weight;
        if (app.state == "update" && !app.installedVersion.empty())
            meta += " · " + say::msg("apps.install.have", "installed: v{version}",
                                     vars({{"version", app.installedVersion}}));
        if (app.rootRequired)
            meta += " · " + say::msg("apps.install.rootBadge", "root");
        paint.glyphs.text(scene, {left, box.y + 58, inner - 360, kit::type::labelM.line}, meta,
                          kit::type::labelM, kit::color::onSurface3);
        paint.glyphs.text(scene, {left, box.y + 90, inner - 360, kit::type::body.line * 2},
                          app.description, kit::type::body, kit::color::onSurface2,
                          Node::Align::Left, 2);
        std::string action =
            app.state == "update"      ? say::msg("tv.catalog.reviewUpdate", "Review and update")
            : app.state == "installed" ? say::msg("apps.uninstall.button", "Uninstall")
            : app.state == "hbc"       ? ""
                                       : say::msg("tv.catalog.reviewInstall", "Review and install");
        if (action.empty())
            return;
        float capWidth = std::max(30.0f, paint.glyphs.measure("OK", kit::type::cap) + 24);
        float textWidth = paint.glyphs.measure(action, kit::type::labelM);
        float x = box.x + box.w - 28 - textWidth - 12 - capWidth, y = box.y + box.h - 28 - 30;
        scene.shape({x, y, capWidth, 30}, kit::color::surface3, kit::radius::sm);
        paint.glyphs.text(scene,
                          {x, y + (30 - kit::type::cap.line) / 2, capWidth, kit::type::cap.line},
                          "OK", kit::type::cap, kit::color::onSurface2, Node::Align::Center);
        paint.glyphs.text(scene,
                          {x + capWidth + 12, y + (30 - kit::type::labelM.line) / 2, textWidth + 8,
                           kit::type::labelM.line},
                          action, kit::type::labelM, kit::color::onSurface3);
    }
    void buildBusy(ScreenHost &host, kit::Paint paint) {
        Scene &scene = paint.scene;
        std::string state = inst_["state"].string();
        Json bytes = inst_["progress"];
        std::string line =
            state == "downloading"
                ? (bytes["total"].number(0) > 0
                       ? say::msg("apps.install.downloading.of", "Downloading {done} of {total}",
                                  vars({{"done", megabytes(bytes["bytes"].number(0))},
                                        {"total", megabytes(bytes["total"].number())}}))
                       : say::msg("apps.install.downloading", "Downloading… {done}",
                                  vars({{"done", megabytes(bytes["bytes"].number(0))}})))
            : state == "verifying"  ? say::msg("apps.install.verifying", "Checking the package…")
            : state == "installing" ? say::msg("apps.install.installing", "Installing…")
                                    : say::msg("apps.install.elevating", "Setting up root access…");
        Rect box = {kit::canvas::safeX, layout::gridTop, 1000, 300};
        Node &card = scene.shape(box, kit::color::surface1, kit::radius::xl);
        card.ring = {1, 0, kit::alpha::outline};
        paint.glyphs.text(scene, {box.x + 48, box.y + 48, box.w - 96, kit::type::headline.line},
                          line, kit::type::headline, kit::color::onSurface);
        Rect track = {box.x + 48, box.y + 140, box.w - 96, layout::barH};
        scene.shape(track, kit::color::surface3, layout::barH / 2);
        float fraction =
            static_cast<float>(std::max(0.0, std::min(1.0, host.motion().value("cat:p"))));
        if (fraction > 0)
            scene.shape({track.x, track.y, track.w * fraction, track.h}, kit::color::accent,
                        layout::barH / 2);
        paint.glyphs.text(
            scene, {box.x + 48, box.y + 180, box.w - 96, kit::type::body.line * 2},
            say::msg("tv.apps.install.busy.hint",
                     "This can take a minute or two. Back works again when it finishes."),
            kit::type::body, kit::color::onSurface2, Node::Align::Left, 2);
    }

    SettingsList list_;
    Json catalog_, inst_;
    std::vector<App> apps_;
    std::string job_, dismissed_, error_;
    size_t focus_ = 0;
    View shown_ = View::List;
    Write writeKind_ = Write::Fetch;
    bool catalogFailed_ = false, owned_ = false, busy_ = false, statusReading_ = false;
    bool root_ = false, replace_ = false, barWas_ = false;
    int armed_ = -1;
    double armedUntil_ = 0, statusAt_ = 0;
};
} // namespace

std::unique_ptr<ScreenView> makeCatalogScreen(const std::string &route) {
    return route == "catalog" ? std::unique_ptr<ScreenView>(new CatalogScreen()) : nullptr;
}
} // namespace nativetv
