// The TV theme: the picker that Settings opens from its TV theme row, the sheet that follows a
// change (keep it, or it goes back by itself), and the one-time suggestion of OLED Care that an OLED
// panel on the default theme is offered. A change shows at once and is saved through the optimistic
// layer, so a refused save puts the old theme back.
#include "core/themes.hpp"
#include "domain/say.hpp"
#include "ui/application.hpp"
#include "ui/kit/entry.hpp"
#include "ui/overlay_routes.hpp"
#include "ui/theme_screen.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace nativetv {
namespace {
using say::msg;
namespace ms = kit::motion::ms;

constexpr double kKeepMs = 10000; // a theme nobody confirms goes back after this
constexpr float kCardX = 80, kCardPitch = 600, kCardW = 560, kCardH = 315, kCardY = 314;
constexpr Rect kSheet = {80, 840, 1760, 148};
constexpr Rect kHints = {80, 998, 0, 30};
constexpr const char *kOfferedTheme = "n-oled-care";
constexpr Color kClear = {0, 0, 0, 0};

enum class Sheet { None, Keep, Suggest };

std::string cardId(int index) {
    return "tvtheme:c:" + std::to_string(index);
}
std::string buttonId(int index) {
    return "tvtheme:b:" + std::to_string(index);
}
std::string nameOf(const std::string &id) {
    const ThemeChoice *choice = findTheme(id);
    return choice ? choice->name : id;
}
const kit::Palette *paletteOf(const std::string &id) {
    for (const kit::ThemeInfo &theme : kit::themes)
        if (id == theme.id)
            return theme.palette;
    return kit::themes[0].palette;
}

// What a theme looks like, drawn from its own colours and look values: a top bar, the hero's lines,
// a row of apps (the first focused) and a row of tiles.
void preview(kit::Paint paint, Rect box, const kit::Palette &p) {
    Scene &scene = paint.scene;
    auto dimmed = [&](Color colour) {
        return kit::focus::mix(Color{0, 0, 0, 1}, colour, p.restBrightness);
    };
    scene.shape(box, p.ink, 16);
    if (p.wall > .002f) {
        Node &wash = scene.shape(box, kClear, 16);
        wash.gradient = Gradient::between(45, kit::focus::withAlpha(p.wash, p.wall),
                                          kit::focus::withAlpha(p.wash, 0));
    }
    const float x = box.x, y = box.y;
    scene.shape({x + 24, y + 22, 28, 28}, p.navButton, 14);
    const float tabWidth[] = {44, 36, 44};
    float tabX = x + 72;
    for (int i = 0; i < 3; ++i) {
        if (i == 0) {
            if (p.ringFocus)
                scene.shape({tabX, y + 46, tabWidth[i], 3}, p.onSurface, 1.5f);
            else
                scene.shape({tabX - 10, y + 20, tabWidth[i] + 20, 30}, p.focusFill, 15);
        }
        scene.shape({tabX, y + 31, tabWidth[i], 8}, i == 0 ? (p.ringFocus ? p.onSurface : p.onFocus)
                                                              : p.onSurface2,
                    4);
        tabX += tabWidth[i] + 28;
    }
    const Color apps[] = {kit::rgb(0xC0392B), kit::rgb(0x12805C), kit::rgb(0x6C3483),
                          kit::rgb(0x1F618D), kit::rgb(0xB9651B)};
    auto circles = [&](float top) {
        for (int i = 0; i < 5; ++i) {
            const Rect circle = {x + 24 + 44.0f * i, y + top, 32, 32};
            scene.shape(circle, i == 0 ? apps[i] : dimmed(apps[i]), 16);
            if (i == 0)
                scene.shape(circle, kClear, 16).ring = {3, 0, p.focusRingColor};
        }
    };
    if (!p.discover) {
        // A Home with no feed: the apps, the inputs and the panel's cards.
        circles(78);
        for (int i = 0; i < 4; ++i) {
            const Rect tile = {x + 24 + 76.0f * i, y + 132, 66, 42};
            scene.shape(tile, p.surface1, 8);
            scene.shape({tile.x + 8, tile.y + 24, 38, 7}, p.onSurface2, 3.5f);
        }
        for (int i = 0; i < 2; ++i) {
            const Rect card = {x + 24 + 150.0f * i, y + 196, 138, 78};
            scene.shape(card, p.surface1, 10);
            scene.shape({card.x + 12, card.y + 12, 52, 7}, p.onSurface2, 3.5f);
            scene.shape({card.x + 12, card.y + 28, 70, 18}, p.onSurface, 5);
            scene.shape({card.x + 12, card.y + card.h - 14, card.w - 24, 4}, p.surface3, 2);
            scene.shape({card.x + 12, card.y + card.h - 14, (card.w - 24) * (i ? .25f : .62f), 4}, p.accent, 2);
        }
        return;
    }
    scene.shape({x + 24, y + 86, 190, 16}, p.onSurface, 8);
    scene.shape({x + 24, y + 112, 250, 8}, p.onSurface2, 4);
    scene.shape({x + 24, y + 128, 210, 8}, p.onSurface2, 4);
    circles(160);
    for (int i = 0; i < 3; ++i) {
        const Rect tile = {x + 24 + 112.0f * i, y + 212, 100, 56};
        Node &face = scene.shape(tile, p.pictures ? p.surface2 : p.surface1, 8);
        if (p.pictures)
            face.gradient = Gradient::between(160, kit::rgb(0x2A6F7F), kit::rgb(0x1C2540));
        scene.shape({tile.x + 8, tile.y + 38, 56, 8}, p.pictures ? kit::rgb(0xFFFFFF) : p.onSurface2, 4);
    }
}

class ThemeScreen final : public ScreenView {
  public:
    explicit ThemeScreen(bool suggestion)
        : sheet_(suggestion ? Sheet::Suggest : Sheet::None), suggestion_(suggestion) {}
    std::string name() const override {
        return suggestion_ ? "tvtheme:suggest" : "tvtheme";
    }
    bool hasBar() const override {
        return false;
    }
    void enter(ScreenHost &host) override {
        const std::string now = kit::themeId();
        for (int i = 0; i < kThemeChoiceCount; ++i)
            if (now == kThemeChoices[i].id)
                focus_ = i;
        for (int i = 0; i < kThemeChoiceCount; ++i)
            host.motion().set(cardId(i), i == focus_ ? 1 : 0);
        host.motion().set(buttonId(0), 1);
        host.motion().set(buttonId(1), 0);
    }
    void leave(ScreenHost &host) override {
        // A theme nobody confirmed does not stay, and an offer not answered counts as no thanks.
        if (sheet_ == Sheet::Keep)
            revert(host);
        else if (sheet_ == Sheet::Suggest)
            decline(host);
        host.motion().erasePrefix("tvtheme:");
    }
    bool key(ScreenHost &host, Key key) override {
        if (sheet_ != Sheet::None)
            return sheetKey(host, key);
        if (key == Key::Left || key == Key::Right) {
            const int next = std::max(0, std::min(kThemeChoiceCount - 1, focus_ + (key == Key::Left ? -1 : 1)));
            if (next != focus_) {
                host.motion().tween(cardId(focus_), 0, ms::focus, Easing::out());
                focus_ = next;
                host.motion().tween(cardId(focus_), 1, ms::focus, Easing::out());
            }
            host.invalidate();
            return true;
        }
        if (key == Key::Ok) {
            choose(host, focus_);
            return true;
        }
        return false;
    }
    bool back(ScreenHost &host) override {
        if (sheet_ == Sheet::Keep) {
            revert(host);
            if (suggestion_)
                host.open(returnRoute(host.cameFrom()));
        } else if (sheet_ == Sheet::Suggest) {
            decline(host);
            host.open(returnRoute(host.cameFrom()));
        } else
            host.open(returnRoute(host.cameFrom()));
        return true;
    }
    void click(ScreenHost &host, const std::string &hit) override {
        if (hit.compare(0, 10, "tvtheme:c:") == 0 && sheet_ == Sheet::None) {
            focus_ = std::atoi(hit.c_str() + 10);
            choose(host, focus_);
        } else if (hit.compare(0, 10, "tvtheme:b:") == 0 && sheet_ != Sheet::None) {
            sheetFocus_ = std::atoi(hit.c_str() + 10);
            press(host);
        }
    }
    bool update(ScreenHost &host) override {
        if (sheet_ != Sheet::Keep)
            return false;
        if (host.nowMs() - sheetAt_ >= kKeepMs) {
            revert(host);
            if (suggestion_)
                host.open(returnRoute(host.cameFrom()));
            return true;
        }
        const int left = secondsLeft(host);
        if (left == shownLeft_)
            return false;
        shownLeft_ = left;
        return true;
    }
    void build(ScreenHost &host, kit::Paint paint) override;
    Json snapshot() const override {
        Json value = Json::object();
        value.set("focus", Json::numeric(focus_));
        value.set("sheet", Json::text(sheet_ == Sheet::Keep ? "keep" : sheet_ == Sheet::Suggest ? "suggest" : ""));
        value.set("previous", Json::text(previous_));
        return value;
    }

