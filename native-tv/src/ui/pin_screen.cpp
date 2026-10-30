// The admin PIN prompt: the four digits on the keypad of the setup pages. Done posts them to
// /api/admin/unlock; the session that comes back goes to the services, which send it with every
// request from then on. The action the server refused is not repeated: the screen says it is
// unlocked, goes back to where it came from and the person does the action again.
#include "domain/say.hpp"
#include "domain/setup_pages.hpp"
#include "ui/application.hpp"
#include "ui/keyboard_pad.hpp"
#include "ui/kit/entry.hpp"
#include "ui/kit/tuning.hpp"
#include "ui/overlay_routes.hpp"
#include "ui/pin_screen.hpp"
#include <algorithm>

namespace nativetv {
namespace {
using kit::Enter;
using say::msg;
namespace ms = kit::motion::ms;

// What each part of the page is called in the entrance: its Motion id is "pin:e:<n>".
enum Item { Label = 0, Title, Lead, Field, Keys, Hints, Items };
constexpr const char *kChannel = "pin:unlock";
constexpr float kLeft = 80, kRight = 944;
constexpr Rect kHints = {80, 998, 0, 30};

class PinScreen final : public ScreenView {
  public:
    std::string name() const override {
        return "pin";
    }
    bool hasBar() const override {
        return false;
    }
    void enter(ScreenHost &host) override {
        KeyboardPad::Options options;
        options.id = "pin:kb";
        options.maximum = kAdminPinLength;
        options.digits = true;
        pad_.begin(host.motion(), "", options);
        pad_.setDone(msg("tv.pin.unlock", "Unlock"), "check");
        // The parts rise in one after another, as the setup pages' do (spec 5.6).
        for (int item = 0; item < Items; ++item) {
            const std::string id = "pin:e:" + std::to_string(item);
            host.motion().set(id, 1);
            host.motion().tween(id, 0, ms::base, Easing::out(),
                                kit::motion::stagger::pageBase + kit::motion::stagger::pageItem * item);
        }
    }
    void leave(ScreenHost &host) override {
        host.services().cancel(kChannel);
        host.motion().erasePrefix("pin:");
    }
    bool key(ScreenHost &host, Key key) override {
        if (!waiting_) {
            const KeyboardPad::Result result = pad_.key(host.motion(), host.nowMs(), key);
            if (result == KeyboardPad::Result::Done)
                unlock(host);
            else if (result == KeyboardPad::Result::Edited)
                message_.clear();
        }
        host.invalidate();
        return true;
    }
    // Back cancels: nothing was unlocked and the refused action stays undone.
    bool back(ScreenHost &host) override {
        host.open(returnRoute(host.cameFrom()));
        return true;
    }
    void click(ScreenHost &host, const std::string &hit) override {
        if (!waiting_ && pad_.click(host.motion(), host.nowMs(), hit) == KeyboardPad::Result::Done)
            unlock(host);
        host.invalidate();
    }
    bool text(ScreenHost &host, const std::string &text) override {
        if (waiting_)
            return false;
        if (pad_.type(host.nowMs(), text) == KeyboardPad::Result::Edited)
            message_.clear();
        host.invalidate();
        return true;
    }
    void response(ScreenHost &host, const Response &reply) override;
    bool update(ScreenHost &host) override {
        const bool lit = pad_.caretLit(host.nowMs());
        if (lit == caretLit_)
            return false;
        caretLit_ = lit;
        return true;
    }
    void build(ScreenHost &host, kit::Paint paint) override;
    Json snapshot() const override {
        Json value = Json::object();
        value.set("digits", Json::numeric(static_cast<double>(pad_.value().size())));
        value.set("message", Json::text(message_));
        value.set("waiting", Json::flag(waiting_));
        value.set("focusedKey", Json::text(pad_.focusedKey()));
        return value;
    }

  private:
    void unlock(ScreenHost &host);
    void refused(ScreenHost &host, const std::string &text) {
        message_ = text;
        pad_.setValue("", host.nowMs());
    }
    Enter enterOf(ScreenHost &host, int item) const {
        const double hidden = host.motion().value("pin:e:" + std::to_string(item));
        return {static_cast<float>(1 - hidden), static_cast<float>(28 * hidden)};
    }

