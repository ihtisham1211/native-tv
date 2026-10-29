#pragma once
#include "core/json.hpp"
#include <string>
#include <vector>

// What the Live page shows of /api/stats, as plain values the view only draws: the cards, their
// words and gauges. Nothing here draws or keeps time.
namespace nativetv {
struct LiveCard {
    std::string key, icon, label; // "processor", "memory", "temperature", "network", "uptime",
                                  // "panel", "input"
    bool numeric = false;         // `number` is the numeral (it animates between readings)
    double number = 0;
    int decimals = 0;
    bool grouped = false; // 4,210
    std::string text;     // the numeral when it is a word, or a dash when the TV reports none
    bool word = false;    // `text` is a name or a duration, set in the smaller numeral
    std::string unit, caption, caption2;
    float bar = -1; // 0..1, or < 0 for none
    bool warn = false;
    bool spark = false;     // the processor's recent load runs under the caption
    std::vector<bool> days; // uptime: the last 14 days, true where the TV was up
    std::vector<std::string> chips;
};
struct LiveView {
    std::string title, subtitle;
    std::vector<LiveCard> cards;
};
// The wall clock is the TV's own, for "Since Sunday 13:07"; weekday counts from Sunday = 0.
LiveView liveView(const Json &stats, int hour, int minute, int weekday);
// The numeral of a numeric card at `value` (mid-animation values round to the card's decimals).
std::string liveNumber(const LiveCard &card, double value);
// The reading of `load` in percent, or -1 when the reply carries none.
double liveLoad(const Json &stats);
} // namespace nativetv
