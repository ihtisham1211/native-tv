// "Who's watching?": the profiles as a row of round avatars, one to add another and a button to
// rename the one in use. Switching and adding post to the profile route the JavaScript page used,
// through the optimistic-update layer: the chosen avatar is the one in use at once, a spinner shows
// until the server answers, and a refusal gives it back and says why.
#include "domain/models.hpp"
#include "domain/say.hpp"
#include "ui/application.hpp"
#include "ui/home_layout.hpp"
#include "ui/kit/overlay.hpp"
#include "ui/kit/tuning.hpp"
#include "ui/optimistic.hpp"
#include "ui/overlay_routes.hpp"
#include "ui/profile_names.hpp"
#include "ui/screen.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace nativetv {
namespace {
using kit::easing;
using say::msg;
constexpr const char *kStats = "profiles:stats", *kGo = "profiles.go", *kStatsMemory = "quick.stats";
constexpr float kAvatarTop = 380, kGap = 48, kMaxAvatar = 180, kMinAvatar = 120;

Color profileColor(const Json &person) {
    std::string value = person["color"].string();
    if (value.size() == 7 && value[0] == '#' &&
        value.find_first_not_of("0123456789abcdefABCDEF", 1) == std::string::npos)
        return kit::rgb(static_cast<unsigned>(std::strtoul(value.c_str() + 1, nullptr, 16)));
    return home::hsl(static_cast<float>(home::hashOf(person["id"].string()) % 360), .5f, .40f);
}
std::string initialOf(const std::string &name) {
    if (name.empty())
        return "?";
    size_t end = 1;
    while (end < name.size() && (static_cast<unsigned char>(name[end]) & 0xc0) == 0x80)
        ++end;
    std::string letter = name.substr(0, end);
    if (letter.size() == 1)
        letter[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(letter[0])));
    return letter;
}