  private:
    int secondsLeft(ScreenHost &host) const {
        return std::max(0, static_cast<int>(std::ceil((kKeepMs - (host.nowMs() - sheetAt_)) / 1000)));
    }
    // Shows `to` now and saves it; the old one comes back if the save is refused.
    static void change(ScreenHost &host, const std::string &to, const std::string &from) {
        kit::useTheme(to);
        Write write;
        write.operation = Operation::Control;
        write.body = [](const Json &target) {
            return fields({{"action", Json::text("setTvTheme")}, {"value", target}});
        };
        write.failure = msg("tv.theme.failed", "The theme could not be saved");
        write.done = [from](bool ok, const Json &) {
            if (!ok)
                kit::useTheme(from);
        };
        host.optimistic().set("tvTheme", Json::text(to), Json::text(from), write);
        host.optimistic().commit("tvTheme");
        host.invalidate();
    }
    void choose(ScreenHost &host, int index) {
        if (index < 0 || index >= kThemeChoiceCount)
            return;
        const std::string id = kThemeChoices[index].id;
        if (id == kit::themeId())
            return;
        previous_ = kit::themeId();
        change(host, id, previous_);
        openKeep(host);
    }
    void openKeep(ScreenHost &host) {
        sheet_ = Sheet::Keep;
        sheetAt_ = host.nowMs();
        shownLeft_ = secondsLeft(host);
        sheetFocus_ = 0;
        host.motion().set(buttonId(0), 1);
        host.motion().set(buttonId(1), 0);
        host.invalidate();
    }
    void revert(ScreenHost &host) {
        sheet_ = Sheet::None;
        change(host, previous_, kit::themeId());
    }
    // "No thanks" to the offer: the server stops offering it.
    void decline(ScreenHost &host) {
        sheet_ = Sheet::None;
        Write write;
        write.operation = Operation::Control;
        write.body = [](const Json &) {
            return fields({{"action", Json::text("dismissTvThemeSuggestion")}});
        };
        host.optimistic().run("tvTheme:dismiss", write);
    }
    bool sheetKey(ScreenHost &host, Key key) {
        if (key == Key::Left || key == Key::Right) {
            const int next = key == Key::Left ? 0 : 1;
            if (next != sheetFocus_) {
                host.motion().tween(buttonId(sheetFocus_), 0, ms::focus, Easing::out());
                sheetFocus_ = next;
                host.motion().tween(buttonId(sheetFocus_), 1, ms::focus, Easing::out());
            }
            host.invalidate();
        } else if (key == Key::Ok)
            press(host);
        else if (key == Key::Back)
            return false;
        return key != Key::Back;
    }
    // The sheet's focused button: Keep or Use it (0), Go back or Not now (1).
    void press(ScreenHost &host) {
        if (sheet_ == Sheet::Suggest) {
            if (sheetFocus_ == 0) {
                previous_ = kit::themeId();
                change(host, kOfferedTheme, previous_);
                openKeep(host);
            } else {
                decline(host);
                host.open(returnRoute(host.cameFrom()));
            }
        } else if (sheetFocus_ == 0) {
            sheet_ = Sheet::None; // kept
            if (suggestion_)
                host.open(returnRoute(host.cameFrom()));
        } else {
            revert(host);
            if (suggestion_)
                host.open(returnRoute(host.cameFrom()));
        }
        host.invalidate();
    }
    void button(ScreenHost &host, kit::Paint paint, Rect box, const std::string &label, int index) const {
        Scene &scene = paint.scene;
        const float focus = static_cast<float>(host.motion().value(buttonId(index)));
        scene.shape(box, kit::focus::mix(kit::color::surface3, kit::color::focusFill, focus), 16);
        kit::focus::ringOnFocus(scene, box, 16, focus);
        paint.glyphs.text(scene, {box.x, box.y + (box.h - kit::type::title.line) / 2, box.w, kit::type::title.line},
                          label, kit::type::title, kit::focus::inkAt(kit::color::onSurface, kit::color::onFocus, focus),
                          Node::Align::Center);
        scene.hits.push_back({box, buttonId(index)});
    }

