#pragma once
#include "core/json.hpp"
#include "ui/optimistic.hpp"
#include <cctype>
#include <string>

// The profile every TV starts with is named "Main" by the server; on screen it takes the TV's own
// name: "B3" on an OLED65B3PUA, "C4" on an OLED55C4PUA, "LGTV" where the model gives none. Once
// the person renames it, the name they chose is shown.
namespace nativetv {
// "OLED65B3PUA.DUSQLJR" -> "B3", "OLED55CXPUA" -> "CX", anything else -> "LGTV".
inline std::string tvShortName(const std::string &model) {
    if (model.compare(0, 4, "OLED") != 0)
        return "LGTV";
    size_t at = 4;
    while (at < model.size() && std::isdigit(static_cast<unsigned char>(model[at])))
        ++at; // the size
    if (at >= model.size() || !std::isupper(static_cast<unsigned char>(model[at])))
        return "LGTV";
    std::string series(1, model[at++]);
    if (at < model.size() && std::isdigit(static_cast<unsigned char>(model[at])))
        series += model[at];
    else if (at + 1 < model.size() && std::isupper(static_cast<unsigned char>(model[at])) &&
             model[at + 1] == 'P')
        series += model[at]; // CX, GX
    return series.size() > 1 ? series : "LGTV";
}
inline std::string profileName(const Json &person, const std::string &model) {
    std::string name = person["name"].string();
    if (person["id"].string() == "main" && name == "Main")
        return tvShortName(model);
    return name;
}
// The name on screen: a rename that is still on its way shows at once.
inline std::string shownProfileName(Optimistic &layer, const Json &person, const std::string &model) {
    return layer.text("profile.name." + person["id"].string(), profileName(person, model));
}
} // namespace nativetv
