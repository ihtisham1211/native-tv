// The Quick settings drawer: a panel that springs in from the right over the page it was opened
// from, with the TV's volume, picture mode, light, sound output, input and sleep timer, the
// screensaver and Screen off, the recent apps, the profile and Home's own switches.
//
// What the TV is on comes from /api/stats when the drawer opens and shortly after each change;
// changes go to the routes the JavaScript drawer used (/api/control, /api/home/layout,
// /api/home/open). Volume goes in steps like the remote's key and steps pressed while one is on
// its way go in one more, so a held key sends a request at a time, never a pile.
#include "domain/models.hpp"
#include "domain/say.hpp"
#include "ui/application.hpp"
#include "ui/kit/overlay.hpp"
#include "ui/kit/tuning.hpp"
#include "ui/overlay_routes.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>

namespace nativetv {
namespace {
namespace layout = kit::layout;
using kit::easing;
using say::msg;

constexpr float kPanelW = 760, kPanelX = kit::canvas::width - kPanelW, kPad = 48,
                kRowW = kPanelW - 2 * kPad, kViewTop = 140, kViewBottom = kit::canvas::height - 96,
                kRowPitch = 92;
constexpr double kStepMs = 600;       // stats are read again this long after a change
constexpr double kLightDebounceMs = 350;
constexpr int kLightStep = 5;
// The panel's own spring: quicker than a page's, still without overshoot.
constexpr kit::SpringTuning kSlide = {420, 1};
constexpr const char *kStats = "quick:stats", *kNow = "quick:now", *kVolume = "quick:vol",
                     *kMute = "quick:mute", *kLight = "quick:light", *kSet = "quick:set",
                     *kAct = "quick:act", *kLayout = "quick:layout";

enum class Level { Main, Picture, Sound, Input, Sleep, Layout };
const char *levelName(Level level) {
    switch (level) {
    case Level::Picture:
        return "picture";
    case Level::Sound:
        return "sound";
    case Level::Input:
        return "input";
    case Level::Sleep:
        return "sleep";
    case Level::Layout:
        return "layout";
    default:
        return "quick";
    }
}
// The row of the main list that opens a level.
const char *rowOpening(Level level) {
    switch (level) {
    case Level::Picture:
        return "picture";
    case Level::Sound:
        return "sound";
    case Level::Input:
        return "input";
    case Level::Sleep:
        return "sleep";
    default:
        return "layout";
    }
}
struct LayoutSwitch {
    const char *key, *icon;
};
const LayoutSwitch kSwitches[] = {{"clockFormat", "clock"}, {"appNames", "apps"},
                                  {"showLive", "tv"},       {"showInputs", "hdmi"},
                                  {"showLgHome", "home"},   {"picture", "display"}};
const char *kSleep[] = {"off", "10", "30", "60", "90", "120"};

std::string failure(const std::string &what, const Json &data) {
    std::string reason = data["error"].string();
    return reason.empty() ? what : what + ": " + reason;
}
bool succeeded(const Response &response, const Json &data) {
    return response.ok() && data.valid() && (!data.has("ok") || data["ok"].boolean());
}
double clamp(double value, double low, double high) {
    return std::max(low, std::min(high, value));
}

class QuickScreen final : public ScreenView {
    struct Row {
        enum class Kind { Plain, Slider, Choice, Switch } kind = Kind::Plain;
        std::string id, icon, title, trail;
        bool on = false, dim = false, go = false;
    };
    struct Reading {
        bool known = false;
        double value = 0;
    };

  public:
    explicit QuickScreen(Level level = Level::Main) : level_(level) {}

    std::string name() const override {
        return level_ == Level::Main ? "quick" : std::string("quick:") + levelName(level_);
    }
    bool hasBar() const override {
        return false;
    }
    bool paintsWall() const override {
        return true;
    }