    Sheet sheet_;
    bool suggestion_;
    int focus_ = 0, sheetFocus_ = 0, shownLeft_ = -1;
    double sheetAt_ = 0;
    std::string previous_;
};

void ThemeScreen::build(ScreenHost &host, kit::Paint paint) {
    Scene &scene = paint.scene;
    if (suggestion_) {
        scene.shape({80, 176 + 9, 10, 10}, kit::color::accent, 5);
        paint.glyphs.text(scene, {102, 176, 738, 28}, msg("tv.theme.offer.label", "OLED panel"),
                          kit::type::labelL, kit::color::accent);
        paint.glyphs.text(scene, kit::metrics::setup::title, msg("tv.theme.offer.title", "Use OLED Care?"),
                          kit::type::setupTitle, kit::color::onSurface, Node::Align::Left, 2);
        paint.glyphs.text(scene, {80, 330, 900, kit::type::body.line * 6},
                          msg("tv.theme.offer.body",
                              "This TV has an OLED panel. OLED Care keeps the screens black and dim, "
                              "and they move a little, so they wear the panel less. Home shows your "
                              "apps, inputs and panel, without film picks. Settings, TV theme "
                              "changes it back."),
                          kit::type::body, kit::color::onSurface2, Node::Align::Left, 6);
        if (sheet_ == Sheet::Suggest) {
            button(host, paint, {80, 560, 280, 84}, msg("tv.theme.offer.use", "Use it"), 0);
            button(host, paint, {392, 560, 280, 84}, msg("tv.theme.offer.no", "Not now"), 1);
        }
    } else {
        paint.glyphs.text(scene, {80, 159, 1760, kit::type::headline.line}, msg("tv.theme.title", "TV theme"),
                          kit::type::headline, kit::color::onSurface);
        paint.glyphs.text(scene, {80, 232, 1760, kit::type::body.line},
                          msg("tv.theme.lead", "Changes how the TV screens look and what Home shows."),
                          kit::type::body, kit::color::onSurface2);
        for (int i = 0; i < kThemeChoiceCount; ++i) {
            const ThemeChoice &choice = kThemeChoices[i];
            const float x = kCardX + kCardPitch * i, focus = static_cast<float>(host.motion().value(cardId(i)));
            const Rect card = {x, kCardY, kCardW, kCardH};
            preview(paint, card, *paletteOf(choice.id));
            if (sheet_ == Sheet::None && focus > .004f)
                scene.shape(card, kClear, 16).ring = {3, 0, kit::focus::withAlpha(kit::elevation::focusRingColor, focus)};
            paint.glyphs.text(scene, {x, 652, kCardW - 170, kit::type::title.line}, choice.name, kit::type::title,
                              focus > .5f ? kit::color::onSurface : kit::color::onSurface2);
            if (kit::themeId() == std::string(choice.id)) {
                paint.glyphs.icon(scene, {x + kCardW - 150, 660, 28, 28}, "check", kit::color::accent);
                paint.glyphs.text(scene, {x + kCardW - 114, 658, 114, kit::type::labelM.line},
                                  msg("tv.theme.inUse", "In use"), kit::type::labelM, kit::color::onSurface2);
            }
            paint.glyphs.text(scene, {x, 708, kCardW - 20, kit::type::labelM.line * 3}, choice.summary,
                              kit::type::labelM, kit::color::onSurface2, Node::Align::Left, 3);
            scene.hits.push_back({card, cardId(i)});
        }
    }
    if (sheet_ == Sheet::Keep) {
        scene.shape(kSheet, kit::color::surface2, 28);
        Json values = Json::object();
        values.set("name", Json::text(nameOf(kit::themeId())));
        paint.glyphs.text(scene, {128, 868, 1100, kit::type::title.line},
                          msg("tv.theme.keep", "Keep {name}?", values), kit::type::title, kit::color::onSurface);
        Json back = Json::object();
        back.set("name", Json::text(nameOf(previous_)));
        back.set("n", Json::text(std::to_string(std::max(0, secondsLeft(host)))));
        paint.glyphs.text(scene, {128, 920, 1100, kit::type::body.line},
                          msg("tv.theme.keepNote", "It goes back to {name} in {n} seconds if neither button is pressed.", back),
                          kit::type::body, kit::color::onSurface2);
        button(host, paint, {1296, 872, 240, 84}, msg("tv.theme.keepYes", "Keep"), 0);
        button(host, paint, {1568, 872, 240, 84}, msg("tv.theme.keepNo", "Go back"), 1);
    }
    kit::hintBar(paint, kHints,
                 {{"", msg("setup.hint.move", "Move")},
                  {"OK", sheet_ == Sheet::None ? msg("tv.theme.hint.use", "Use") : msg("tv.theme.hint.choose", "Choose")},
                  {"Back", sheet_ == Sheet::Keep ? msg("tv.theme.hint.back", "Go back")
                           : sheet_ == Sheet::Suggest ? msg("tv.theme.hint.later", "Not now")
                                                       : msg("tv.theme.hint.return", "Return")}});
}
} // namespace

std::unique_ptr<ScreenView> makeThemeScreen(const std::string &route) {
    if (route == "tvtheme")
        return std::unique_ptr<ScreenView>(new ThemeScreen(false));
    if (route == "tvtheme:suggest")
        return std::unique_ptr<ScreenView>(new ThemeScreen(true));
    return nullptr;
}
} // namespace nativetv