class ProfilesScreen final : public ScreenView {
  public:
    std::string name() const override {
        return "profiles";
    }
    bool hasBar() const override {
        return false;
    }
    void enter(ScreenHost &host) override {
        size_t index = 0;
        std::vector<Json> people = host.view().catalogue["profiles"].items();
        for (size_t i = 0; i < people.size(); ++i)
            if (people[i]["active"].boolean())
                index = i;
        selected_ = index;
        active_ = shownActive(host);
        // The first profile is named after the TV: its model comes from the stats.
        if (!host.memory().count(kStatsMemory))
            statsGeneration_ = host.services().read(kStats, Operation::Metrics);
        Timeline timeline;
        Tween rise(1, 0, kit::motion::ms::base, easing(kit::motion::ease::out));
        for (size_t i = 0; i <= people.size() + 1; ++i) {
            host.motion().set(id("e", i), 1);
            timeline.add(id("e", i),
                         rise.delayedBy(kit::motion::stagger::pageBase +
                                        kit::motion::stagger::pageItem * static_cast<double>(std::min<size_t>(i, 14))));
        }
        timeline.play(host.motion());
        focusMoved(host, true);
    }
    void leave(ScreenHost &host) override {
        host.services().cancel(kStats);
        host.memory().erase(kGo); // a switch still on its way no longer opens Home from here
        host.motion().erasePrefix("profiles:");
    }
    bool update(ScreenHost &host) override {
        // The list can change under the focus (a profile removed from a phone): the focus stays on
        // a profile or on Add, and the rename button goes with the active profile.
        const size_t count = host.view().catalogue["profiles"].size();
        if (selected_ > count || (onButton_ && activeIndex(host) >= count)) {
            selected_ = std::min(selected_, count);
            onButton_ = onButton_ && activeIndex(host) < count;
            focusMoved(host, true);
        }
        busy_ = host.optimistic().pending("profiles.active") || host.optimistic().pending("profiles.add");
        active_ = shownActive(host);
        return busy_;
    }
    bool key(ScreenHost &host, Key key) override {
        size_t count = host.view().catalogue["profiles"].size();
        bool hasActive = activeIndex(host) < count;
        switch (key) {
        case Key::Left:
            if (!onButton_ && selected_)
                --selected_;
            break;
        case Key::Right:
            if (!onButton_ && selected_ < count)
                ++selected_;
            break;
        case Key::Down:
            onButton_ = hasActive;
            break;
        case Key::Up:
            onButton_ = false;
            break;
        case Key::Ok:
            choose(host);
            return true;
        default:
            return false;
        }
        focusMoved(host, false);
        return true;
    }
    bool back(ScreenHost &host) override {
        leaveFor(host);
        return true;
    }
    void click(ScreenHost &host, const std::string &hit) override {
        if (hit == "profiles:back") {
            leaveFor(host);
            return;
        }
        onButton_ = hit == "profiles:rename";
        if (hit == "profiles:add")
            selected_ = host.view().catalogue["profiles"].size();
        else if (hit.compare(0, 11, "profiles:p:") == 0)
            selected_ = static_cast<size_t>(std::atoi(hit.c_str() + 11));
        focusMoved(host, false);
        choose(host);
    }
    void response(ScreenHost &host, const Response &response) override {
        if (response.channel != kStats || response.generation < statsGeneration_)
            return;
        Json data = response.json();
        if (response.ok() && data.valid()) {
            host.memory()[kStatsMemory] = data;
            host.invalidate();
        }
    }
    void build(ScreenHost &host, kit::Paint paint) override {
        const Motion &motion = host.motion();
        Scene &scene = paint.scene;
        std::vector<Json> people = host.view().catalogue["profiles"].items();
        size_t count = people.size();
        const std::string model = modelOf(host), activeId = shownActive(host);
        float diameter = std::max(kMinAvatar, std::min(kMaxAvatar, (kit::canvas::width - 2 * kit::canvas::safeX -
                                                                     static_cast<float>(count) * kGap) /
                                                                        static_cast<float>(count + 1)));
        float total = static_cast<float>(count + 1) * diameter + static_cast<float>(count) * kGap;
        float left = (kit::canvas::width - total) / 2;
        paint.glyphs.text(scene, {0, 170, kit::canvas::width, kit::type::display.line},
                          msg("tv.profiles.title", "Who’s watching?"), kit::type::display,
                          kit::color::onSurface, Node::Align::Center);
        paint.glyphs.text(scene, {0, 258, kit::canvas::width, kit::type::body.line},
                          msg("tv.profiles.lead", "Each profile has its own Home."), kit::type::body,
                          kit::color::onSurface2, Node::Align::Center);
        for (size_t i = 0; i <= count; ++i) {
            bool add = i == count;
            Rect rest = {left + static_cast<float>(i) * (diameter + kGap), kAvatarTop, diameter, diameter};
            float focus = static_cast<float>(motion.value(id("f", i)));
            double hidden = motion.value(id("e", i));
            kit::focus::Look look = kit::focus::lookAt(rest, kit::motion::scale::app, focus);
            scene.pushOpacity(static_cast<float>(1 - hidden) * 1.0f);
            scene.pushTransform(Transform::translate(0, static_cast<float>(28 * hidden)));
            scene.pushTransform(kit::focus::transformAt(rest, look.scale));
            Node &disc = scene.shape(rest, add ? kit::color::surface2 : profileColor(people[i]),
                                     diameter / 2);
            if (!add)
                disc.gradient = Gradient::between(145, profileColor(people[i]),
                                                  kit::focus::mix(profileColor(people[i]), kit::color::ink, .45f));
            else
                disc.ring = {3, 0, kit::alpha::outlineStrong};
            if (look.ringAlpha > 0) {
                disc.ring = {0, look.ringWidth,
                             kit::focus::withAlpha(kit::elevation::focusRingColor, look.ringAlpha)};
                disc.shadow = {0, look.shadowDy, look.shadowBlur, 0, {0, 0, 0, look.shadowAlpha}};
            }
            if (add)
                paint.glyphs.icon(scene, {rest.x + diameter / 2 - 26, rest.y + diameter / 2 - 26, 52, 52},
                                  "add", kit::color::onSurface);
            else
                paint.glyphs.text(scene, {rest.x, rest.y + (diameter - kit::type::display.line) / 2, diameter,
                                          kit::type::display.line},
                                  initialOf(shownProfileName(host.optimistic(), people[i], model)), kit::type::display,
                                  {1, 1, 1, 1}, Node::Align::Center);
            scene.popTransform();
            bool active = !add && people[i]["id"].string() == activeId;
            std::string label = add ? msg("tv.profiles.add", "Add profile")
                                    : shownProfileName(host.optimistic(), people[i], model);
            Color ink = kit::focus::inkAt(kit::color::onSurface2, kit::color::onSurface, focus);
            paint.glyphs.text(scene, {rest.x - kGap / 2, rest.y + diameter + 28, diameter + kGap,
                                      kit::type::title.line},
                              label, kit::type::title, ink, Node::Align::Center);
            if (active)
                paint.glyphs.text(scene, {rest.x - kGap / 2, rest.y + diameter + 28 + kit::type::title.line + 4,
                                          diameter + kGap, kit::type::labelM.line},
                                  msg("tv.profiles.current", "Watching now"), kit::type::labelM,
                                  kit::color::accent, Node::Align::Center);
            // A change on its way: the avatar turns a spinner where the answer is awaited.
            if ((active && host.optimistic().pending("profiles.active")) ||
                (add && host.optimistic().pending("profiles.add")))
                kit::spinner(paint, {rest.x + diameter - 44, rest.y + diameter - 44, 32, 32}, host.nowMs(),
                             kit::color::onSurface);
            scene.popTransform();
            scene.popOpacity();
            scene.hits.push_back({look.rect, add ? "profiles:add" : "profiles:p:" + std::to_string(i)});
        }
        if (activeIndex(host) < count && !host.optimistic().pending("profiles.active")) {
            kit::PillButton rename;
            rename.rest = {kit::canvas::width / 2 - 170, kAvatarTop + diameter + 156, 340, 64};
            rename.id = "profiles:rename";
            rename.label = msg("tv.profiles.rename", "Rename profile");
            rename.icon = "keyboard";
            rename.focus = static_cast<float>(motion.value("profiles:fb"));
            rename.enter = {static_cast<float>(1 - motion.value(id("e", count + 1))),
                            static_cast<float>(28 * motion.value(id("e", count + 1)))};
            kit::pillButton(paint, rename);
        }
        kit::keyHint(paint, kit::canvas::safeX, 1002, "Back", msg("tv.profiles.return", "Return"),
                     "profiles:back");
    }
    Json snapshot() const override {
        Json value = Json::object();
        value.set("selected", Json::numeric(static_cast<double>(selected_)));
        value.set("onButton", Json::flag(onButton_));
        value.set("busy", Json::flag(busy_));
        value.set("active", Json::text(active_));
        return value;
    }