    void enter(ScreenHost &host) override {
        host.motion().set("quick:x", kPanelW);
        host.motion().spring("quick:x", 0, kit::springParams(kSlide));
        statsGeneration_ = host.services().read(kStats, Operation::Metrics);
        nowGeneration_ = host.services().read(kNow, Operation::HomeNow);
        rebuild(host, true);
        focusMoved(host, true);
        playEntrance(host);
    }
    void leave(ScreenHost &host) override {
        // A change not yet sent goes now; the reply is not waited for.
        if (stepSum_)
            flushVolume(host, true);
        if (lightAt_) {
            lightAt_ = 0;
            send(host, kLight, control("backlight", Json::numeric(light_.value)));
        }
        for (const char *channel : {kStats, kNow, kSet, kAct, kLayout, kMute})
            host.services().cancel(channel);
        host.motion().erasePrefix("quick:");
    }
    bool update(ScreenHost &host) override {
        double now = host.nowMs();
        if (closing_ && !closed_ && !host.motion().moving("quick:x")) {
            closed_ = true;
            host.open(destination_);
            return false;
        }
        if (lightAt_ && now >= lightAt_) {
            lightAt_ = 0;
            lightGeneration_ = send(host, kLight, control("backlight", Json::numeric(light_.value)));
        }
        if (refreshAt_ && now >= refreshAt_) {
            refreshAt_ = 0;
            statsGeneration_ = host.services().read(kStats, Operation::Metrics);
        }
        return lightAt_ > 0 || refreshAt_ > 0;
    }

    // ---- Keys --------------------------------------------------------------------------------

    bool key(ScreenHost &host, Key key) override {
        if (closing_)
            return true;
        switch (key) {
        case Key::Up:
        case Key::Down:
        case Key::PageUp:
        case Key::PageDown: {
            int delta = (key == Key::Up || key == Key::PageUp ? -1 : 1) *
                        (key == Key::PageUp || key == Key::PageDown ? 6 : 1);
            size_t last = rows_.empty() ? 0 : rows_.size() - 1;
            size_t next = static_cast<size_t>(clamp(static_cast<double>(focus_) + delta, 0, static_cast<double>(last)));
            if (next != focus_) {
                focus_ = next;
                focusMoved(host, false);
            }
            return true;
        }
        case Key::Left:
        case Key::Right:
            sideways(host, key == Key::Right ? 1 : -1);
            return true;
        case Key::Ok:
            press(host);
            return true;
        default:
            return false;
        }
    }
    bool back(ScreenHost &host) override {
        if (closing_)
            return true;
        if (level_ != Level::Main)
            toMain(host);
        else
            close(host, returnRoute(host.cameFrom()));
        return true;
    }
    void click(ScreenHost &host, const std::string &id) override {
        if (closing_)
            return;
        if (id == "quick:close") {
            close(host, returnRoute(host.cameFrom()));
            return;
        }
        if (id.compare(0, 10, "quick:row:") != 0)
            return;
        size_t index = static_cast<size_t>(std::atoi(id.c_str() + 10));
        if (index >= rows_.size())
            return;
        focus_ = index;
        focusMoved(host, false);
        press(host);
    }

    // ---- Replies -----------------------------------------------------------------------------

