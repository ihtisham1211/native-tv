// First-run setup on the TV: the steps and the server writes of pages/setup/setup.js, drawn on the
// kit. Each step is a page; the pages share the left column (step, title, lead) and put the
// choices, or on the handoff page the keyboard, in the right one (spec 3.6).
#include "ui/setup_screen.hpp"
#include "domain/say.hpp"
#include "domain/setup_pages.hpp"
#include "ui/keyboard_pad.hpp"
#include "ui/optimistic.hpp"
#include "ui/kit/entry.hpp"
#include "ui/kit/tuning.hpp"
#include "ui/splash.hpp"
#include <algorithm>
#include <cmath>

namespace nativetv {
namespace {
namespace ms = kit::motion::ms;
using kit::Enter;

// What each item of a page is called in the entrance: its Motion id is "setup:e:<n>".
enum Item { Label = 0, Title, Lead, Action, Content, Field = 9, Keys, Hints, Dots, Items };
constexpr int kPollMs = 2000, kAdvanceMs = 1500, kRestartWaitMs = 1500, kRetryMs = 1000,
              kRetries = 60;
constexpr float kLeft = 80, kRight = 944, kRowsTop = 228, kRowPitch = 100;
constexpr Rect kDots = {80, 934, 0, 8}, kHints = {80, 998, 0, 30};
constexpr Rect kAction = {80, 530, 300, 74};
constexpr Rect kMark = {1264, 300, 256, 256};

std::string id(const char *kind, size_t index) {
    return std::string("setup:") + kind + ":" + std::to_string(index);
}
// "http://192.168.1.42:3000/?c=ab12" -> "192.168.1.42:3000"
std::string addressOf(std::string url) {
    for (const char *scheme : {"https://", "http://"})
        if (url.compare(0, std::string(scheme).size(), scheme) == 0)
            url.erase(0, std::string(scheme).size());
    size_t end = url.find("/?c=");
    return end == std::string::npos ? url : url.substr(0, end);
}
// A write the page waits for (the server restarts on it, or its answer is what comes next): it
// runs through the layer, which sends it, times it out and says so when it fails; the answer is
// left in memory for update() to take.
Write waitedWrite(ScreenHost &host, Operation operation, const Json &body, const std::string &name) {
    ScreenHost *application = &host;
    Write write;
    write.operation = operation;
    write.body = [body](const Json &) { return body; };
    write.done = [application, name](bool ok, const Json &reply) {
        Json result = Json::object();
        result.set("ok", Json::flag(ok));
        if (reply.valid())
            result.set("reply", reply);
        application->memory()["setup.reply." + name] = result;
        application->invalidate();
    };
    return write;
}
bool takeReply(ScreenHost &host, const std::string &name, bool &ok, Json &reply) {
    auto found = host.memory().find("setup.reply." + name);
    if (found == host.memory().end())
        return false;
    ok = found->second["ok"].boolean();
    reply = found->second["reply"];
    host.memory().erase(found);
    return true;
}
std::string trimmed(const std::string &value) {
    size_t first = value.find_first_not_of(" \t"), last = value.find_last_not_of(" \t");
    return first == std::string::npos ? "" : value.substr(first, last - first + 1);
}

class SetupScreen final : public ScreenView {
  public:
    explicit SetupScreen(SetupStep entry)
        : step_(entry), entry_(entry), fromSettings_(entry == SetupStep::Handoff) {}
    std::string name() const override {
        return std::string("setup:") + setupStepName(step_);
    }
    bool hasBar() const override {
        return false;
    }
    // First-run setup has its own PIN step; the Settings entry (the phone link) has none.
    bool takesAdminPin() const override {
        return fromSettings_;
    }
    void enter(ScreenHost &host) override {
        // Answers a page before this one left behind are not this page's.
        for (auto it = host.memory().begin(); it != host.memory().end();)
            it = it->first.compare(0, 12, "setup.reply.") == 0 ? host.memory().erase(it) : std::next(it);
        host.motion().set("setup:pg", 0);
        waitForServer(host, After::Start, kRetries, 0);
    }
    void leave(ScreenHost &host) override {
        stopLink(host);
        host.motion().erasePrefix("setup:");
    }
    bool key(ScreenHost &host, Key key) override;
    bool back(ScreenHost &host) override;
    void click(ScreenHost &host, const std::string &hit) override;
    void response(ScreenHost &host, const Response &response) override;
    bool update(ScreenHost &host) override;
    bool text(ScreenHost &host, const std::string &text) override;
    void build(ScreenHost &host, kit::Paint paint) override;
    Json snapshot() const override;

  private:
    enum class After { Start, Network, Retry };
    // The phone link: being made, showing its code, refused or expired, or the TV's own broker
    // details on their way to the server.
    enum class Link { Preparing, Ready, Failed, Sending };

