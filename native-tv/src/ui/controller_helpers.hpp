#pragma once
#include "ui/application.hpp"
#include <algorithm>

namespace nativetv {
namespace {
inline size_t clampIndex(size_t value, size_t count) {
    return count ? std::min(value, count - 1) : 0;
}
inline std::vector<size_t> navigationStops(const ViewState &state) {
    std::vector<size_t> stops;
    for (size_t i = 0; i < state.home.tabs.size(); ++i)
        stops.push_back(i);
    // The avatar opens Who's watching?, where the one profile's name and colour are edited too.
    if (state.catalogue["profiles"].size())
        stops.insert(stops.begin(), state.home.tabs.size());
    // Search is drawn left of the gear (Quick settings), so the focus reaches it first.
    if (state.discover["titlesEnabled"].boolean())
        stops.push_back(state.home.tabs.size() + 2);
    stops.push_back(state.home.tabs.size() + 1);
    return stops;
}
inline MenuRow option(const std::string &label, const Command &command,
                      const std::string &value = "", const std::string &hint = "") {
    return {label, value, hint, "", command};
}
} // namespace
} // namespace nativetv