    void response(ScreenHost &host, const Response &response) override {
        const std::string &channel = response.channel;
        Json data = response.json();
        bool ok = succeeded(response, data);
        if (channel == "catalogue") {
            // The layout was read again: what was switched is what the catalogue now says.
            layoutOverride_.clear();
            rebuild(host);
        } else if (channel == kStats && response.generation == statsGeneration_) {
            if (response.ok() && data.valid()) {
                applyStats(data);
                rebuild(host);
            }
        } else if (channel == kNow && response.generation == nowGeneration_) {
            nowPort_ = ok && data["kind"].string() == "input" ? data["port"].string() : "";
            rebuild(host);
        } else if (channel == kVolume && response.generation == volumeGeneration_) {
            stepBusy_ = false;
            if (!ok) {
                host.toast(failure(msg("tv.quick.failed", "That did not work"), data));
                stepSum_ = 0;
                refreshAt_ = host.nowMs();
            } else if (stepSum_)
                flushVolume(host, false);
            else
                refreshSoon(host);
        } else if (channel == kMute && response.generation == muteGeneration_) {
            if (!ok) {
                muted_ = !muted_;
                host.toast(failure(msg("tv.quick.failed", "That did not work"), data));
            } else
                refreshSoon(host);
        } else if (channel == kLight && response.generation == lightGeneration_) {
            if (!ok)
                host.toast(failure(msg("tv.quick.failed", "That did not work"), data));
            refreshSoon(host);
        } else if (channel == kSet && response.generation == setGeneration_) {
            setGeneration_ = 0;
            if (!ok) {
                host.toast(failure(msg("tv.quick.failed", "That did not work"), data));
            } else {
                chosen(host);
                refreshSoon(host);
            }
        } else if (channel == kAct && response.generation == actGeneration_) {
            actGeneration_ = 0;
            if (!ok)
                host.toast(failure(msg("tv.quick.failed", "That did not work"), data));
            else
                close(host, returnRoute(host.cameFrom()));
        } else if (channel == kLayout && response.generation == layoutGeneration_) {
            layoutGeneration_ = 0;
            if (!ok) {
                layoutOverride_.erase(switching_);
                host.toast(failure(msg("tv.quick.failed", "That did not work"), data));
            } else
                host.services().read("catalogue", Operation::HomeCatalogue);
            rebuild(host);
        } else
            return;
        host.invalidate();
    }

    // ---- Drawing -----------------------------------------------------------------------------

    void build(ScreenHost &host, kit::Paint paint) override {
        const Motion &motion = host.motion();
        Scene &scene = paint.scene;
        float slide = static_cast<float>(motion.value("quick:x"));
        float open = kit::focus::clamp01(1 - slide / kPanelW);
        if (host.cameFrom() == "home" && !host.view().home.tabs.empty())
            drawHome(paint, Frame{host.view(), motion, host.nowMs()});
        else
            kit::ambientWall(paint, 1, host.view().home.art.wallTint);
        kit::veil(paint, .62f * open);
        scene.hits.push_back({{0, 0, kPanelX, kit::canvas::height}, "quick:close"});

        scene.pushTransform(Transform::translate(slide, 0));
        Node &panel = scene.shape({kPanelX, 0, kPanelW, kit::canvas::height}, kit::color::surface1);
        panel.corners = {kit::radius::xl, 0, 0, kit::radius::xl};
        panel.ring = {1, 0, kit::alpha::outline};
        scene.hits.push_back({{kPanelX, 0, kPanelW, kit::canvas::height}, "quick:panel"});
        paint.glyphs.text(scene, {kPanelX + kPad, 56, kRowW, kit::type::headline.line}, title(),
                          kit::type::headline, kit::color::onSurface);

        scene.pushClip({kPanelX + slide, kViewTop, kPanelW, kViewBottom - kViewTop});
        float scroll = static_cast<float>(motion.value("quick:sy"));
        for (size_t i = 0; i < rows_.size(); ++i) {
            Rect rest = {kPanelX + kPad, kViewTop + layout::srowGap + static_cast<float>(i) * kRowPitch + scroll,
                         kRowW, layout::srowH};
            if (rest.y + rest.h < kViewTop - 40 || rest.y > kViewBottom + 40)
                continue;
            drawRow(host, paint, rows_[i], i, rest);
        }
        scene.popClip();
        if (!hint().empty())
            paint.glyphs.text(scene, {kPanelX + kPad, kit::canvas::height - 64, kRowW, kit::type::labelM.line},
                              hint(), kit::type::labelM, kit::color::onSurface3);
        scene.popTransform();
    }
    Json snapshot() const override {
        Json value = Json::object();
        value.set("level", Json::text(levelName(level_)));
        value.set("selected", Json::numeric(static_cast<double>(focus_)));
        if (focus_ < rows_.size())
            value.set("focus", Json::text(rows_[focus_].title));
        value.set("volume", Json::numeric(volume_.value));
        value.set("muted", Json::flag(muted_));
        value.set("light", Json::numeric(light_.value));
        value.set("closing", Json::flag(closing_));
        value.set("rows", Json::numeric(static_cast<double>(rows_.size())));
        return value;
    }