    // ---- Flow --------------------------------------------------------------------------------
    void waitForServer(ScreenHost &host, After after, int tries, int delayMs);
    void serverAnswered(ScreenHost &host);
    void showStep(ScreenHost &host, SetupStep step);   // the page is on screen from here
    void goTo(ScreenHost &host, SetupStep step);       // the old page leaves first
    void afterHomeAssistant(ScreenHost &host) {
        goTo(host, setupHasStandby(state_) ? SetupStep::Standby : SetupStep::Summary);
    }
    // Where Back from the network question, and Done on the name, lead: the PIN when it is asked.
    SetupStep afterName() const {
        return askPin_ ? SetupStep::Pin : SetupStep::Network;
    }
    void activate(ScreenHost &host);
    void choosePin(ScreenHost &host);
    void acceptPin(ScreenHost &host);
    void writePin(ScreenHost &host, const std::string &pin);
    void chooseNetwork(ScreenHost &host);
    void chooseStandby(ScreenHost &host);
    void openDashboard(ScreenHost &host);
    void select(ScreenHost &host, size_t index);
    bool demo() const {
        return state_["demo"].boolean();
    }
    Json withDemo(Json body) const {
        if (demo())
            body.set("demo", Json::flag(true));
        return body;
    }
    // ---- Phone link and broker ---------------------------------------------------------------
    void startLink(ScreenHost &host);
    void stopLink(ScreenHost &host);
    void linkFailed(ScreenHost &host, const std::string &reason);
    void polled(ScreenHost &host, const Response &response);
    void connected(ScreenHost &host);
    void beginField(ScreenHost &host);
    void fieldDone(ScreenHost &host);
    void beginName(ScreenHost &host);
    void beginPin(ScreenHost &host);
    void acceptName(ScreenHost &host);
    void answered(ScreenHost &host, const std::string &name, bool ok, const Json &data);
    // What the server holds, what the page shows: the optimistic layer's view of the choices.
    bool serverStandby(ScreenHost &host) const;
    std::string serverName(ScreenHost &host) const;
    Json shownState(ScreenHost &host);
    std::string pageKey(ScreenHost &host);
    std::string shownName(ScreenHost &host);
    void drawEntry(ScreenHost &host, kit::Paint paint);
    void submitBroker(ScreenHost &host);
    void brokerFailed(ScreenHost &host, const std::string &error);
    // ---- Drawing -----------------------------------------------------------------------------
    Enter enterOf(ScreenHost &host, int item) const;
    bool onChoices() const {
        return !choices().empty();
    }
    const std::vector<SetupChoice> &choices() const {
        return step_ == SetupStep::Handoff ? linkChoices_ : page_.choices;
    }
    bool hasAction() const {
        return !page_.action.empty() || step_ == SetupStep::Trouble;
    }
    // The keyboard is on screen: the name's and the PIN's pages, and the handoff's while it takes
    // typing.
    bool usesKeys() const {
        return step_ == SetupStep::Name || step_ == SetupStep::PinOwn ||
               (step_ == SetupStep::Handoff && (link_ == Link::Preparing || link_ == Link::Ready));
    }
    // The page is waiting on the server (a write that restarts it, the broker being saved): it says
    // so with a breathing dot and "Saving…", the moment the key was pressed. The PIN's own write
    // is quick and its pages' lead is long: those only hold the keys.
    bool waitsOnServer() const {
        const bool pinPage = step_ == SetupStep::Pin || step_ == SetupStep::PinOwn;
        return (busy_ && !pinPage) || (step_ == SetupStep::Handoff && link_ == Link::Sending && !joined_);
    }
    bool showsCode() const {
        return step_ == SetupStep::Handoff && link_ == Link::Ready && !url_.empty();
    }
    size_t focusableCount() const;
    std::string leadText() const;
    void drawLeft(ScreenHost &host, kit::Paint paint);
    void drawChoices(ScreenHost &host, kit::Paint paint);
    void drawWaiting(ScreenHost &host, kit::Paint paint);
    void drawHandoff(ScreenHost &host, kit::Paint paint);
    void drawFooter(ScreenHost &host, kit::Paint paint);

