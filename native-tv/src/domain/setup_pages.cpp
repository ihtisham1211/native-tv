#include "domain/setup_pages.hpp"
#include "domain/names.hpp"
#include "domain/say.hpp"
#include <cstdlib>
#include <string>

namespace nativetv {
namespace {
const struct {
    SetupStep step;
    const char *name;
} kSteps[] = {{SetupStep::Welcome, "welcome"},      {SetupStep::Name, "name"},
              {SetupStep::Pin, "pin"},              {SetupStep::PinOwn, "pinown"},
              {SetupStep::Network, "network"},
              {SetupStep::HomeAssistant, "ha"},     {SetupStep::Handoff, "handoff"},
              {SetupStep::Standby, "standby"},      {SetupStep::Summary, "summary"},
              {SetupStep::Restarting, "restarting"}, {SetupStep::Trouble, "trouble"}};

std::string stepLabel(size_t number, size_t total) {
    Json values = Json::object();
    values.set("n", Json::text(std::to_string(number)));
    values.set("total", Json::text(std::to_string(total)));
    return say::msg("setup.step", "Step {n} of {total}", values);
}
// The questions after the name and the PIN: the dashboard's network, Home Assistant and, where the
// TV has it, standby.
size_t steps(const Json &state) {
    return setupHasStandby(state) ? 3 : 2;
}
// 1 where the PIN is asked: every step after it moves one on.
size_t pinOffset(const Json &state) {
    return setupAsksPin(state) ? 1 : 0;
}
// What "Step n of total" counts: those, the name and the PIN.
size_t questions(const Json &state) {
    return steps(state) + 1 + pinOffset(state);
}
std::string quickChoices(size_t total) {
    if (total >= 5)
        return say::msg("setup.welcome.five",
                        "Five quick choices first, and you can change any of them later.");
    if (total == 4)
        return say::msg("setup.welcome.four",
                        "Four quick choices first, and you can change any of them later.");
    return say::msg("setup.welcome.three",
                    "Three quick choices first, and you can change any of them later.");
}
SetupChoice choice(const char *id, const std::string &label, const std::string &hint,
                   const char *icon) {
    SetupChoice result;
    result.id = id;
    result.label = label;
    result.hint = hint;
    result.icon = icon;
    return result;
}
SetupChoice summary(const std::string &label, const std::string &value, bool on, const char *icon) {
    SetupChoice result;
    result.label = label;
    result.value = value;
    result.on = on;
    result.icon = icon;
    result.readOnly = true;
    return result;
}
std::string onOff(bool on) {
    return on ? say::msg("common.on", "On") : say::msg("common.off", "Off");
}
} // namespace

const char *setupStepName(SetupStep step) {
    for (const auto &entry : kSteps)
        if (entry.step == step)
            return entry.name;
    return "welcome";
}
bool setupStepFromName(const std::string &name, SetupStep &step) {
    for (const auto &entry : kSteps)
        if (name == entry.name) {
            step = entry.step;
            return true;
        }
    return false;
}
bool setupHasStandby(const Json &state) {
    return state.has("alwaysReady");
}
bool setupAsksPin(const Json &state) {
    return !state["adminPin"].boolean();
}
size_t setupDotCount(const Json &state) {
    return steps(state) + 3 + pinOffset(state);
}
std::string setupDefaultName(const Json &state) {
    return names::tvDefaultName(state["model"].string());
}

SetupPage setupPage(SetupStep step, const Json &state, bool netChosen, bool fromSettings,
                    const std::string &tvName) {
    SetupPage page;
    const bool standby = setupHasStandby(state);
    const size_t offset = pinOffset(state);
    const int dotShift = static_cast<int>(offset);
    switch (step) {
    case SetupStep::Welcome:
        page.step = say::msg("setup.firstTime", "First-time setup");
        page.title = say::msg("setup.slogan", "Your TV, your rules.");
        page.lead = say::msg("setup.welcome", "Readings, controls and privacy settings for this TV, "
                                              "right here with the remote.") +
                    " " + quickChoices(questions(state));
        page.action = say::msg("setup.start", "Get started");
        page.dots = 1;
        break;
    case SetupStep::Name:
        page.step = stepLabel(1, questions(state));
        page.title = say::msg("setup.name.title", "Name this TV");
        page.lead = say::msg("setup.name.lead",
                             "This is the name of the main profile on Home. OK on Next keeps the name "
                             "shown; type another to change it.");
        page.dots = 2;
        break;
    case SetupStep::Pin:
    case SetupStep::PinOwn:
        page.step = stepLabel(2, questions(state));
        page.title = say::msg("setup.pin.title", "Choose an admin PIN");
        page.dots = 3;
        if (step == SetupStep::PinOwn) {
            page.lead = say::msg("setup.pin.own.lead", "Four digits that only you know.");
            break;
        }
        page.lead = say::msg("setup.pin.lead",
                             "These four digits are asked before apps are installed or removed, SSH "
                             "keys are changed, a backup is restored, or the TV is restarted or "
                             "powered off, on the TV and in the dashboard.");
        page.choices.push_back(choice(
            "default", say::msg("setup.pin.default", "Use 0000"),
            say::msg("setup.pin.default.hint",
                     "0000 is the default. Anyone who knows it can use it, so choose your own now, "
                     "or change it later in the web dashboard."),
            "lock"));
        page.choices.push_back(choice(
            "own", say::msg("setup.pin.own", "Choose my own"),
            say::msg("setup.pin.own.hint", "Type four digits of your own."), "keyboard"));
        break;
    case SetupStep::Network: {
        page.step = stepLabel(2 + offset, questions(state));
        page.title = say::msg("setup.net.title", "Turn on the web dashboard?");
        page.choices.push_back(choice(
            "open", say::msg("setup.net.open", "Yes, on phones and computers at home"),
            say::msg("setup.net.open.hint",
                     "A fuller version of this dashboard, with more controls, that opens in a "
                     "browser on any phone or computer on your home network.") +
                " " +
                (state["token"].boolean()
                     ? say::msg("setup.net.token", "Using it needs the dashboard\xe2\x80\x99s access token.")
                     : say::msg("setup.net.noPassword",
                                "There is no password, so anyone on that network can use it.")),
            "globe"));
        page.choices.push_back(choice(
            "closed", say::msg("setup.net.closed", "No, only on this TV"),
            say::msg("setup.net.closed.hint",
                     "Nothing else on your network can reach the dashboard. The safest choice, and "
                     "it can be switched on later in Settings."),
            "lock"));
        // A fresh install starts closed, so the TV's own state would always preselect "only on
        // this TV"; it is followed once a choice has been made.
        page.selected = netChosen && !state["network"].boolean() ? 1 : 0;
        page.dots = 3 + dotShift;
        break;
    }
    case SetupStep::HomeAssistant:
        page.step = stepLabel(3 + offset, questions(state));
        page.title = say::msg("setup.ha.title", "Connect to Home Assistant?");
        page.choices.push_back(choice(
            "phone", say::msg("setup.ha.phone", "Set it up on my phone"),
            say::msg("setup.ha.phone.hint",
                     "The TV connects through an MQTT broker, such as Home Assistant\xe2\x80\x99s "
                     "Mosquitto add-on. Scan a code with your phone and enter the broker\xe2\x80\x99s "
                     "details there."),
            "phone"));
        page.choices.push_back(
            choice("later", say::msg("setup.ha.later", "Not now"),
                   say::msg("setup.ha.later.hint",
                            "The dashboard works without it. Set it up any time from Settings."),
                   "clock"));
        page.dots = 4 + dotShift;
        break;
    case SetupStep::Standby:
        page.step = stepLabel(4 + offset, questions(state));
        page.title = say::msg("setup.standby.title", "Stay connected when off?");
        page.choices.push_back(choice(
            "on", say::msg("setup.standby.on", "Yes, keep it connected"),
            say::msg("setup.standby.on.hint",
                     "When the TV is switched off, it stays on your network with the screen dark, so "
                     "this dashboard and Home Assistant keep working and can switch it back on. It "
                     "uses about 12 W while off. For five hours each night it sleeps fully and is "
                     "offline; the dashboard can move those hours."),
            "power"));
        page.choices.push_back(choice(
            "off", say::msg("setup.standby.off", "No, let it sleep"),
            say::msg("setup.standby.off.hint",
                     "Switched off, the TV sleeps as normal and uses almost no power. The dashboard "
                     "and Home Assistant wait until it is switched back on."),
            "timer"));
        page.selected = state["alwaysReady"].boolean() ? 0 : 1;
        page.dots = 5 + dotShift;
        break;
    case SetupStep::Summary: {
        const Json ha = state["homeAssistant"];
        const std::string haState = ha["state"].string();
        page.step = say::msg("setup.summary.step", "All set");
        page.title = say::msg("setup.summary.title", "Ready to go");
        page.lead = say::msg("setup.summary.change", "Change any of these in Settings.");
        if (!tvName.empty())
            page.choices.push_back(
                summary(say::msg("setup.summary.name", "TV name"), tvName, false, "tv"));
        const bool defaultPin = state["adminPinDefault"].boolean();
        page.choices.push_back(summary(say::msg("setup.summary.pin", "Admin PIN"),
                                       defaultPin ? say::msg("setup.summary.pin.default", "Default (0000)")
                                                  : say::msg("setup.summary.pin.set", "Set"),
                                       !defaultPin, "lock"));
        page.choices.push_back(
            summary(say::msg("setup.summary.onTv", "On this TV"), say::msg("setup.yes", "Yes"), true, "tv"));
        page.choices.push_back(summary(say::msg("tv.settings.webDashboard", "Web dashboard"),
                                       onOff(state["network"].boolean()), state["network"].boolean(),
                                       "globe"));
        page.choices.push_back(summary(
            say::msg("tv.settings.homeAssistant", "Home Assistant"),
            haState == "connected"
                ? say::msg("mqtt.state.connected", "Connected")
                : ha["configured"].boolean() ? say::msg("mqtt.state.connecting", "Connecting")
                                             : say::msg("tv.settings.ha.notSetUp", "Not set up"),
            haState == "connected", "ha"));
        if (standby)
            page.choices.push_back(summary(say::msg("setup.summary.standby", "Stays connected when off"),
                                           onOff(state["alwaysReady"].boolean()),
                                           state["alwaysReady"].boolean(), "power"));
        page.action = say::msg("setup.summary.open", "Open dashboard");
        page.dots = static_cast<int>(steps(state)) + 3 + dotShift;
        break;
    }
    case SetupStep::Restarting:
        page.step = say::msg("setup.restarting.step", "One moment");
        page.title = say::msg("setup.restarting.title", "Applying your choice\xe2\x80\xa6");
        page.dots = 3 + dotShift;
        break;
    case SetupStep::Trouble:
        page.step = say::msg("setup.trouble.step", "Setup");
        page.title = say::msg("tv.msg.wentWrong", "Something went wrong");
        page.lead = say::msg("setup.trouble", "The dashboard stopped answering. Press OK to try again.");
        break;
    case SetupStep::Handoff:
        page.step = say::msg("setup.handoff.step", "Home Assistant");
        page.title = say::msg("setup.handoff.title", "Finish on your phone");
        page.dots = 4 + dotShift;
        break;
    }
    if (fromSettings)
        page.dots = 0;
    return page;
}

HandoffWords handoffWords() {
    HandoffWords words;
    words.step = say::msg("setup.handoff.step", "Home Assistant");
    words.title = say::msg("setup.handoff.title", "Finish on your phone");
    words.preparing = say::msg("setup.handoff.preparing", "Setting up the link\xe2\x80\xa6");
    words.scan = say::msg("setup.handoff.scan",
                          "Scan this with your phone\xe2\x80\x99s camera. This screen moves on by "
                          "itself once the TV connects.");
    words.address = say::msg("setup.handoff.address",
                             "Open the camera on a phone on the same Wi-Fi and point it at the code.");
    words.waiting = say::msg("setup.handoff.waiting", "Waiting for your phone\xe2\x80\xa6");
    words.connected = say::msg("mqtt.state.connected", "Connected");
    return words;
}
std::string handoffFailed(const std::string &reason) {
    if (reason == "notConnected")
        return say::msg("setup.handoff.notConnected",
                        "It did not connect. The broker\xe2\x80\x99s details may be wrong, or it may "
                        "not be running.");
    if (reason == "expired")
        return say::msg("setup.handoff.expired", "The code has expired.");
    return say::msg("setup.handoff.failed", "The link could not be opened.");
}
std::vector<SetupChoice> handoffChoices(bool fromSettings) {
    return {choice("retry", say::msg("setup.handoff.retry", "Try again"),
                   say::msg("setup.handoff.retry.hint", "A new code to scan with your phone."),
                   "refresh"),
            choice("skip",
                   fromSettings ? say::msg("setup.handoff.backToSettings", "Back to Settings")
                                : say::msg("setup.handoff.skip", "Skip for now"),
                   say::msg("setup.ha.later.hint",
                            "The dashboard works without it. Set it up any time from Settings."),
                   "close")};
}

NameWords nameWords() {
    NameWords words;
    words.label = say::msg("setup.name.label", "TV name");
    words.done = say::msg("setup.name.next", "Next");
    return words;
}
PinWords pinWords() {
    PinWords words;
    words.label = say::msg("setup.pin.label", "Admin PIN");
    words.placeholder = say::msg("setup.pin.placeholder", "4 digits");
    words.done = say::msg("setup.pin.next", "Next");
    words.incomplete = say::msg("setup.pin.incomplete", "Enter four digits.");
    return words;
}
Json pinBody(const std::string &pin) {
    Json body = Json::object();
    body.set("action", Json::text("adminPin"));
    body.set("pin", Json::text(pin));
    return body;
}
Json nameBody(const std::string &name) {
    Json body = Json::object();
    body.set("action", Json::text("rename"));
    body.set("id", Json::text("main"));
    body.set("name", Json::text(name));
    return body;
}

std::vector<SetupField> brokerFields() {
    SetupField host, port, user, pass;
    host.id = "host";
    host.label = say::msg("setup.broker.host", "Or enter the broker address here");
    host.placeholder = "192.168.1.10";
    host.done = say::msg("setup.broker.next", "Next");
    host.doneIcon = "arrowright";
    host.address = true;
    port.id = "port";
    port.label = say::msg("setup.broker.port", "Port");
    port.placeholder = "1883";
    port.done = host.done;
    port.doneIcon = host.doneIcon;
    port.maximum = 5;
    user.id = "username";
    user.label = say::msg("setup.broker.user", "Username, if the broker asks for one");
    user.placeholder = say::msg("setup.broker.optional", "Optional");
    user.done = host.done;
    user.doneIcon = host.doneIcon;
    user.maximum = 64;
    pass.id = "password";
    pass.label = say::msg("setup.broker.password", "Password, if the broker asks for one");
    pass.placeholder = user.placeholder;
    pass.done = say::msg("setup.broker.connect", "Connect");
    pass.doneIcon = "check";
    pass.secret = true;
    pass.maximum = 128;
    return {host, port, user, pass};
}
std::string brokerProblem(const std::string &host, const std::string &port) {
    if (host.find_first_not_of(" \t") == std::string::npos)
        return say::msg("phone.needHost", "Enter the broker address.");
    char *end = nullptr;
    long number = std::strtol(port.c_str(), &end, 10);
    if (port.empty() || *end || number < 1 || number > 65535)
        return say::msg("phone.badPort", "The port is a number from 1 to 65535.");
    return "";
}
Json brokerSettings(const Json &current, const std::string &host, const std::string &port,
                    const std::string &username, const std::string &password) {
    const Json held = current["mqtt"];
    Json mqtt = Json::object();
    mqtt.set("enabled", Json::flag(true));
    mqtt.set("host", Json::text(host));
    mqtt.set("port", Json::numeric(std::strtol(port.c_str(), nullptr, 10)));
    // The phone form moves to the TLS port together with its switch; the same rule here.
    mqtt.set("tls", Json::flag(port == "8883"));
    mqtt.set("username", Json::text(username));
    mqtt.set("password", Json::text(password));
    for (const char *key : {"tlsRejectUnauthorized", "topicPrefix", "discoveryPrefix",
                            "telemetryIntervalMs", "entities"})
        if (held.has(key))
            mqtt.set(key, held[key]);
    Json device = Json::object();
    device.set("id", current["device"]["id"]);
    device.set("name", current["device"]["name"]);
    Json body = Json::object();
    body.set("mqtt", mqtt);
    body.set("device", device);
    if (current["iconServer"].isBoolean())
        body.set("iconServer", current["iconServer"]);
    return body;
}
} // namespace nativetv