  private:
    // ---- State -------------------------------------------------------------------------------

    std::string title() const {
        switch (level_) {
        case Level::Picture:
            return msg("tv.quick.pictureMode", "Picture mode");
        case Level::Sound:
            return msg("tv.quick.sound", "Sound output");
        case Level::Input:
            return msg("tv.quick.input", "Input");
        case Level::Sleep:
            return msg("tv.quick.sleep", "Sleep timer");
        case Level::Layout:
            return msg("tv.quick.settings", "Home settings");
        default:
            return msg("tv.quick.title", "Quick settings");
        }
    }
    static std::string sleepLabel(const std::string &value) {
        return value == "off" ? msg("tv.quick.off", "Off")
                              : msg("tv.quick.minutes", "{n} min", fields({{"n", Json::text(value)}}));
    }
    std::string volumeText() const {
        if (!volume_.known)
            return "—";
        return muted_ ? msg("tv.quick.muted", "Muted") : std::to_string(static_cast<int>(volume_.value));
    }
    bool layoutOn(const ScreenHost &host, const std::string &key) const {
        auto found = layoutOverride_.find(key);
        if (found != layoutOverride_.end())
            return found->second;
        Json layout = host.view().catalogue["layout"];
        if (key == "clockFormat")
            return layout["clockFormat"].string() == "24h";
        if (key == "appNames" || key == "showInputs" || key == "showLgHome")
            return layout[key].boolean(true);
        return layout[key].boolean(false);
    }
    std::string activeProfile(const ScreenHost &host) const {
        for (const auto &person : host.view().catalogue["profiles"].items())
            if (person["active"].boolean())
                return person["name"].string();
        return "";
    }
    // The cards of the Inputs tab: the devices in the layout's order, less the ones hidden.
    std::vector<Card> inputs(const ScreenHost &host) const {
        std::vector<CardRow> rows = homeRows("inputs", host.view().catalogue, host.view().discover,
                                             host.view().guide);
        return rows.empty() ? std::vector<Card>() : rows[0].cards;
    }
    bool inputOn(const Card &card) const {
        return !nowPort_.empty() && card.data["port"].string() == nowPort_;
    }
    static std::string inputTitle(const Card &card) {
        return card.data["unused"].boolean()
                   ? msg("tv.quick.port", "HDMI {port}", fields({{"port", card.data["port"]}}))
                   : card.title;
    }