    SetupStep step_, entry_;
    int next_ = -1;          // the step to show once the page has left
    bool fromSettings_ = false, loaded_ = false, busy_ = false, netChosen_ = false;
    // The PIN: asked on this run (the TV had none when setup opened, so the step counts stay put
    // once the PIN exists), what has been written, and that it is the default.
    bool askPin_ = true, pinDefault_ = false;
    std::string pinWritten_;
    size_t dotCount_ = 0;
    Json state_;             // the server's /api/setup
    SetupPage page_;
    size_t selected_ = 0, leadShown_ = 0, rev_ = 0;
    // Waiting for the server to answer: at the start, after a restart, after trouble.
    bool waiting_ = false, askedServer_ = false;
    After after_ = After::Start;
    int tries_ = 0;
    double retryAt_ = 0;
    // The handoff page.
    Link link_ = Link::Preparing;
    size_t linkRev_ = 0, field_ = 0;
    bool linkOpen_ = false, restarted_ = false, polling_ = false, pollAsked_ = false,
         litCaret_ = true, joined_ = false;
    int failedFor_ = 0, goneFor_ = 0;
    double pollAt_ = 0, advanceAt_ = 0;
    std::string url_, message_;
    std::vector<SetupChoice> linkChoices_;
    std::vector<SetupField> fields_ = brokerFields();
    HandoffWords words_ = handoffWords();
    PinWords pinWords_ = pinWords();
    std::string savingText_ = say::msg("setup.saving", "Saving\xe2\x80\xa6");
    std::string connectingText_ = say::msg("setup.broker.connecting", "Connecting to the broker\xe2\x80\xa6");
    std::vector<std::string> entered_ = std::vector<std::string>(4);
    // The first step: what the main profile is called, and what the server holds (nothing, so
    // its own default, until it is written).
    std::string tvName_;
    std::string pinSent_; // the PIN on its way to the server
    std::string summaryKey_; // what the summary page was last built from
    KeyboardPad pad_;
};

// ---- Flow ---------------------------------------------------------------------------------------

void SetupScreen::waitForServer(ScreenHost &host, After after, int tries, int delayMs) {
    waiting_ = true;
    askedServer_ = false;
    after_ = after;
    tries_ = tries;
    retryAt_ = host.nowMs() + delayMs;
}
void SetupScreen::serverAnswered(ScreenHost &host) {
    waiting_ = false;
    busy_ = false;
    switch (after_) {
    case After::Start:
        loaded_ = true;
        askPin_ = setupAsksPin(state_);
        tvName_ = setupDefaultName(state_);
        showStep(host, entry_);
        break;
    case After::Network:
        goTo(host, SetupStep::HomeAssistant);
        break;
    case After::Retry:
        askPin_ = setupAsksPin(state_);
        goTo(host, fromSettings_ ? SetupStep::Handoff : SetupStep::Welcome);
        break;
    }
}
void SetupScreen::showStep(ScreenHost &host, SetupStep step) {
    Motion &motion = host.motion();
    step_ = step;
    ++rev_;
    page_ = setupPage(step_, shownState(host), netChosen_, fromSettings_, shownName(host));
    summaryKey_ = pageKey(host);
    selected_ = leadShown_ = page_.selected;
    motion.set("setup:pg", 0);
    motion.set("setup:lead", 0);
    motion.erasePrefix("setup:f:");
    motion.set(id("f", selected_), 1);
    motion.set("setup:fa", hasAction() && focusableCount() == 0 ? 1 : 0);
    // Items rise in one after another (spec 5.6); the dots move on with the step.
    for (int item = 0; item < Items; ++item) {
        motion.set(id("e", item), 1);
        motion.tween(id("e", item), 0, ms::base, Easing::out(),
                     kit::motion::stagger::pageBase + kit::motion::stagger::pageItem * std::min(item, 14));
    }
    dotCount_ = setupDotCount(shownState(host));
    for (size_t dot = 0; dot < dotCount_; ++dot)
        motion.tween(id("dot", dot), static_cast<int>(dot) < page_.dots ? 1 : 0, ms::fast,
                     Easing::out());
    if (step_ == SetupStep::Handoff)
        startLink(host);
    if (step_ == SetupStep::Name) {
        message_.clear();
        beginName(host);
    }
    if (step_ == SetupStep::PinOwn) {
        message_.clear();
        beginPin(host);
    }
    host.invalidate();
}
void SetupScreen::goTo(ScreenHost &host, SetupStep step) {
    next_ = static_cast<int>(step);
    host.motion().tween("setup:pg", 1, ms::pageOut, Easing::out());
}
size_t SetupScreen::focusableCount() const {
    size_t count = 0;
    for (const SetupChoice &choice : choices())
        count += choice.readOnly ? 0 : 1;
    return count;
}
void SetupScreen::select(ScreenHost &host, size_t index) {
    if (index == selected_ || index >= choices().size() || choices()[index].readOnly)
        return;
    Motion &motion = host.motion();
    // Focus follows the spring the design gives native focus (spec 5.2).
    const SpringParams focusSpring = SpringParams::fromDampingRatio(560, 1);
    motion.spring(id("f", selected_), 0, focusSpring);
    selected_ = index;
    motion.spring(id("f", selected_), 1, focusSpring);
    // The lead beside the rows explains the focused row: out, swap, in (spec 5.9).
    motion.tween("setup:lead", 1, ms::instant, Easing::out());
}
void SetupScreen::activate(ScreenHost &host) {
    switch (step_) {
    case SetupStep::Welcome:
        goTo(host, SetupStep::Name);
        break;
    case SetupStep::Name:
        acceptName(host);
        break;
    case SetupStep::Pin:
        choosePin(host);
        break;
    case SetupStep::PinOwn:
        acceptPin(host);
        break;
    case SetupStep::Network:
        chooseNetwork(host);
        break;
    case SetupStep::HomeAssistant:
        if (page_.choices[selected_].id == "phone")
            goTo(host, SetupStep::Handoff);
        else
            afterHomeAssistant(host);
        break;
    case SetupStep::Handoff:
        if (link_ != Link::Failed || selected_ >= linkChoices_.size())
            break;
        if (linkChoices_[selected_].id == "retry")
            goTo(host, SetupStep::Handoff); // a new code
        else {
            stopLink(host);
            if (fromSettings_)
                host.open("dashboard:settings");
            else
                afterHomeAssistant(host);
        }
        break;
    case SetupStep::Standby:
        chooseStandby(host);
        break;
    case SetupStep::Summary:
        openDashboard(host);
        break;
    case SetupStep::Trouble:
        busy_ = true;
        waitForServer(host, After::Retry, kRetries, 0);
        break;
    case SetupStep::Restarting:
        break;
    }
}
// "Use 0000" writes the default; "Choose my own" opens the keyboard for the four digits.
void SetupScreen::choosePin(ScreenHost &host) {
    if (page_.choices[selected_].id == "own")
        goTo(host, SetupStep::PinOwn);
    else
        writePin(host, kDefaultAdminPin);
}
void SetupScreen::acceptPin(ScreenHost &host) {
    const std::string pin = pad_.value();
    if (pin.size() != kAdminPinLength) {
        message_ = pinWords_.incomplete;
        return;
    }
    message_.clear();
    writePin(host, pin);
}
// The PIN is the one answer the next page waits for: the server may refuse it, and what it answers
// is the session every later request carries (answered()). The same PIN again, after Back, needs
// no second write.
void SetupScreen::writePin(ScreenHost &host, const std::string &pin) {
    if (pin == pinWritten_) {
        goTo(host, SetupStep::Network);
        return;
    }
    busy_ = true;
    pinSent_ = pin;
    Write write = waitedWrite(host, Operation::Setup, withDemo(pinBody(pin)), "pin");
    write.failure = say::msg("setup.pin.failed", "The PIN could not be saved");
    host.optimistic().run("setup:pin", write);
}
void SetupScreen::chooseNetwork(ScreenHost &host) {
    const bool open = page_.choices[selected_].id == "open";
    netChosen_ = true;
    Json body = Json::object();
    body.set("action", Json::text("network"));
    body.set("open", Json::flag(open));
    if (demo())
        state_.set("network", Json::flag(open));
    else if (open == state_["network"].boolean()) {
        goTo(host, SetupStep::HomeAssistant);
        return;
    }
    // The server restarts to apply it, so the connection may be cut before an answer: the page
    // says so ("Applying your choice") and waits for the server to come back instead of reading
    // the reply. One of the two writes that stay on the raw path, with the link's close below.
    busy_ = true;
    if (!demo())
        goTo(host, SetupStep::Restarting);
    host.services().write("setup:network", Operation::Setup, withDemo(body));
}
// The choice shows at once and the write goes out behind it; a refusal puts the old answer back
// with a message (the summary reads the shown value, so it follows).
void SetupScreen::chooseStandby(ScreenHost &host) {
    const bool on = page_.choices[selected_].id == "on";
    const bool dryRun = demo();
    ScreenHost *application = &host;
    Write write;
    write.operation = Operation::Setup;
    write.body = [dryRun](const Json &target) {
        Json body = Json::object();
        body.set("action", Json::text("alwaysReady"));
        body.set("on", Json::flag(target.boolean()));
        if (dryRun)
            body.set("demo", Json::flag(true));
        return body;
    };
    write.failure = say::msg("setup.standby.failed", "Standby could not be changed");
    write.done = [application, on](bool ok, const Json &) {
        if (ok)
            application->memory()["setup.standby"] = Json::flag(on);
    };
    host.optimistic().set("setup:standby", Json::flag(on), Json::flag(serverStandby(host)), write);
    goTo(host, SetupStep::Summary);
}
void SetupScreen::openDashboard(ScreenHost &host) {
    Json body = Json::object();
    body.set("action", Json::text("done"));
    // The dashboard opens at once; the marker is gone once the server says so, and a refusal
    // comes back as a message over whatever is showing.
    host.optimistic().run("setup:done", waitedWrite(host, Operation::Setup, withDemo(body), "done"));
    host.open("dashboard:system");
}
bool SetupScreen::serverStandby(ScreenHost &host) const {
    auto found = host.memory().find("setup.standby");
    return found != host.memory().end() ? found->second.boolean() : state_["alwaysReady"].boolean();
}
std::string SetupScreen::serverName(ScreenHost &host) const {
    auto found = host.memory().find("setup.name");
    return found != host.memory().end() ? found->second.string() : setupDefaultName(state_);
}
// /api/setup as the page shows it: the standby choice as made, not as the TV last said.
Json SetupScreen::shownState(ScreenHost &host) {
    Json view = Json::parse(state_.dump());
    view.set("adminPin", Json::flag(!askPin_));
    view.set("adminPinDefault", Json::flag(pinDefault_));
    if (setupHasStandby(view))
        view.set("alwaysReady", Json::flag(host.optimistic().flag("setup:standby", serverStandby(host))));
    return view;
}
std::string SetupScreen::pageKey(ScreenHost &host) {
    return std::string(host.optimistic().flag("setup:standby", serverStandby(host)) ? "1" : "0") +
           (pinDefault_ ? "1" : "0") + shownName(host);
}
std::string SetupScreen::shownName(ScreenHost &host) {
    return host.optimistic().text("setup:name", serverName(host));
}

bool SetupScreen::key(ScreenHost &host, Key key) {
    // Nothing is chosen while the page loads, changes or waits on the server.
    if (!loaded_ || busy_ || next_ >= 0 || advanceAt_ > 0)
        return true;
    if (usesKeys()) {
        KeyboardPad::Result result = pad_.key(host.motion(), host.nowMs(), key);
        if (result == KeyboardPad::Result::Done)
            fieldDone(host);
        host.invalidate();
        return true;
    }
    if (key == Key::Ok) {
        activate(host);
    } else if (key == Key::Up || key == Key::Down) {
        const int step = key == Key::Up ? -1 : 1;
        for (int i = static_cast<int>(selected_) + step; i >= 0 && i < static_cast<int>(choices().size());
             i += step)
            if (!choices()[i].readOnly) {
                select(host, static_cast<size_t>(i));
                break;
            }
    } else if (key == Key::Left)
        back(host);
    host.invalidate();
    return true;
}
bool SetupScreen::back(ScreenHost &host) {
    if (!loaded_)
        return false; // a server that does not answer does not trap the person here
    if (busy_ || next_ >= 0 || advanceAt_ > 0)
        return true;
    if (step_ == SetupStep::Handoff && field_ > 0 && usesKeys()) {
        --field_;
        message_.clear();
        beginField(host);
        return true;
    }
    if (step_ == SetupStep::Handoff)
        stopLink(host);
    if (fromSettings_) {
        host.open("dashboard:settings");
        return true;
    }
    switch (step_) {
    case SetupStep::Name:
        goTo(host, SetupStep::Welcome);
        return true;
    case SetupStep::Pin:
        goTo(host, SetupStep::Name);
        return true;
    case SetupStep::PinOwn:
        goTo(host, SetupStep::Pin);
        return true;
    case SetupStep::Network:
        goTo(host, askPin_ ? SetupStep::Pin : SetupStep::Name);
        return true;
    case SetupStep::HomeAssistant:
        goTo(host, SetupStep::Network);
        return true;
    case SetupStep::Handoff:
    case SetupStep::Standby:
        goTo(host, SetupStep::HomeAssistant);
        return true;
    case SetupStep::Summary:
        goTo(host, setupHasStandby(state_) ? SetupStep::Standby : SetupStep::HomeAssistant);
        return true;
    default:
        // Welcome leaves the app, as Back does everywhere on the TV.
        return false;
    }
}
void SetupScreen::click(ScreenHost &host, const std::string &hit) {
    if (!loaded_ || busy_ || next_ >= 0 || advanceAt_ > 0)
        return;
    if (hit == "setup:action") {
        activate(host);
        return;
    }
    if (hit.compare(0, 10, "setup:row:") == 0) {
        size_t index = static_cast<size_t>(std::atoi(hit.c_str() + 10));
        if (index == selected_)
            activate(host);
        else
            select(host, index);
        return;
    }
    if (usesKeys() && pad_.click(host.motion(), host.nowMs(), hit) == KeyboardPad::Result::Done)
        fieldDone(host);
}
bool SetupScreen::text(ScreenHost &host, const std::string &text) {
    if (!loaded_ || busy_ || next_ >= 0 || advanceAt_ > 0 || !usesKeys())
        return false;
    pad_.type(host.nowMs(), text);
    host.invalidate();
    return true;
}

// ---- Server replies -------------------------------------------------------------------------------

void SetupScreen::response(ScreenHost &host, const Response &reply) {
    if (reply.channel.compare(0, 6, "setup:") != 0)
        return;
    const Json data = reply.json();
    const bool good = reply.ok() && data.isObject();
    if (reply.channel == "setup:state") {
        askedServer_ = false;
        if (!waiting_)
            return;
        if (good) {
            state_ = data;
            serverAnswered(host);
        } else if (--tries_ <= 0) {
            waiting_ = busy_ = false;
            loaded_ = true;
            goTo(host, SetupStep::Trouble);
        } else
            retryAt_ = host.nowMs() + kRetryMs;
    } else if (reply.channel == "setup:network") {
        if (demo()) {
            busy_ = false;
            goTo(host, SetupStep::HomeAssistant);
        } else
            waitForServer(host, After::Network, kRetries, kRestartWaitMs);
    } else if (reply.channel == "setup:poll") {
        pollAsked_ = false;
        polled(host, reply);
    } else if (reply.channel == "setup:settings") {
        if (link_ != Link::Sending)
            return;
        if (!good || !data["ok"].boolean()) {
            brokerFailed(host, data["error"].string());
            return;
        }
        host.optimistic().run("setup:save",
                              waitedWrite(host, Operation::Settings,
                                          brokerSettings(data, trimmed(entered_[0]), entered_[1],
                                                         trimmed(entered_[2]), entered_[3]),
                                          "save"));
    }
    host.invalidate();
}
// The answer to a write the page waited for (waitedWrite): what it was for decides what follows.
void SetupScreen::answered(ScreenHost &host, const std::string &name, bool ok, const Json &data) {
    if (name == "pin") {
        busy_ = false;
        if (ok && (step_ == SetupStep::Pin || step_ == SetupStep::PinOwn)) {
            // A dry run answers no session: nothing was set.
            host.services().adminSession(data["session"].string());
            pinWritten_ = pinSent_;
            pinDefault_ = pinSent_ == kDefaultAdminPin;
            goTo(host, SetupStep::Network);
        }
    } else if (name == "handoff") {
        if (step_ != SetupStep::Handoff || link_ != Link::Preparing)
            return;
        if (!ok || data["url"].string().empty()) {
            linkFailed(host, "failed");
            return;
        }
        url_ = data["url"].string();
        link_ = Link::Ready;
        polling_ = true;
        pollAt_ = host.nowMs() + kPollMs;
        host.motion().ambient("setup:sweep");
        host.motion().ambient("setup:pulse");
    } else if (name == "save") {
        if (link_ != Link::Sending)
            return;
        if (!ok) {
            brokerFailed(host, data["error"].string());
            return;
        }
        // The server restarts to take the broker, which ends the phone link with it; the polls
        // that fail are that restart.
        restarted_ = true;
        failedFor_ = 0;
        linkOpen_ = false;
        polling_ = true;
        pollAt_ = host.nowMs() + kPollMs;
    }
    host.invalidate();
}
void SetupScreen::polled(ScreenHost &host, const Response &reply) {
    if (step_ != SetupStep::Handoff || !polling_)
        return;
    const Json data = reply.json();
    // The server closes the link and restarts once the phone sends its details, so a poll that
    // fails means they arrived. No link and no restart, twice running, means the code ran out.
    if (!reply.ok() || !data.isObject()) {
        restarted_ = true;
        return;
    }
    state_ = data;
    const std::string haState = data["homeAssistant"]["state"].string();
    if (restarted_ && haState == "connected")
        connected(host);
    else if (restarted_ && haState == "error" && ++failedFor_ > 5)
        linkFailed(host, "notConnected");
    else if (!restarted_ && data["handoff"].string().empty() && ++goneFor_ >= 2)
        linkFailed(host, "expired");
}
void SetupScreen::connected(ScreenHost &host) {
    polling_ = false;
    joined_ = true; // the page says Connected for a moment, then moves on
    message_.clear();
    advanceAt_ = host.nowMs() + kAdvanceMs;
}
void SetupScreen::linkFailed(ScreenHost &host, const std::string &reason) {
    polling_ = false;
    link_ = Link::Failed;
    message_ = handoffFailed(reason);
    linkChoices_ = handoffChoices(fromSettings_);
    selected_ = leadShown_ = 0;
    host.motion().erasePrefix("setup:f:");
    host.motion().set(id("f", 0), 1);
    host.invalidate();
}
void SetupScreen::startLink(ScreenHost &host) {
    link_ = Link::Preparing;
    linkRev_ = rev_;
    url_.clear();
    message_.clear();
    restarted_ = polling_ = pollAsked_ = joined_ = false;
    failedFor_ = goneFor_ = 0;
    advanceAt_ = 0;
    field_ = 0;
    entered_.assign(4, "");
    linkChoices_.clear();
    beginField(host);
    linkOpen_ = true;
    Json body = Json::object();
    body.set("action", Json::text("handoff"));
    host.optimistic().run("setup:handoff", waitedWrite(host, Operation::Setup, body, "handoff"));
}
void SetupScreen::stopLink(ScreenHost &host) {
    if (!linkOpen_)
        return;
    linkOpen_ = false;
    Json body = Json::object();
    body.set("action", Json::text("handoffStop"));
    host.services().write("setup:stop", Operation::Setup, withDemo(body));
}
void SetupScreen::beginField(ScreenHost &host) {
    const SetupField &field = fields_[field_];
    KeyboardPad::Options options;
    options.id = "setup:kb";
    options.maximum = field.maximum;
    options.address = field.address;
    pad_.begin(host.motion(), entered_[field_].empty() && field.id == "port" ? "1883" : entered_[field_],
               options);
    pad_.setDone(field.done, field.doneIcon);
}
void SetupScreen::beginName(ScreenHost &host) {
    KeyboardPad::Options options;
    options.id = "setup:kb";
    options.maximum = 20; // the Home profile's limit
    pad_.begin(host.motion(), shownName(host), options);
    pad_.setDone(nameWords().done, "arrowright");
}
void SetupScreen::beginPin(ScreenHost &host) {
    KeyboardPad::Options options;
    options.id = "setup:kb";
    options.maximum = kAdminPinLength;
    options.digits = true;
    pad_.begin(host.motion(), "", options);
    pad_.setDone(pinWords_.done, "arrowright");
}
// The name on the keyboard is the main profile's. It shows as the name at once and the next
// question comes; the write goes out behind it, and a refusal puts the old name back with a
// message. The TV's own series stays the server's default, so nothing is written for it.
void SetupScreen::acceptName(ScreenHost &host) {
    std::string name = trimmed(pad_.value());
    if (name.empty())
        name = setupDefaultName(state_);
    tvName_ = name;
    message_.clear();
    if (!demo()) {
        ScreenHost *application = &host;
        Write write;
        write.operation = Operation::SetProfile;
        write.body = [](const Json &target) { return nameBody(target.string()); };
        write.failure = say::msg("tv.editor.saveFailed", "The name could not be saved. Try again.");
        write.done = [application, name](bool ok, const Json &) {
            if (ok)
                application->memory()["setup.name"] = Json::text(name);
        };
        host.optimistic().set("setup:name", Json::text(name), Json::text(serverName(host)), write);
    }
    goTo(host, afterName());
}
void SetupScreen::fieldDone(ScreenHost &host) {
    if (step_ == SetupStep::Name) {
        acceptName(host);
        return;
    }
    if (step_ == SetupStep::PinOwn) {
        acceptPin(host);
        return;
    }
    entered_[field_] = pad_.value();
    message_.clear();
    if (field_ + 1 < entered_.size()) {
        ++field_;
        beginField(host);
    } else
        submitBroker(host);
}
void SetupScreen::submitBroker(ScreenHost &host) {
    const std::string problem = brokerProblem(entered_[0], entered_[1]);
    if (!problem.empty()) {
        message_ = problem;
        field_ = trimmed(entered_[0]).empty() ? 0 : 1;
        beginField(host);
        return;
    }
    link_ = Link::Sending;
    message_.clear();
    if (demo()) {
        // The dry run saves nothing and shows the page it would reach.
        connected(host);
        return;
    }
    host.services().read("setup:settings", Operation::Settings);
}
void SetupScreen::brokerFailed(ScreenHost &host, const std::string &error) {
    link_ = Link::Ready;
    message_ = error.empty() ? say::msg("phone.failed", "That did not work. Try again.") : error;
    field_ = 0;
    beginField(host);
}

// ---- Time -----------------------------------------------------------------------------------------

bool SetupScreen::update(ScreenHost &host) {
    Motion &motion = host.motion();
    const double now = host.nowMs();
    bool draw = false;
    for (const char *name : {"handoff", "save", "pin"}) {
        bool ok = false;
        Json data;
        if (takeReply(host, name, ok, data)) {
            answered(host, name, ok, data);
            draw = true;
        }
    }
    if (waiting_ && !askedServer_ && now >= retryAt_) {
        askedServer_ = true;
        host.services().read("setup:state", Operation::Setup);
    }
    // The old page has left: the next one comes in.
    if (next_ >= 0 && motion.value("setup:pg") >= .999) {
        SetupStep step = static_cast<SetupStep>(next_);
        next_ = -1;
        showStep(host, step);
    }
    if (step_ == SetupStep::Handoff && polling_ && !pollAsked_ && now >= pollAt_) {
        pollAsked_ = true;
        pollAt_ = now + kPollMs;
        host.services().read("setup:poll", Operation::Setup);
    }
    if (advanceAt_ > 0 && now >= advanceAt_) {
        advanceAt_ = 0;
        if (fromSettings_) {
            stopLink(host);
            host.open("dashboard:settings");
            return true;
        }
        afterHomeAssistant(host);
    }
    // The lead has faded out for a new focus: show that row's words and bring it back.
    if (leadShown_ != selected_ && motion.value("setup:lead") >= .999) {
        leadShown_ = selected_;
        motion.tween("setup:lead", 0, ms::fast, Easing::out());
    }
    // The scan line and the waiting dot loop for as long as the code is on screen.
    if (showsCode() && !motion.moving("setup:sweep")) {
        motion.ambient("setup:sweep");
        motion.play("setup:sweep", Tween(0, 1, 3200, Easing::linear()));
    }
    if (showsCode() || waitsOnServer()) {
        if (!motion.moving("setup:pulse")) {
            motion.ambient("setup:pulse");
            motion.play("setup:pulse", Tween(0, 1, 1600, Easing::linear()));
        }
    }
    if (usesKeys()) {
        bool lit = pad_.caretLit(now);
        if (lit != litCaret_) {
            litCaret_ = lit;
            draw = true;
        }
    }
    return draw;
}

// ---- Drawing --------------------------------------------------------------------------------------

Enter SetupScreen::enterOf(ScreenHost &host, int item) const {
    const Motion &motion = host.motion();
    double hidden = std::max(motion.value(id("e", static_cast<size_t>(item))), motion.value("setup:pg"));
    return {static_cast<float>(1 - hidden), static_cast<float>(28 * hidden)};
}
std::string SetupScreen::leadText() const {
    if (step_ == SetupStep::Handoff) {
        if (link_ == Link::Failed)
            return message_;
        if (link_ == Link::Sending)
            return connectingText_;
        return link_ == Link::Preparing ? words_.preparing : words_.scan;
    }
    // A page with its own lead keeps it above the focused row's words.
    if (!page_.choices.empty() && !page_.choices[0].readOnly && leadShown_ < page_.choices.size())
        return page_.lead.empty() ? page_.choices[leadShown_].hint
                                  : page_.lead + " " + page_.choices[leadShown_].hint;
    return page_.lead;
}
void SetupScreen::drawLeft(ScreenHost &host, kit::Paint paint) {
    Scene &scene = paint.scene;
    const Motion &motion = host.motion();
    Enter label = enterOf(host, Label);
    {
        kit::Layer layer(scene, label);
        scene.shape({kLeft, 176 + 9, 10, 10}, kit::color::accent, 5);
        paint.glyphs.text(scene, {kLeft + 22, 176, 738, 28}, page_.step, kit::type::labelL,
                          kit::color::accent);
    }
    {
        kit::Layer layer(scene, enterOf(host, Title));
        paint.glyphs.text(scene, kit::metrics::setup::title, page_.title, kit::type::setupTitle,
                          kit::color::onSurface, Node::Align::Left, 2);
    }
    Enter lead = enterOf(host, Lead);
    float swap = static_cast<float>(motion.value("setup:lead"));
    lead.alpha *= 1 - swap;
    lead.dy += 10 * swap;
    kit::Layer layer(scene, lead);
    paint.glyphs.text(scene, {kLeft, 382, 700, kit::type::body.line * 8}, leadText(),
                      kit::type::body, step_ == SetupStep::Handoff && link_ == Link::Failed
                                           ? kit::color::warn
                                           : kit::color::onSurface2,
                      Node::Align::Left, 8);
}
void SetupScreen::drawChoices(ScreenHost &host, kit::Paint paint) {
    const Motion &motion = host.motion();
    Scene &scene = paint.scene;
    const auto &rows = choices();
    for (size_t i = 0; i < rows.size(); ++i) {
        const SetupChoice &row = rows[i];
        const float focus = row.readOnly ? 0.0f : static_cast<float>(motion.value(id("f", i)));
        const Rect rest = {kRight, kRowsTop + kRowPitch * i, 896, 84};
        kit::focus::Look look = kit::focus::lookAt(rest, kit::motion::scale::row, focus,
                                                   kit::focus::leftCentre);
        Transform scale = kit::focus::transformAt(rest, look.scale, kit::focus::leftCentre);
        kit::Layer layer(scene, enterOf(host, Content + static_cast<int>(i)),
                         look.scale != 1 ? &scale : nullptr);
        scene.shape(rest, kit::color::surface1, kit::radius::lg);
        if (focus > 0)
            scene.shape(rest, kit::focus::withAlpha(kit::color::focusFill, kit::focus::clamp01(focus)),
                        kit::radius::lg);
        const Rect well = {rest.x + kit::layout::srowPadLeft, rest.y + (rest.h - 48) / 2, 48, 48};
        scene.shape(well,
                    kit::focus::mix(kit::color::surface3, {19 / 255.0f, 17 / 255.0f, 24 / 255.0f, .12f}, focus),
                    24);
        paint.glyphs.icon(scene, {well.x + 11, well.y + 11, 26, 26}, row.icon,
                          kit::focus::inkAt(kit::color::onSurface, kit::color::onFocus, focus));
        const float titleX = well.x + well.w + kit::layout::srowWellGap;
        const float valueWidth = row.value.empty() ? 0 : 220;
        paint.glyphs.text(scene,
                          {titleX, rest.y + (rest.h - kit::type::settingRow.line) / 2,
                           rest.x + rest.w - 28 - titleX - valueWidth, kit::type::settingRow.line},
                          row.label, kit::type::settingRow,
                          kit::focus::inkAt(kit::color::onSurface, kit::color::onFocus, focus));
        if (!row.value.empty())
            paint.glyphs.text(scene,
                              {rest.x + rest.w - 28 - valueWidth,
                               rest.y + (rest.h - kit::type::labelL.line) / 2, valueWidth,
                               kit::type::labelL.line},
                              row.value, kit::type::labelL,
                              row.on ? kit::color::accent : kit::color::onSurface2,
                              Node::Align::Right);
        if (!row.readOnly)
            scene.hits.push_back({look.rect, "setup:row:" + std::to_string(i)});
    }
}
void SetupScreen::drawWaiting(ScreenHost &host, kit::Paint paint) {
    static const Easing inOut = Easing::inOut();
    const double p = host.motion().value("setup:pulse");
    const float u = static_cast<float>(p < .5 ? inOut(p * 2) : 1 - inOut((p - .5) * 2)),
                size = 12 * (.8f + .2f * u);
    kit::Layer layer(paint.scene, enterOf(host, Lead));
    paint.scene.shape({kLeft + 6 - size / 2, 548 - size / 2, size, size},
                      kit::focus::withAlpha(kit::color::accent, .35f + .65f * u), size / 2);
    paint.glyphs.text(paint.scene, {kLeft + 28, 534, 400, kit::type::labelL.line}, savingText_,
                      kit::type::labelL, kit::color::onSurface2);
}
void SetupScreen::drawHandoff(ScreenHost &host, kit::Paint paint) {
    Scene &scene = paint.scene;
    const Motion &motion = host.motion();
    const HandoffWords &words = words_;
    if (showsCode()) {
        kit::Layer layer(scene, enterOf(host, Content));
        kit::QrTile tile;
        tile.text = url_;
        // The line runs the tile's height on the in-out curve and shows only in the middle of its
        // run (spec 5.9).
        static const Easing inOut = Easing::inOut();
        const double t = motion.value("setup:sweep");
        tile.sweep = static_cast<float>(inOut(t));
        tile.sweepAlpha = static_cast<float>(
            .85 * (t < .12 ? inOut(t / .12) : t > .88 ? 1 - inOut((t - .88) / .12) : 1));
        kit::qrTile(paint, tile);
        paint.glyphs.text(scene, {380, 529, 380, kit::type::url.line}, addressOf(url_),
                          kit::type::url, kit::color::onSurface);
        paint.glyphs.text(scene, {380, 573, 380, kit::type::body.line * 3}, words.address,
                          kit::type::body, kit::color::onSurface2, Node::Align::Left, 3);
        // The dot breathes between .35 and 1 opacity and .8 and 1 size (spec 5.9).
        const double p = motion.value("setup:pulse");
        const float u = static_cast<float>(p < .5 ? inOut(p * 2) : 1 - inOut((p - .5) * 2)),
                    size = 10 * (.8f + .2f * u);
        scene.shape({385 - size / 2, 701 - size / 2, size, size},
                    kit::focus::withAlpha(kit::color::accent, .35f + .65f * u), size / 2);
        paint.glyphs.text(scene, {400, 692, 360, kit::type::labelM.line},
                          joined_ ? words.connected : words.waiting, kit::type::labelM,
                          kit::color::onSurface3);
    }
    if (link_ == Link::Sending && joined_)
        paint.glyphs.text(scene, {kLeft, 530, 700, kit::type::title.line}, words.connected,
                          kit::type::title, kit::color::accent);
}
// The field and the keyboard of the right-hand column: the name's, or the broker's field by field.
void SetupScreen::drawEntry(ScreenHost &host, kit::Paint paint) {
    Scene &scene = paint.scene;
    std::string label, placeholder;
    bool secret = false;
    if (step_ == SetupStep::Name) {
        label = nameWords().label;
        placeholder = setupDefaultName(state_);
    } else if (step_ == SetupStep::PinOwn) {
        label = pinWords_.label;
        placeholder = pinWords_.placeholder;
        secret = true;
    } else {
        label = fields_[field_].label;
        placeholder = fields_[field_].placeholder;
        secret = fields_[field_].secret;
    }
    {
        kit::Layer layer(scene, enterOf(host, Field));
        const bool failing = !message_.empty();
        paint.glyphs.text(scene, {kRight, 176, 896, kit::type::title.line},
                          failing ? message_ : label, kit::type::title,
                          failing ? kit::color::warn : kit::color::onSurface2);
        kit::textField(paint, pad_.field(host.nowMs(), kit::metrics::setup::field, placeholder, secret));
    }
    pad_.paint(paint, host.motion(), enterOf(host, Keys));
}
void SetupScreen::drawFooter(ScreenHost &host, kit::Paint paint) {
    const Motion &motion = host.motion();
    if (page_.dots > 0 && !fromSettings_) {
        kit::StepDots dots;
        dots.origin = kDots;
        for (size_t i = 0; i < dotCount_; ++i)
            dots.on.push_back(static_cast<float>(motion.value(id("dot", i))));
        dots.enter = enterOf(host, Dots);
        kit::stepDots(paint, dots);
    }
    std::vector<kit::Hint> hints = {{"", say::msg("setup.hint.move", "Move")}};
    if (usesKeys()) {
        hints.push_back({"OK", say::msg("setup.hint.type", "Type")});
        const bool handoff = step_ == SetupStep::Handoff;
        hints.push_back({"Back", handoff && field_ > 0 ? say::msg("setup.hint.previous", "Previous field")
                                 : fromSettings_ ? say::msg("setup.handoff.backToSettings", "Back to Settings")
                                 : handoff ? say::msg("setup.hint.skip", "Skip for now")
                                           : say::msg("setup.hint.back", "Back")});
    } else {
        hints.push_back({"OK", say::msg("setup.hint.choose", "Choose")});
        if (step_ != SetupStep::Welcome && step_ != SetupStep::Restarting)
            hints.push_back({"Back", say::msg("setup.hint.back", "Back")});
    }
    kit::hintBar(paint, kHints, hints, enterOf(host, Hints));
}
void SetupScreen::build(ScreenHost &host, kit::Paint paint) {
    if (!loaded_)
        return;
    Scene &scene = paint.scene;
    const Motion &motion = host.motion();
    if (step_ == SetupStep::Summary && pageKey(host) != summaryKey_) {
        // A write that was taken back changes what the summary says.
        page_ = setupPage(step_, shownState(host), netChosen_, fromSettings_, shownName(host));
        summaryKey_ = pageKey(host);
    }
    drawLeft(host, paint);
    if (hasAction()) {
        kit::KeyCap button;
        button.rest = kAction;
        button.id = "setup:action";
        button.label = step_ == SetupStep::Trouble ? say::msg("setup.handoff.retry", "Try again")
                                                   : page_.action;
        button.accent = true;
        button.focus = static_cast<float>(motion.value("setup:fa"));
        button.enter = enterOf(host, Action);
        kit::keyCap(paint, button);
    }
    if (step_ == SetupStep::Handoff)
        drawHandoff(host, paint);
    if (waitsOnServer())
        drawWaiting(host, paint);
    if (usesKeys())
        drawEntry(host, paint);
    if (onChoices())
        drawChoices(host, paint);
    if (step_ == SetupStep::Welcome) {
        kit::Layer layer(scene, enterOf(host, Content));
        drawMark(paint, kMark, .92f);
    }
    drawFooter(host, paint);
}
Json SetupScreen::snapshot() const {
    Json value = Json::object();
    value.set("step", Json::text(setupStepName(step_)));
    value.set("loaded", Json::flag(loaded_));
    value.set("busy", Json::flag(busy_));
    value.set("selected", Json::numeric(static_cast<double>(selected_)));
    value.set("fromSettings", Json::flag(fromSettings_));
    value.set("tvName", Json::text(tvName_));
    value.set("pinSet", Json::flag(!pinWritten_.empty()));
    value.set("pinDefault", Json::flag(pinDefault_));
    if (usesKeys()) {
        value.set("typed", Json::text(pad_.value()));
        value.set("focusedKey", Json::text(pad_.focusedKey()));
    }
    if (step_ == SetupStep::Handoff) {
        static const char *links[] = {"preparing", "ready", "failed", "sending"};
        value.set("link", Json::text(links[static_cast<int>(link_)]));
        value.set("field", Json::numeric(static_cast<double>(field_)));
        value.set("url", Json::text(url_));
    }
    return value;
}
} // namespace

std::unique_ptr<ScreenView> makeSetupScreen(const std::string &route) {
    SetupStep step;
    if (route.compare(0, 6, "setup:") != 0 || !setupStepFromName(route.substr(6), step) ||
        step == SetupStep::Restarting || step == SetupStep::Trouble || step == SetupStep::PinOwn)
        return nullptr;
    return std::unique_ptr<ScreenView>(new SetupScreen(step));
}
} // namespace nativetv
