#include "domain/models.hpp"

namespace nativetv {
std::string setupTitle(const std::string &step) {
    if (step == "network")
        return "Turn on the web dashboard?";
    if (step == "ha")
        return "Connect to Home Assistant?";
    if (step == "handoff")
        return "Finish on a phone";
    if (step == "standby")
        return "Stay connected when off?";
    if (step == "summary")
        return "Ready to go";
    return "Own the glass.";
}
std::vector<MenuRow> setupRows(const std::string &step, const Json &state) {
    if (step == "welcome")
        return {{"Get started", "",
                 "Readings, controls and privacy settings for this TV, with the remote.", "",
                 navigate("setup:network")}};
    if (step == "network")
        return {{"Yes, on phones and computers at home", "",
                 state["token"].boolean() ? "The dashboard access token is required."
                                          : "Anyone on the home network can use the dashboard.",
                 "",
                 post(Operation::Setup,
                      fields({{"action", Json::text("network")}, {"open", Json::flag(true)}}))},
                {"No, only on this TV", "", "The dashboard remains available on the TV.", "",
                 post(Operation::Setup,
                      fields({{"action", Json::text("network")}, {"open", Json::flag(false)}}))}};
    if (step == "ha")
        return {{"Set it up on a phone", "", "Connect an MQTT broker for Home Assistant.", "",
                 navigate("setup:handoff")},
                {"Not now", "", "Home Assistant can be set up later in Settings.", "",
                 navigate(state.has("alwaysReady") ? "setup:standby" : "setup:summary")}};
    if (step == "handoff")
        return {{"Create a new link", "", "Scan the QR code with a phone camera.", "",
                 post(Operation::Setup, fields({{"action", Json::text("handoff")}}))},
                {"Continue", "", "", "",
                 navigate(state.has("alwaysReady") ? "setup:standby" : "setup:summary")}};
    if (step == "standby")
        return {{"Yes, keep it connected", "",
                 "Keeps the network available with the screen dark. Uses about 12 W on a C2, apart "
                 "from five hours of full sleep each night.",
                 "",
                 post(Operation::Setup,
                      fields({{"action", Json::text("alwaysReady")}, {"on", Json::flag(true)}}))},
                {"No, let it sleep", "",
                 "Native Tv and Home Assistant wait until the TV is on again.", "",
                 post(Operation::Setup,
                      fields({{"action", Json::text("alwaysReady")}, {"on", Json::flag(false)}}))}};
    return {
        {"Web dashboard", state["network"].boolean() ? "On" : "Off", "", "", Command()},
        {"Home Assistant", state["homeAssistant"]["state"].string("Not set up"), "", "", Command()},
        {"Open dashboard", "", "", "",
         post(Operation::Setup, fields({{"action", Json::text("done")}}))}};
}
} // namespace nativetv