    KeyboardPad pad_;
    std::string message_;
    bool waiting_ = false, caretLit_ = true;
};

void PinScreen::unlock(ScreenHost &host) {
    if (pad_.value().size() != kAdminPinLength) {
        message_ = pinWords().incomplete;
        return;
    }
    Json body = Json::object();
    body.set("pin", Json::text(pad_.value()));
    message_.clear();
    waiting_ = host.services().write(kChannel, Operation::AdminUnlock, body) != 0;
    if (!waiting_)
        message_ = msg("tv.pin.busy", "Too many pending requests. Try again.");
}
void PinScreen::response(ScreenHost &host, const Response &reply) {
    if (reply.channel != kChannel)
        return;
    waiting_ = false;
    const Json data = reply.json();
    if (reply.ok() && data["ok"].boolean()) {
        host.services().adminSession(data["session"].string());
        host.toast(msg("tv.pin.unlocked", "Unlocked. Do that again."));
        host.open(returnRoute(host.cameFrom())); // this screen is gone from here on
        return;
    }
    if (reply.status == 409) {
        // The PIN was removed (in the dashboard) since the action was refused: nothing is locked.
        host.toast(msg("tv.pin.none", "No admin PIN is set. Do that again."));
        host.open(returnRoute(host.cameFrom()));
        return;
    }
    const long wait = static_cast<long>(data["retryAfter"].number());
    if (reply.status == 429 || (reply.status == 403 && wait > 0)) {
        Json values = Json::object();
        values.set("n", Json::text(std::to_string(std::max<long>(wait, 1))));
        refused(host, msg("tv.pin.locked", "Try again in {n} s", values));
    } else if (reply.status == 403)
        refused(host, msg("tv.pin.wrong", "Wrong PIN. Try again."));
    else
        refused(host, msg("tv.pin.failed", "The TV did not answer. Try again."));
    host.invalidate();
}
void PinScreen::build(ScreenHost &host, kit::Paint paint) {
    Scene &scene = paint.scene;
    {
        kit::Layer layer(scene, enterOf(host, Label));
        scene.shape({kLeft, 176 + 9, 10, 10}, kit::color::accent, 5);
        paint.glyphs.text(scene, {kLeft + 22, 176, 738, 28}, pinWords().label, kit::type::labelL,
                          kit::color::accent);
    }
    {
        kit::Layer layer(scene, enterOf(host, Title));
        paint.glyphs.text(scene, kit::metrics::setup::title, msg("tv.pin.title", "Enter the admin PIN"),
                          kit::type::setupTitle, kit::color::onSurface, Node::Align::Left, 2);
    }
    {
        kit::Layer layer(scene, enterOf(host, Lead));
        paint.glyphs.text(scene, {kLeft, 382, 700, kit::type::body.line * 8},
                          msg("tv.pin.lead",
                              "That needs the four-digit admin PIN. Once entered, it stays unlocked "
                              "until ten minutes pass without use."),
                          kit::type::body, kit::color::onSurface2, Node::Align::Left, 8);
    }
    {
        kit::Layer layer(scene, enterOf(host, Field));
        const bool failing = !message_.empty();
        paint.glyphs.text(scene, {kRight, 176, 896, kit::type::title.line},
                          failing ? message_ : msg("tv.pin.field", "Four digits"), kit::type::title,
                          failing ? kit::color::warn : kit::color::onSurface2);
        kit::textField(paint, pad_.field(host.nowMs(), kit::metrics::setup::field,
                                         pinWords().placeholder, true));
    }
    pad_.paint(paint, host.motion(), enterOf(host, Keys));
    kit::hintBar(paint, kHints,
                 {{"", msg("setup.hint.move", "Move")},
                  {"OK", msg("setup.hint.type", "Type")},
                  {"Back", msg("tv.pin.cancel", "Cancel")}},
                 enterOf(host, Hints));
}
} // namespace

std::unique_ptr<ScreenView> makePinScreen(const std::string &route) {
    return route == "pin" ? std::unique_ptr<ScreenView>(new PinScreen()) : nullptr;
}
} // namespace nativetv