  private:
    static std::string id(const char *name, size_t index) {
        return std::string("profiles:") + name + ":" + std::to_string(index);
    }
    static size_t activeIndex(ScreenHost &host) {
        std::vector<Json> people = host.view().catalogue["profiles"].items();
        for (size_t i = 0; i < people.size(); ++i)
            if (people[i]["active"].boolean())
                return i;
        return people.size();
    }
    void focusMoved(ScreenHost &host, bool jump) {
        size_t count = host.view().catalogue["profiles"].size();
        auto glide = [&](const std::string &name, double to) {
            if (jump)
                host.motion().set(name, to);
            else
                host.motion().tween(name, to, kit::motion::ms::focus, easing(kit::motion::ease::out));
        };
        for (size_t i = 0; i <= count; ++i)
            glide(id("f", i), !onButton_ && i == selected_ ? 1 : 0);
        glide("profiles:fb", onButton_ ? 1 : 0);
        host.invalidate();
    }
    static std::string modelOf(ScreenHost &host) {
        auto found = host.memory().find(kStatsMemory);
        return found == host.memory().end() ? "" : found->second["device"]["model"].string();
    }
    static std::string serverActive(ScreenHost &host) {
        for (const auto &person : host.view().catalogue["profiles"].items())
            if (person["active"].boolean())
                return person["id"].string();
        return "";
    }
    static std::string shownActive(ScreenHost &host) {
        return host.optimistic().text("profiles.active", serverActive(host));
    }
    static Write profileWrite(ScreenHost &host, const std::string &token) {
        Write write;
        write.operation = Operation::SetProfile;
        write.readChannel = "catalogue";
        write.readOperation = Operation::HomeCatalogue;
        host.memory()[kGo] = Json::text(token);
        return write;
    }
    void choose(ScreenHost &host) {
        std::vector<Json> people = host.view().catalogue["profiles"].items();
        if (onButton_) {
            host.open("rename");
            return;
        }
        if (selected_ >= people.size()) {
            Write write = profileWrite(host, "add");
            write.body = [](const Json &) {
                Json body = Json::object();
                body.set("action", Json::text("add"));
                body.set("switch", Json::flag(true));
                return body;
            };
            // A new profile has its own layout: Home starts again from its first tab.
            write.done = [&host](bool ok, const Json &) {
                if (!ok)
                    return;
                host.toast(msg("tv.profiles.added", "Profile added"));
                if (host.memory()[kGo].string() == "add")
                    host.open("home");
            };
            host.optimistic().run("profiles.add", write);
        } else if (people[selected_]["id"].string() == shownActive(host)) {
            leaveFor(host);
            return;
        } else {
            const std::string chosen = people[selected_]["id"].string();
            Write write = profileWrite(host, chosen);
            write.body = [](const Json &target) {
                Json body = Json::object();
                body.set("action", Json::text("switch"));
                body.set("id", target);
                return body;
            };
            write.done = [&host, chosen](bool ok, const Json &) {
                if (ok && host.memory()[kGo].string() == chosen)
                    host.open("home");
            };
            host.optimistic().set("profiles.active", Json::text(chosen), Json::text(serverActive(host)), write);
        }
        busy_ = host.optimistic().pending("profiles.active") || host.optimistic().pending("profiles.add");
        active_ = shownActive(host);
        host.invalidate();
    }
    void leaveFor(ScreenHost &host) {
        host.open(returnRoute(host.cameFrom()));
    }

    size_t selected_ = 0;
    bool onButton_ = false, busy_ = false;
    std::string active_; // the profile in use as shown, for the snapshot
    unsigned statsGeneration_ = 0;
};
} // namespace

std::unique_ptr<ScreenView> makeProfilesScreen(const std::string &route) {
    return route == "profiles" ? std::unique_ptr<ScreenView>(new ProfilesScreen()) : nullptr;
}
} // namespace nativetv