    void applyStats(const Json &stats) {
        stats_ = stats;
        if (!stepBusy_ && !stepSum_) {
            volume_.known = stats["volume"].isNumber();
            volume_.value = stats["volume"].number();
            muted_ = stats["muted"].boolean();
        }
        if (!lightAt_) {
            light_.known = stats["picture"]["backlight"].isNumber();
            light_.value = stats["picture"]["backlight"].number();
        }
    }
    void rebuild(ScreenHost &host, bool jump = false) {
        std::vector<Row> rows;
        auto add = [&](Row::Kind kind, const std::string &id, const std::string &icon,
                       const std::string &title, const std::string &trail = "", bool on = false,
                       bool go = false, bool dim = false) {
            Row row;
            row.kind = kind;
            row.id = id;
            row.icon = icon;
            row.title = title;
            row.trail = trail;
            row.on = on;
            row.go = go;
            row.dim = dim;
            rows.push_back(row);
        };
        const Json &s = stats_;
        if (level_ == Level::Main) {
            std::string current;
            for (const Card &card : inputs(host))
                if (inputOn(card))
                    current = inputTitle(card);
            bool oled = s["oled"].valid() && (s["oled"].isObject() || s["oled"].boolean());
            std::string output = s["sound"]["output"].string(s["audio_output"].string("—"));
            add(Row::Kind::Slider, "volume", "volume", msg("tv.quick.volume", "Volume"), "", false, false,
                !volume_.known);
            add(Row::Kind::Plain, "picture", "picture", msg("tv.quick.pictureMode", "Picture mode"),
                s["picture"]["mode"].string("—"), false, true);
            add(Row::Kind::Slider, "light", "light",
                oled ? msg("tv.quick.oledLight", "OLED light") : msg("tv.quick.backlight", "Backlight"), "",
                false, false, !light_.known);
            add(Row::Kind::Plain, "sound", "speaker", msg("tv.quick.sound", "Sound output"), output, false, true);
            add(Row::Kind::Plain, "input", "hdmi", msg("tv.quick.input", "Input"), current, false, true);
            add(Row::Kind::Plain, "sleep", "clock", msg("tv.quick.sleep", "Sleep timer"),
                s["sleepTimer"].string().empty() ? "—" : sleepLabel(s["sleepTimer"].string()), false, true);
            add(Row::Kind::Plain, "saver", "monitor", msg("tv.quick.screensaver", "Screensaver"));
            add(Row::Kind::Plain, "off", "power", msg("tv.quick.screenOff", "Screen off"));
            add(Row::Kind::Plain, "recents", "apps", msg("tv.quick.recents", "Recent apps"), "", false, true);
            add(Row::Kind::Plain, "profile", "profile", msg("tv.quick.profile", "Profile"), activeProfile(host),
                false, true);
            add(Row::Kind::Plain, "layout", "gear", msg("tv.quick.settings", "Home settings"), "", false, true);
        } else if (level_ == Level::Picture) {
            for (const auto &mode : s["picture"]["modes"].items())
                add(Row::Kind::Choice, mode["value"].string(), "picture", mode["label"].string(), "",
                    mode["value"].string() == s["picture"]["mode_raw"].string());
        } else if (level_ == Level::Sound) {
            for (const auto &output : s["sound"]["outputs"].items())
                add(Row::Kind::Choice, output["id"].string(), "speaker", output["name"].string(), "",
                    output["id"].string() == s["sound"]["output_raw"].string());
        } else if (level_ == Level::Input) {
            for (const Card &card : inputs(host))
                add(Row::Kind::Choice, card.id, "hdmi", inputTitle(card),
                    card.data["unused"].boolean() ? msg("tv.quick.nothingConnected", "Nothing connected") : "",
                    inputOn(card), false, card.data["unused"].boolean());
        } else if (level_ == Level::Sleep) {
            for (const char *value : kSleep)
                add(Row::Kind::Choice, value, "timer", sleepLabel(value), "",
                    s["sleepTimer"].string() == value);
        } else {
            static const std::map<std::string, std::string> titles = {
                {"clockFormat", msg("tv.quick.clock24", "24-hour clock")},
                {"appNames", msg("tv.quick.appNames", "App names")},
                {"showLive", msg("tv.quick.showLive", "Live tab")},
                {"showInputs", msg("tv.quick.showInputs", "Inputs tab")},
                {"showLgHome", msg("tv.quick.showLgHome", "LG Home tile")},
                {"picture", msg("tv.quick.picture", "Picture details")}};
            for (const LayoutSwitch &entry : kSwitches)
                add(Row::Kind::Switch, entry.key, entry.icon, titles.at(entry.key), "",
                    layoutOn(host, entry.key));
        }
        if (rows.empty())
            add(Row::Kind::Plain, "", "", msg("tv.quick.nothing", "Nothing to choose from"), "", false, false, true);
        rows_ = rows;
        focus_ = std::min(focus_, rows_.size() - 1);
        // Switches slide to where the catalogue (or a press) put them.
        for (size_t i = 0; i < rows_.size(); ++i)
            if (rows_[i].kind == Row::Kind::Switch) {
                std::string id = "quick:t:" + std::to_string(i);
                if (jump)
                    host.motion().set(id, rows_[i].on ? 1 : 0);
                else
                    host.motion().tween(id, rows_[i].on ? 1 : 0, kit::motion::ms::fast,
                                        easing(kit::motion::ease::out));
            }
        host.invalidate();
    }
    std::string hint() const {
        if (focus_ >= rows_.size() || closing_)
            return "";
        const Row &row = rows_[focus_];
        if (level_ == Level::Main && row.id == "volume")
            return msg("tv.quick.volumeHint", "Left and Right adjust. OK mutes.");
        if (level_ == Level::Main && row.id == "light")
            return msg("tv.quick.lightHint", "Left and Right adjust.");
        if (level_ == Level::Layout)
            return msg("tv.quick.switchHint", "OK switches. Left or Back returns.");
        if (level_ != Level::Main)
            return msg("tv.quick.levelHint", "OK chooses. Left or Back returns.");
        return "";
    }

