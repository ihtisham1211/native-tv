#pragma once
#include "core/json.hpp"
#include <string>
#include <vector>

namespace nativetv {
// First-run setup as a page per step, the words and choices of setup.js. The screen reads these;
// nothing here draws or talks to the server.
enum class SetupStep { Welcome, Network, HomeAssistant, Handoff, Standby, Summary, Restarting, Trouble };
const char *setupStepName(SetupStep step); // "welcome", "network", "ha", "handoff", ...
bool setupStepFromName(const std::string &name, SetupStep &step);

struct SetupChoice {
    std::string id, label, hint, icon, value;
    bool on = false;       // value is "On"/"Yes"/"Connected": drawn in the accent
    bool readOnly = false; // a line of the summary, never focused
};
struct SetupPage {
    std::string step, title, lead;
    std::vector<SetupChoice> choices;
    std::string action; // the accent button: "Get started", "Open dashboard"
    int dots = 0;       // dots lit; 0 draws none
    size_t selected = 0;
};
// The server's /api/setup answer says what this TV has: an `alwaysReady` field means LG's
// Always-on exists and the standby step is asked.
bool setupHasStandby(const Json &state);
size_t setupDotCount(const Json &state);
// `netChosen`: the person has already answered the network question, so the TV's state (which
// starts closed on a fresh install) decides the preselected row. `fromSettings`: only the phone
// handoff runs, so there are no dots and Back returns to Settings.
SetupPage setupPage(SetupStep step, const Json &state, bool netChosen, bool fromSettings);

// The handoff page's own words.
struct HandoffWords {
    std::string step, title, preparing, scan, address, waiting, connected;
};
HandoffWords handoffWords();
std::string handoffFailed(const std::string &reason); // reason: "failed", "notConnected", "expired"
std::vector<SetupChoice> handoffChoices(bool fromSettings);

// The fields typed on the TV in place of the phone: the broker's address, port, user, password.
struct SetupField {
    std::string id, label, placeholder, done, doneIcon;
    size_t maximum = 80;
    bool secret = false, address = false;
};
std::vector<SetupField> brokerFields();
// Why `host` and `port` cannot be sent, or empty when they can (the phone form's checks).
std::string brokerProblem(const std::string &host, const std::string &port);
// The /api/settings body that saves the broker over the settings the server returned.
Json brokerSettings(const Json &current, const std::string &host, const std::string &port,
                    const std::string &username, const std::string &password);
} // namespace nativetv
