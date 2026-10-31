// The Quick settings drawer: a panel that springs in from the right over the page it was opened
// from, with the TV's volume, picture mode, light, sound output, input and sleep timer, the
// screensaver and Screen off, the recent apps, the profile and Home's own switches.
//
// Opening it shows each row's last known value at once (kept in the application between openings)
// or a spinner where there is none yet, and reads the TV behind it. Every change goes through the
// optimistic-update layer (optimistic.hpp): the value is on screen the moment the key is pressed,
// the writes leave at a limited pace and carry the newest value, and a refusal puts the value back
// and says so. Volume and light are absolute targets (/api/control "volume" and "backlight"), so a
// held key from 5 to 95 shows 5, 6, 7 ... 95 and nothing a late reply or a re-read brings can move
// it backwards.
#include "domain/models.hpp"
#include "domain/say.hpp"
#include "ui/application.hpp"
#include "ui/kit/overlay.hpp"
#include "ui/kit/tuning.hpp"
#include "ui/optimistic.hpp"
#include "ui/overlay_routes.hpp"
#include "ui/profile_names.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace nativetv {
namespace {
namespace layout = kit::layout;
using kit::easing;
using say::msg;

constexpr float kPanelW = 760, kPanelX = kit::canvas::width - kPanelW, kPad = 48,
                kRowW = kPanelW - 2 * kPad, kViewTop = 140, kViewBottom = kit::canvas::height - 96,
                kRowPitch = 92;
constexpr int kLightStep = 5;
constexpr double kRetryMs = 3000;
// The panel's own spring: quicker than a page's, still without overshoot.
constexpr kit::SpringTuning kSlide = {420, 1};
constexpr const char *kStats = "quick:stats", *kNow = "quick:now";
constexpr const char *kStatsMemory = "quick.stats", *kNowMemory = "quick.now";

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

double clamp(double value, double low, double high) {
    return std::max(low, std::min(high, value));
}
Write controlWrite(const char *action) {
    Write write;
    write.operation = Operation::Control;
    const std::string name = action;
    write.body = [name](const Json &target) {
        Json body = Json::object();
        body.set("action", Json::text(name));
        body.set("value", target);
        return body;
    };
    write.readChannel = kStats;
    write.readOperation = Operation::Metrics;
    return write;
}
Write layoutWrite(const std::string &key) {
    Write write;
    write.operation = Operation::SetHomeLayout;
    write.body = [key](const Json &target) {
        Json body = Json::object();
        body.set(key, key == "clockFormat" ? Json::text(target.boolean() ? "24h" : "12h") : target);
        return body;
    };
    write.readChannel = "catalogue";
    write.readOperation = Operation::HomeCatalogue;
    return write;
}
// Rows whose value is read from the TV: they wait for the first read.
bool readsStats(const std::string &id) {
    return id == "volume" || id == "light" || id == "picture" || id == "sound" || id == "input" ||
           id == "sleep";
}

class QuickScreen final : public ScreenView {
    struct Row {
        enum class Kind { Plain, Slider, Choice, Switch } kind = Kind::Plain;
        std::string id, icon, title, raw, trail;
        bool go = false, dim = false, on = false;
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
        // What was read last time shows at once; the read behind it brings today's values.
        auto &memory = host.memory();
        if (memory.count(kStatsMemory) && memory[kStatsMemory].isObject()) {
            stats_ = memory[kStatsMemory];
            loaded_ = true;
        }
        if (memory.count(kNowMemory) && memory[kNowMemory].isObject())
            nowPort_ = memory[kNowMemory]["port"].string();
        statsGeneration_ = host.services().read(kStats, Operation::Metrics);
        nowGeneration_ = host.services().read(kNow, Operation::HomeNow);
        syncShown(host);
        rebuild(host, true);
        focusMoved(host, true);
        playEntrance(host);
    }
    void leave(ScreenHost &host) override {
        // Writes in flight are the layer's and finish without this page.
        for (const char *channel : {kStats, kNow})
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
        if (retryAt_ > 0 && now >= retryAt_) {
            retryAt_ = 0;
            statsGeneration_ = host.services().read(kStats, Operation::Metrics);
        }
        if (needsRebuild_) {
            needsRebuild_ = false;
            rebuild(host, false);
        }
        syncShown(host);
        // The switches follow what is shown, so a refusal that takes one back moves it back.
        if (level_ == Level::Layout)
            for (size_t i = 0; i < rows_.size(); ++i)
                host.motion().tween("quick:t:" + std::to_string(i), layoutFlag(host, rows_[i].id) ? 1 : 0,
                                    kit::motion::ms::fast, easing(kit::motion::ease::out));
        // The spinner turns while anything is being read.
        return !loaded_ || retryAt_ > 0;
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

    // What the TV said. The layer decides what shows while a change of ours is pending, so what is
    // read is simply taken.
    void response(ScreenHost &host, const Response &response) override {
        const std::string &channel = response.channel;
        Json data = response.json();
        bool ok = response.ok() && data.valid() && (!data.has("ok") || data["ok"].boolean());
        if (channel == "catalogue") {
            needsRebuild_ = true; // the inputs and the profile names come from it
        } else if (channel == kStats && response.generation >= statsGeneration_) {
            if (ok) {
                stats_ = data;
                host.memory()[kStatsMemory] = data;
                loaded_ = true;
                readFailed_ = false;
                retryAt_ = 0;
                needsRebuild_ = true;
            } else if (!loaded_) {
                // Nothing to show yet and the TV did not answer: dashes, and another try.
                loaded_ = true;
                readFailed_ = true;
                retryAt_ = host.nowMs() + kRetryMs;
                needsRebuild_ = true;
            }
        } else if (channel == kNow && response.generation >= nowGeneration_) {
            nowPort_ = ok && data["kind"].string() == "input" ? data["port"].string() : "";
            if (ok)
                host.memory()[kNowMemory] = data;
            needsRebuild_ = true;
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
        syncShown(host);
        if (host.cameFrom() == "home" && !host.view().home.tabs.empty())
            drawHome(paint, Frame{host.view(), motion, host.nowMs()});
        else
            kit::ambientWall(paint, 1, host.view().home.art.wallTint);
        kit::veil(paint, .62f * open); // the one dim scrim over the page
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
                              hint(), kit::type::labelM,
                              readFailed_ ? kit::color::warn : kit::color::onSurface3);
        scene.popTransform();
    }
    Json snapshot() const override {
        Json value = Json::object();
        value.set("level", Json::text(levelName(level_)));
        value.set("selected", Json::numeric(static_cast<double>(focus_)));
        if (focus_ < rows_.size())
            value.set("focus", Json::text(rows_[focus_].title));
        value.set("volume", Json::numeric(shownVolume_));
        value.set("muted", Json::flag(shownMuted_));
        value.set("light", Json::numeric(shownLight_));
        value.set("sleep", Json::text(shownSleep_));
        value.set("picture", Json::text(shownPicture_));
        value.set("writing", Json::flag(writing_));
        value.set("loading", Json::flag(!loaded_));
        value.set("readFailed", Json::flag(readFailed_));
        value.set("closing", Json::flag(closing_));
        value.set("rows", Json::numeric(static_cast<double>(rows_.size())));
        value.set("volumeBackwards", Json::numeric(volumeBackwards_));
        value.set("lightBackwards", Json::numeric(lightBackwards_));
        return value;
    }

  private:
    // ---- What is shown -----------------------------------------------------------------------

    bool volumeKnown() const {
        return stats_["volume"].isNumber();
    }
    bool lightKnown() const {
        return stats_["picture"]["backlight"].isNumber();
    }
    // The values on screen, as the layer decides them: the wanted one while a change is pending.
    double volume(ScreenHost &host) {
        return host.optimistic().number("quick.volume", stats_["volume"].number(0));
    }
    double light(ScreenHost &host) {
        return host.optimistic().number("quick.light", stats_["picture"]["backlight"].number(0));
    }
    bool muted(ScreenHost &host) {
        return host.optimistic().flag("quick.mute", stats_["muted"].boolean());
    }
    std::string sleepValue(ScreenHost &host) {
        return host.optimistic().text("quick.sleep", stats_["sleepTimer"].string());
    }
    std::string pictureValue(ScreenHost &host) {
        return host.optimistic().text("quick.picture", stats_["picture"]["mode_raw"].string());
    }
    std::string soundValue(ScreenHost &host) {
        return host.optimistic().text("quick.sound", stats_["sound"]["output_raw"].string());
    }
    // What the catalogue says of a Home switch; the clock is "24h" or "12h".
    static bool layoutServer(const ScreenHost &host, const std::string &key) {
        Json layout = host.view().catalogue["layout"];
        return key == "clockFormat"                                                 ? layout["clockFormat"].string() == "24h"
               : key == "appNames" || key == "showInputs" || key == "showLgHome" ? layout[key].boolean(true)
                                                                                  : layout[key].boolean(false);
    }
    bool layoutFlag(ScreenHost &host, const std::string &key) {
        return host.optimistic().flag("layout." + key, layoutServer(host, key));
    }
    // For the snapshot and for the held-key checks: what is shown now, and whether it ever went
    // against the key that is held.
    void syncShown(ScreenHost &host) {
        double volumeNow = volume(host), lightNow = light(host);
        if (volumeDirection_ && (volumeNow - shownVolume_) * volumeDirection_ < 0)
            ++volumeBackwards_;
        if (lightDirection_ && (lightNow - shownLight_) * lightDirection_ < 0)
            ++lightBackwards_;
        shownVolume_ = volumeNow;
        shownLight_ = lightNow;
        shownMuted_ = muted(host);
        shownSleep_ = sleepValue(host);
        shownPicture_ = pictureValue(host);
        writing_ = false;
        for (const char *key : {"quick.volume", "quick.light", "quick.mute", "quick.sleep", "quick.picture", "quick.sound"})
            writing_ = writing_ || host.optimistic().pending(key);
    }
    std::string modeLabel(const std::string &raw) const {
        for (const auto &mode : stats_["picture"]["modes"].items())
            if (mode["value"].string() == raw)
                return mode["label"].string();
        return raw == stats_["picture"]["mode_raw"].string() ? stats_["picture"]["mode"].string() : raw;
    }
    std::string outputLabel(const std::string &raw) const {
        for (const auto &output : stats_["sound"]["outputs"].items())
            if (output["id"].string() == raw)
                return output["name"].string();
        return raw == stats_["sound"]["output_raw"].string() ? stats_["sound"]["output"].string() : raw;
    }
    static std::string sleepLabel(const std::string &value) {
        return value == "off" ? msg("tv.quick.off", "Off")
                              : msg("tv.quick.minutes", "{n} min", fields({{"n", Json::text(value)}}));
    }
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
    std::string activeProfile(const ScreenHost &host) const {
        std::string model = stats_["device"]["model"].string();
        for (const auto &person : host.view().catalogue["profiles"].items())
            if (person["active"].boolean())
                return profileName(person, model);
        return "";
    }
    // The text at the right of a row.
    std::string trail(ScreenHost &host, const Row &row) {
        if (row.kind != Row::Kind::Plain)
            return row.trail;
        if (readFailed_ && readsStats(row.id))
            return "—";
        if (row.id == "picture") {
            std::string raw = pictureValue(host);
            return raw.empty() ? "—" : modeLabel(raw);
        }
        if (row.id == "sound") {
            std::string raw = soundValue(host);
            return raw.empty() ? stats_["audio_output"].string("—") : outputLabel(raw);
        }
        if (row.id == "input") {
            for (const Card &card : inputs(host))
                if (inputOn(card))
                    return inputTitle(card);
            return "";
        }
        if (row.id == "sleep") {
            std::string value = sleepValue(host);
            return value.empty() ? "—" : sleepLabel(value);
        }
        if (row.id == "profile")
            return activeProfile(host);
        return "";
    }
    bool rowOn(ScreenHost &host, const Row &row) {
        if (row.kind == Row::Kind::Switch)
            return layoutFlag(host, row.id);
        if (level_ == Level::Picture)
            return row.raw == pictureValue(host);
        if (level_ == Level::Sound)
            return row.raw == soundValue(host);
        if (level_ == Level::Sleep)
            return row.raw == sleepValue(host);
        return row.on;
    }
    void rebuild(ScreenHost &host, bool jump) {
        std::vector<Row> rows;
        auto add = [&](Row::Kind kind, const std::string &id, const std::string &icon,
                       const std::string &title, const std::string &raw = "", bool go = false,
                       bool dim = false, bool on = false, const std::string &trail = "") {
            Row row;
            row.kind = kind;
            row.id = id;
            row.icon = icon;
            row.title = title;
            row.raw = raw;
            row.go = go;
            row.dim = dim;
            row.on = on;
            row.trail = trail;
            rows.push_back(row);
        };
        const Json &s = stats_;
        if (level_ == Level::Main) {
            bool oled = s["oled"].valid() && (s["oled"].isObject() || s["oled"].boolean());
            add(Row::Kind::Slider, "volume", "volume", msg("tv.quick.volume", "Volume"));
            add(Row::Kind::Plain, "picture", "picture", msg("tv.quick.pictureMode", "Picture mode"), "", true);
            add(Row::Kind::Slider, "light", "light",
                oled ? msg("tv.quick.oledLight", "OLED light") : msg("tv.quick.backlight", "Backlight"));
            add(Row::Kind::Plain, "sound", "speaker", msg("tv.quick.sound", "Sound output"), "", true);
            add(Row::Kind::Plain, "input", "hdmi", msg("tv.quick.input", "Input"), "", true);
            add(Row::Kind::Plain, "sleep", "clock", msg("tv.quick.sleep", "Sleep timer"), "", true);
            add(Row::Kind::Plain, "saver", "monitor", msg("tv.quick.screensaver", "Screensaver"));
            add(Row::Kind::Plain, "off", "power", msg("tv.quick.screenOff", "Screen off"));
            add(Row::Kind::Plain, "recents", "apps", msg("tv.quick.recents", "Recent apps"), "", true);
            add(Row::Kind::Plain, "profile", "profile", msg("tv.quick.profile", "Profile"), "", true);
            add(Row::Kind::Plain, "layout", "gear", msg("tv.quick.settings", "Home settings"), "", true);
        } else if (level_ == Level::Picture) {
            for (const auto &mode : s["picture"]["modes"].items())
                add(Row::Kind::Choice, mode["value"].string(), "picture", mode["label"].string(),
                    mode["value"].string());
        } else if (level_ == Level::Sound) {
            for (const auto &output : s["sound"]["outputs"].items())
                add(Row::Kind::Choice, output["id"].string(), "speaker", output["name"].string(),
                    output["id"].string());
        } else if (level_ == Level::Input) {
            for (const Card &card : inputs(host))
                add(Row::Kind::Choice, card.id, "hdmi", inputTitle(card), card.id, false,
                    card.data["unused"].boolean(), inputOn(card),
                    card.data["unused"].boolean() ? msg("tv.quick.nothingConnected", "Nothing connected") : "");
        } else if (level_ == Level::Sleep) {
            for (const char *value : kSleep)
                add(Row::Kind::Choice, value, "timer", sleepLabel(value), value);
        } else {
            static const std::vector<std::pair<std::string, std::string>> titles = {
                {"clockFormat", msg("tv.quick.clock24", "24-hour clock")},
                {"appNames", msg("tv.quick.appNames", "App names")},
                {"showLive", msg("tv.quick.showLive", "Live tab")},
                {"showInputs", msg("tv.quick.showInputs", "Inputs tab")},
                {"showLgHome", msg("tv.quick.showLgHome", "LG Home tile")},
                {"picture", msg("tv.quick.picture", "Picture details")}};
            for (const LayoutSwitch &entry : kSwitches)
                for (const auto &title : titles)
                    if (title.first == entry.key)
                        add(Row::Kind::Switch, entry.key, entry.icon, title.second);
        }
        if (rows.empty())
            add(Row::Kind::Plain, "", "", msg("tv.quick.nothing", "Nothing to choose from"), "", false, true);
        rows_ = rows;
        focus_ = std::min(focus_, rows_.size() - 1);
        // Switches slide to where they are shown.
        for (size_t i = 0; i < rows_.size(); ++i)
            if (rows_[i].kind == Row::Kind::Switch) {
                std::string id = "quick:t:" + std::to_string(i);
                double to = layoutFlag(host, rows_[i].id) ? 1 : 0;
                if (jump)
                    host.motion().set(id, to);
                else
                    host.motion().tween(id, to, kit::motion::ms::fast, easing(kit::motion::ease::out));
            }
        host.invalidate();
    }
    bool rowDim(const Row &row) const {
        if (row.dim)
            return true;
        if (!loaded_ && readsStats(row.id) && level_ == Level::Main)
            return true;
        return (row.id == "volume" && !volumeKnown()) || (row.id == "light" && !lightKnown());
    }
    std::string hint() const {
        if (closing_)
            return "";
        if (readFailed_)
            return msg("tv.quick.noAnswer", "The TV did not answer. Trying again…");
        if (focus_ >= rows_.size())
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
        bool dim = rowDim(row);
        bool waiting = !loaded_ && readsStats(row.id) && level_ == Level::Main;
        kit::SettingsRow item;
        item.rest = rest;
        item.id = "quick:row:" + number;
        item.icon = row.icon;
        item.title = row.title;
        item.focus = static_cast<float>(motion.value("quick:f:" + number));
        item.enter = {static_cast<float>(1 - hidden) * (dim ? .6f : 1.0f), static_cast<float>(20 * hidden)};
        if (row.kind == Row::Kind::Switch) {
            item.toggle = true;
            item.on = static_cast<float>(motion.value("quick:t:" + number));
        } else if (row.kind == Row::Kind::Slider) {
            if (!waiting)
                item.value = row.id == "volume" ? volumeText() : lightKnown() ? std::to_string(static_cast<int>(shownLight_)) : "—";
        } else if (!waiting) {
            item.value = trail(host, row);
            // The rows Left and Right step through the values of show a chevron on each side.
            item.stepper = level_ == Level::Main && steps(row.id) && loaded_ && !dim;
        }
        kit::settingsRow(paint, item);
        float right = rest.x + rest.w - layout::srowPadRight;
        // A value still being read is a turning spinner where the number goes.
        if (waiting) {
            scene.pushOpacity(item.enter.alpha);
            scene.pushTransform(Transform::translate(0, item.enter.dy));
            kit::focus::Look look = kit::focus::lookAt(rest, kit::motion::scale::row, item.focus, kit::focus::leftCentre);
            scene.pushTransform(kit::focus::transformAt(rest, look.scale, kit::focus::leftCentre));
            kit::spinner(paint, {right - 28, rest.y + (rest.h - 28) / 2, 28, 28}, host.nowMs(),
                         kit::focus::inkAt(kit::color::onSurface2, kit::color::onFocus, item.focus));
            scene.popTransform();
            scene.popTransform();
            scene.popOpacity();
            return;
        }
        if (row.kind != Row::Kind::Slider && row.kind != Row::Kind::Choice)
            return;
        // The mark or the bar scales and fades with the row it sits on.
        kit::focus::Look look = kit::focus::lookAt(rest, kit::motion::scale::row, item.focus,
                                                   kit::focus::leftCentre);
        scene.pushOpacity(item.enter.alpha);
        scene.pushTransform(Transform::translate(0, item.enter.dy));
        scene.pushTransform(kit::focus::transformAt(rest, look.scale, kit::focus::leftCentre));
        if (row.kind == Row::Kind::Choice) {
            kit::radio(paint, {right - 28, rest.y + (rest.h - 28) / 2, 28, 28}, rowOn(host, row) ? 1.0f : 0.0f,
                       item.focus);
        } else {
            bool isVolume = row.id == "volume";
            double level = isVolume ? (volumeKnown() ? shownVolume_ : 0) : (lightKnown() ? shownLight_ : 0);
            std::string barId = std::string("quick:bar:") + row.id;
            // The bar follows the value with a short glide, so each key repeat moves it a little.
            host.motion().tween(barId, clamp(level, 0, 100), 90, easing(kit::motion::ease::out));
            float amount = static_cast<float>(clamp(host.motion().value(barId), 0, 100) / 100);
            if (isVolume && shownMuted_)
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
    std::string volumeText() const {
        if (!volumeKnown())
            return "—";
        return shownMuted_ ? msg("tv.quick.muted", "Muted") : std::to_string(static_cast<int>(shownVolume_));
    }

    // ---- Doing things ------------------------------------------------------------------------

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
            if (rows_[i].kind == Row::Kind::Choice && rowOn(host, rows_[i]))
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
        else if (steps(row.id))
            stepList(host, row.id, direction);
    }
    // Picture mode, Sound output and the Sleep timer have a list of values: OK opens it, and Left and Right
    // go to the previous or the next one without opening it.
    static bool steps(const std::string &id) {
        return id == "picture" || id == "sound" || id == "sleep";
    }
    void stepList(ScreenHost &host, const std::string &which, int direction) {
        if (!loaded_)
            return;
        std::vector<std::string> values;
        std::string current;
        if (which == "picture") {
            for (const auto &mode : stats_["picture"]["modes"].items())
                values.push_back(mode["value"].string());
            current = pictureValue(host);
        } else if (which == "sound") {
            for (const auto &output : stats_["sound"]["outputs"].items())
                values.push_back(output["id"].string());
            current = soundValue(host);
        } else {
            for (const char *value : kSleep)
                values.push_back(value);
            current = sleepValue(host);
        }
        if (values.empty())
            return;
        const auto found = std::find(values.begin(), values.end(), current);
        const size_t count = values.size();
        size_t target = found == values.end() ? (direction > 0 ? 0 : count - 1)
                        : direction > 0       ? (static_cast<size_t>(found - values.begin()) + 1) % count
                                              : (static_cast<size_t>(found - values.begin()) + count - 1) % count;
        const std::string &value = values[target];
        if (which == "picture")
            host.optimistic().set("quick.picture", Json::text(value), Json::text(stats_["picture"]["mode_raw"].string()),
                                  controlWrite("pictureMode"));
        else if (which == "sound")
            host.optimistic().set("quick.sound", Json::text(value), Json::text(stats_["sound"]["output_raw"].string()),
                                  controlWrite("soundOutput"));
        else
            host.optimistic().set("quick.sleep", Json::text(value), Json::text(stats_["sleepTimer"].string()),
                                  controlWrite("sleepTimer"));
        syncShown(host);
        host.invalidate();
    }
    // Held, the key repeats: each repeat changes the shown value at once and the layer sends the
    // newest target at most every 120 ms.
    void stepVolume(ScreenHost &host, int direction) {
        if (!volumeKnown() || !loaded_)
            return;
        double now = volume(host), target = clamp(now + direction, 0, 100);
        volumeDirection_ = direction;
        if (target == now)
            return;
        host.optimistic().set("quick.volume", Json::numeric(target), Json::numeric(stats_["volume"].number(0)),
                              controlWrite("volume"));
        syncShown(host);
        host.invalidate();
    }
    void stepLight(ScreenHost &host, int amount) {
        if (!lightKnown() || !loaded_)
            return;
        double now = light(host), target = clamp(now + amount, 0, 100);
        lightDirection_ = amount > 0 ? 1 : -1;
        if (target == now)
            return;
        host.optimistic().set("quick.light", Json::numeric(target),
                              Json::numeric(stats_["picture"]["backlight"].number(0)), controlWrite("backlight"));
        syncShown(host);
        host.invalidate();
    }
    void press(ScreenHost &host) {
        if (focus_ >= rows_.size())
            return;
        const Row &row = rows_[focus_];
        if (rowDim(row) && !row.go)
            return;
        if (level_ != Level::Main) {
            choose(host, row);
            return;
        }
        if (!loaded_ && readsStats(row.id))
            return;
        if (row.id == "volume") {
            if (!volumeKnown())
                return;
            host.optimistic().set("quick.mute", Json::flag(!muted(host)), Json::flag(stats_["muted"].boolean()),
                                  controlWrite("mute"));
            syncShown(host);
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
            // The TV acts; the drawer is already gone, and a refusal is said by the layer.
            host.optimistic().run("quick.act", controlWrite(row.id == "saver" ? "screensaver" : "screenOff"));
            close(host, returnRoute(host.cameFrom()));
        } else if (row.id == "recents")
            close(host, "recents");
        else if (row.id == "profile")
            close(host, "profiles");
    }
    void choose(ScreenHost &host, const Row &row) {
        if (level_ == Level::Layout) {
            bool on = !layoutFlag(host, row.id);
            host.optimistic().set("layout." + row.id, Json::flag(on), Json::flag(layoutServer(host, row.id)),
                                  layoutWrite(row.id));
            host.invalidate();
            return;
        }
        if (level_ == Level::Input) {
            if (row.dim)
                return;
            Write open;
            open.operation = Operation::OpenHomeItem;
            const std::string id = row.raw;
            open.body = [id](const Json &) {
                Json body = Json::object();
                body.set("kind", Json::text("dev"));
                body.set("id", Json::text(id));
                return body;
            };
            host.optimistic().run("quick.act", open);
            close(host, returnRoute(host.cameFrom()));
            return;
        }
        if (level_ == Level::Picture)
            host.optimistic().set("quick.picture", Json::text(row.raw),
                                  Json::text(stats_["picture"]["mode_raw"].string()), controlWrite("pictureMode"));
        else if (level_ == Level::Sound)
            host.optimistic().set("quick.sound", Json::text(row.raw),
                                  Json::text(stats_["sound"]["output_raw"].string()), controlWrite("soundOutput"));
        else
            host.optimistic().set("quick.sleep", Json::text(row.raw), Json::text(stats_["sleepTimer"].string()),
                                  controlWrite("sleepTimer"));
        syncShown(host);
        toMain(host);
    }

    Level level_;
    Json stats_ = Json::object();
    std::string nowPort_;
    std::vector<Row> rows_;
    size_t focus_ = 0;
    bool loaded_ = false, readFailed_ = false, needsRebuild_ = false, closing_ = false, closed_ = false;
    double retryAt_ = 0;
    unsigned statsGeneration_ = 0, nowGeneration_ = 0;
    std::string destination_;
    // What is shown, for the snapshot and the held-key counts.
    double shownVolume_ = 0, shownLight_ = 0;
    bool shownMuted_ = false, writing_ = false;
    std::string shownSleep_, shownPicture_;
    int volumeDirection_ = 0, lightDirection_ = 0;
    unsigned volumeBackwards_ = 0, lightBackwards_ = 0;
};
} // namespace

std::unique_ptr<ScreenView> makeQuickScreen(const std::string &route) {
    return route == "quick" ? std::unique_ptr<ScreenView>(new QuickScreen()) : nullptr;
}
} // namespace nativetv