    // ---- Motion ------------------------------------------------------------------------------

    void focusMoved(ScreenHost &host, bool jump) {
        Motion &motion = host.motion();
        for (size_t i = 0; i < rows_.size(); ++i) {
            double to = i == focus_ ? 1 : 0;
            std::string id = "quick:f:" + std::to_string(i);
            if (jump)
                motion.set(id, to);
            else
                motion.tween(id, to, kit::motion::ms::focus, easing(kit::motion::ease::out));
        }
        const float visible = std::floor((kViewBottom - kViewTop) / kRowPitch);
        float room = std::max(0.0f, static_cast<float>(rows_.size()) * kRowPitch + 2 * layout::srowGap -
                                        (kViewBottom - kViewTop));
        double target = -clamp((static_cast<double>(focus_) - (visible - 3)) * kRowPitch, 0, room);
        if (jump)
            motion.set("quick:sy", target);
        else
            motion.spring("quick:sy", target, kit::springParams(kit::motion::spring::list));
        host.invalidate();
    }
    void playEntrance(ScreenHost &host) {
        Timeline timeline;
        Tween rise(1, 0, kit::motion::ms::base, easing(kit::motion::ease::out));
        for (size_t i = 0; i < rows_.size() && i < 14; ++i) {
            std::string id = "quick:e:" + std::to_string(i);
            host.motion().set(id, 1);
            timeline.add(id, rise.delayedBy(kit::motion::stagger::pageItem * static_cast<double>(i) + 60));
        }
        timeline.play(host.motion());
    }
    void drawRow(ScreenHost &host, kit::Paint paint, const Row &row, size_t index, Rect rest) {
        const Motion &motion = host.motion();
        Scene &scene = paint.scene;
        std::string number = std::to_string(index);
        double hidden = index < 14 ? motion.value("quick:e:" + number) : 0;
        kit::SettingsRow item;
        item.rest = rest;
        item.id = "quick:row:" + number;
        item.icon = row.icon;
        item.title = row.title;
        item.focus = static_cast<float>(motion.value("quick:f:" + number));
        item.enter = {static_cast<float>(1 - hidden) * (row.dim ? .5f : 1.0f), static_cast<float>(20 * hidden)};
        if (row.kind == Row::Kind::Switch) {
            item.toggle = true;
            item.on = static_cast<float>(motion.value("quick:t:" + number));
        } else if (row.kind == Row::Kind::Slider)
            item.value = row.id == "volume" ? volumeText() : light_.known ? std::to_string(static_cast<int>(light_.value)) : "—";
        else
            item.value = row.trail;
        kit::settingsRow(paint, item);
        if (row.kind != Row::Kind::Slider && row.kind != Row::Kind::Choice)
            return;
        // The mark or the bar scales and fades with the row it sits on.
        kit::focus::Look look = kit::focus::lookAt(rest, kit::motion::scale::row, item.focus,
                                                   kit::focus::leftCentre);
        scene.pushOpacity(item.enter.alpha);
        scene.pushTransform(Transform::translate(0, item.enter.dy));
        scene.pushTransform(kit::focus::transformAt(rest, look.scale, kit::focus::leftCentre));
        float right = rest.x + rest.w - layout::srowPadRight;
        if (row.kind == Row::Kind::Choice) {
            kit::radio(paint, {right - 28, rest.y + (rest.h - 28) / 2, 28, 28}, row.on ? 1.0f : 0.0f,
                       item.focus);
        } else {
            bool volume = row.id == "volume";
            double level = volume ? (volume_.known ? volume_.value : 0) : (light_.known ? light_.value : 0);
            float amount = static_cast<float>(clamp(level, 0, 100) / 100);
            if (volume && muted_)
                amount = 0;
            Rect track = {right - 84 - 16 - 160, rest.y + (rest.h - layout::barH) / 2, 160, layout::barH};
            scene.shape(track, kit::focus::mix(kit::color::surface3, Color{0.075f, 0.067f, 0.094f, .14f}, item.focus),
                        layout::barH / 2);
            if (amount > 0) {
                Node &fill = scene.shape({track.x, track.y, track.w * amount, track.h},
                                         kit::focus::inkAt(kit::color::accent, kit::color::onAccent, item.focus));
                float end = amount >= .999f ? layout::barH / 2 : 0;
                fill.corners = {layout::barH / 2, end, end, layout::barH / 2};
            }
        }
        scene.popTransform();
        scene.popTransform();
        scene.popOpacity();
    }

