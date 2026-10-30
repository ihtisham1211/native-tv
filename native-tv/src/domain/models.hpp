#pragma once
#include "core/json.hpp"
#include "core/operations.hpp"
#include <string>
#include <utility>
#include <vector>

namespace nativetv {
struct Command {
    Operation operation = Operation::None;
    std::string route;
    Json body;
    bool confirm = false;
    bool empty() const {
        return operation == Operation::None && route.empty();
    }
};
Command post(Operation operation, const Json &body, bool confirm = false);
Command control(const std::string &action, const Json &value = Json(), bool confirm = false);
Command navigate(const std::string &route);
Json fields(std::initializer_list<std::pair<std::string, Json>> values);
struct MenuRow {
    std::string label, value, hint, image;
    Command command;
    bool heading = false;
    // What OK does, in a few words, where "Turn off" or "Change" would not say it.
    std::string action = std::string();
    // 1 or 0 for a switch whose value is not On or Off (a timer, a tile), -1 for anything else.
    int state = -1;
    // A row that steps through values: each value it can read, with the command that sets it, in
    // the order OK steps through them. Empty for a row that is not a stepper.
    std::vector<std::pair<std::string, Command>> choices =
        std::vector<std::pair<std::string, Command>>();
    // A switch's way back: the command that sets the other state, where it is not `command` with
    // its flag turned over (which is what the Settings screen uses when this is empty).
    Command reverse = Command();
    // On a heading with no label: the title of the subsection that starts here, for the Settings
    // screen's tabs. Rows keep their order, so a model with no subsections is one list.
    std::string page = std::string();
};
struct Card {
    std::string id, kind, title, subtitle, image, backdrop;
    Json data;
    Command command;
};
struct CardRow {
    std::string key, title;
    bool posters = false;
    std::vector<Card> cards;
};
struct Tab {
    std::string key, title;
    Operation operation = Operation::None;
};
std::vector<Tab> homeTabs(const Json &catalogue);
std::vector<CardRow> homeRows(const std::string &tab, const Json &catalogue, const Json &discover,
                              const Json &guide);
std::string artwork(const Json &item, bool hero = false);
std::string titleFacts(const Json &item);
std::string pictureLabel(const Json &picture);
std::vector<Tab> dashboardTabs();
std::vector<MenuRow> dashboardRows(const std::string &tab, const Json &data, const Json &caps,
                                   const std::string &sub = "");
std::vector<MenuRow> setupRows(const std::string &step, const Json &state);
std::string setupTitle(const std::string &step);
} // namespace nativetv