    // ---- Doing things ------------------------------------------------------------------------

    unsigned send(ScreenHost &host, const char *channel, const Command &command) {
        return host.services().write(channel, command.operation, command.body);
    }
    void refreshSoon(ScreenHost &host) {
        refreshAt_ = host.nowMs() + kStepMs;
    }
    void close(ScreenHost &host, const std::string &destination) {
        if (closing_)
            return;
        closing_ = true;
        destination_ = destination;
        host.motion().spring("quick:x", kPanelW, kit::springParams(kSlide));
        host.invalidate();
    }
    void openLevel(ScreenHost &host, Level level) {
        level_ = level;
        focus_ = 0;
        rebuild(host, true);
        // A list of choices opens on the one in force; a list of switches, on its first.
        for (size_t i = 0; i < rows_.size(); ++i)
            if (rows_[i].on && rows_[i].kind == Row::Kind::Choice)
                focus_ = i;
        host.motion().erasePrefix("quick:f:");
        focusMoved(host, true);
        playEntrance(host);
    }
    void toMain(ScreenHost &host) {
        std::string opener = rowOpening(level_);
        level_ = Level::Main;
        rebuild(host, true);
        for (size_t i = 0; i < rows_.size(); ++i)
            if (rows_[i].id == opener)
                focus_ = i;
        host.motion().erasePrefix("quick:f:");
        focusMoved(host, true);
        playEntrance(host);
    }
    void sideways(ScreenHost &host, int direction) {
        if (focus_ >= rows_.size())
            return;
        const Row &row = rows_[focus_];
        if (level_ != Level::Main) {
            if (direction < 0)
                toMain(host);
            return;
        }
        if (row.id == "volume")
            stepVolume(host, direction);
        else if (row.id == "light")
            stepLight(host, direction * kLightStep);
    }
    void stepVolume(ScreenHost &host, int direction) {
        if (!volume_.known)
            return;
        // Only what the volume really moved is sent: at the end of the range a held key is idle.
        double moved = clamp(volume_.value + direction, 0, 100) - volume_.value;
        if (moved == 0)
            return;
        volume_.value += moved;
        stepSum_ += static_cast<int>(moved);
        flushVolume(host, false);
        host.invalidate();
    }
    // One request at a time; what was pressed meanwhile goes as a single step when it answers.
    void flushVolume(ScreenHost &host, bool leaving) {
        if ((stepBusy_ && !leaving) || !stepSum_)
            return;
        int steps = stepSum_;
        stepSum_ = 0;
        volumeGeneration_ = send(host, kVolume, control("volumeStep", Json::numeric(steps)));
        stepBusy_ = volumeGeneration_ != 0;
        if (!volumeGeneration_ && !leaving) {
            stepSum_ = steps;
            host.toast(msg("tv.quick.busy", "Too many pending requests. Try again."));
        }
    }
    void stepLight(ScreenHost &host, int amount) {
        if (!light_.known)
            return;
        light_.value = clamp(light_.value + amount, 0, 100);
        lightAt_ = host.nowMs() + kLightDebounceMs;
        host.invalidate();
    }
    void press(ScreenHost &host) {
        if (focus_ >= rows_.size())
            return;
        const Row &row = rows_[focus_];
        if (row.dim && !row.go)
            return;
        if (level_ != Level::Main) {
            choose(host, row);
            return;
        }
        if (row.id == "volume") {
            if (!volume_.known)
                return;
            muted_ = !muted_;
            muteGeneration_ = send(host, kMute, control("mute", Json::flag(muted_)));
            host.invalidate();
        } else if (row.id == "picture")
            openLevel(host, Level::Picture);
        else if (row.id == "sound")
            openLevel(host, Level::Sound);
        else if (row.id == "input")
            openLevel(host, Level::Input);
        else if (row.id == "sleep")
            openLevel(host, Level::Sleep);
        else if (row.id == "layout")
            openLevel(host, Level::Layout);
        else if (row.id == "saver" || row.id == "off") {
            if (actGeneration_)
                return;
            actGeneration_ = send(host, kAct, control(row.id == "saver" ? "screensaver" : "screenOff"));
        } else if (row.id == "recents")
            close(host, "recents");
        else if (row.id == "profile")
            close(host, "profiles");
    }
    void choose(ScreenHost &host, const Row &row) {
        if (level_ == Level::Layout) {
            if (layoutGeneration_)
                return;
            bool on = !row.on;
            Json body = Json::object();
            body.set(row.id, row.id == "clockFormat" ? Json::text(on ? "24h" : "12h") : Json::flag(on));
            layoutGeneration_ = host.services().write(kLayout, Operation::SetHomeLayout, body);
            if (layoutGeneration_) {
                switching_ = row.id;
                layoutOverride_[row.id] = on;
                rebuild(host);
            }
            return;
        }
        if (setGeneration_ || actGeneration_)
            return;
        pending_ = row.id;
        pendingTitle_ = row.title;
        if (level_ == Level::Input) {
            Json body = Json::object();
            body.set("kind", Json::text("dev"));
            body.set("id", Json::text(row.id));
            actGeneration_ = host.services().write(kAct, Operation::OpenHomeItem, body);
            return;
        }
        const char *action = level_ == Level::Picture ? "pictureMode"
                             : level_ == Level::Sound ? "soundOutput"
                                                      : "sleepTimer";
        setGeneration_ = send(host, kSet, control(action, Json::text(row.id)));
    }
    // A choice was taken: what the TV is on is what was chosen, and the list goes back to the main
    // one on its row.
    void chosen(ScreenHost &host) {
        if (level_ == Level::Picture) {
            stats_["picture"].set("mode_raw", Json::text(pending_));
            stats_["picture"].set("mode", Json::text(pendingTitle_));
        } else if (level_ == Level::Sound) {
            stats_["sound"].set("output_raw", Json::text(pending_));
            stats_["sound"].set("output", Json::text(pendingTitle_));
        } else if (level_ == Level::Sleep)
            stats_.set("sleepTimer", Json::text(pending_));
        toMain(host);
    }

    Level level_;
    Json stats_ = Json::object();
    Reading volume_, light_;
    bool muted_ = false;
    std::string nowPort_;
    std::vector<Row> rows_;
    size_t focus_ = 0;
    int stepSum_ = 0;
    bool stepBusy_ = false, closing_ = false, closed_ = false;
    double lightAt_ = 0, refreshAt_ = 0;
    unsigned statsGeneration_ = 0, nowGeneration_ = 0, volumeGeneration_ = 0, muteGeneration_ = 0,
             lightGeneration_ = 0, setGeneration_ = 0, actGeneration_ = 0, layoutGeneration_ = 0;
    std::string destination_, pending_, pendingTitle_, switching_;
    std::map<std::string, bool> layoutOverride_;
};
} // namespace

std::unique_ptr<ScreenView> makeQuickScreen(const std::string &route) {
    return route == "quick" ? std::unique_ptr<ScreenView>(new QuickScreen()) : nullptr;
}
} // namespace nativetv
