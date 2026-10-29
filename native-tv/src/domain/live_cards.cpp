#include "domain/live_cards.hpp"
#include "domain/say.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <strings.h>

namespace nativetv {
namespace {
const char *const kDash = "\xe2\x80\x94"; // an em dash: the TV reports nothing
const char *const kDot = " \xc2\xb7 ";    // " · "
constexpr int kDays = 14;                 // the uptime strip

Json vars(std::initializer_list<std::pair<const char *, std::string>> values) {
    Json result = Json::object();
    for (const auto &value : values)
        result.set(value.first, Json::text(value.second));
    return result;
}
std::string fixed(double value, int decimals) {
    char text[48];
    std::snprintf(text, sizeof(text), "%.*f", decimals, value);
    return text;
}
std::string grouped(long long value) {
    std::string digits = std::to_string(value < 0 ? -value : value), result;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i && (digits.size() - i) % 3 == 0)
            result += ',';
        result += digits[i];
    }
    return value < 0 ? "-" + result : result;
}
float clamp01(double value) {
    return static_cast<float>(value < 0 ? 0 : value > 1 ? 1 : value);
}
std::string kilobytes(double kb) {
    return kb >= 1048576 ? fixed(kb / 1048576, 1) + " GB"
                         : std::to_string(std::lround(kb / 1024)) + " MB";
}
std::string rate(double bytesPerSecond) {
    return bytesPerSecond >= 1048576 ? fixed(bytesPerSecond / 1048576, 1) + " MB/s"
                                     : fixed(bytesPerSecond / 1024, 1) + " KB/s";
}
// "HDMI4" is written "HDMI 4" for the eye.
std::string spaced(const std::string &port) {
    std::string result;
    for (size_t i = 0; i < port.size(); ++i) {
        if (i && std::isdigit(static_cast<unsigned char>(port[i])) &&
            std::isalpha(static_cast<unsigned char>(port[i - 1])))
            result += ' ';
        result += port[i];
    }
    return result;
}
// The TV reports a flag as a boolean or as "ON".
bool flagged(const Json &value) {
    if (value.isBoolean())
        return value.boolean();
    return value.isString() && (strcasecmp(value.string().c_str(), "on") == 0 ||
                                strcasecmp(value.string().c_str(), "true") == 0);
}
std::string dayName(int day) {
    switch (((day % 7) + 7) % 7) {
    case 0:
        return say::msg("tv.live.day.sun", "Sunday");
    case 1:
        return say::msg("tv.live.day.mon", "Monday");
    case 2:
        return say::msg("tv.live.day.tue", "Tuesday");
    case 3:
        return say::msg("tv.live.day.wed", "Wednesday");
    case 4:
        return say::msg("tv.live.day.thu", "Thursday");
    case 5:
        return say::msg("tv.live.day.fri", "Friday");
    }
    return say::msg("tv.live.day.sat", "Saturday");
}
LiveCard start(const char *key, const char *icon, const std::string &label) {
    LiveCard result;
    result.key = key;
    result.icon = icon;
    result.label = label;
    return result;
}
void numeral(LiveCard &card, double value, int decimals, const std::string &unit,
             bool group = false) {
    card.numeric = true;
    card.number = value;
    card.decimals = decimals;
    card.grouped = group;
    card.unit = unit;
}
void nothing(LiveCard &card, const std::string &caption) {
    card.text = kDash;
    card.caption = caption;
}

LiveCard processor(const Json &stats) {
    LiveCard card = start("processor", "cpu", say::msg("tv.live.processor", "Processor"));
    double load = liveLoad(stats);
    if (load < 0) {
        nothing(card, say::msg("tv.live.noReading", "No reading yet"));
        return card;
    }
    numeral(card, load, 0, "%");
    card.spark = true;
    int cores = static_cast<int>(stats["coresTotal"].number(0));
    if (cores > 0)
        card.caption = say::msg("tv.live.cores", "{n} cores", vars({{"n", std::to_string(cores)}}));
    if (stats["mhz"].number(0) > 0)
        card.caption += (card.caption.empty() ? "" : kDot) +
                        say::msg("tv.live.ghz", "{ghz} GHz",
                                 vars({{"ghz", fixed(stats["mhz"].number() / 1000, 1)}}));
    return card;
}
LiveCard memory(const Json &stats) {
    LiveCard card = start("memory", "memory", say::msg("tv.live.memory", "Memory"));
    double total = stats["mem"]["total"].number(0), available = stats["mem"]["avail"].number(0);
    if (total <= 0) {
        nothing(card, say::msg("tv.live.noReading", "No reading yet"));
        return card;
    }
    double used = std::max(0.0, total - available);
    numeral(
        card, used / 1048576, 1,
        say::msg("tv.live.ofTotal", "of {total} GB", vars({{"total", fixed(total / 1048576, 1)}})));
    card.bar = clamp01(used / total);
    Json swap = stats["swap"];
    card.caption = swap["total"].number(0) > 0
                       ? say::msg("tv.live.swap", "Swap {free} free of {total}",
                                  vars({{"free", kilobytes(swap["free"].number(0))},
                                        {"total", kilobytes(swap["total"].number())}}))
                       : say::msg("tv.live.swapOff", "Swap off");
    return card;
}
LiveCard temperature(const Json &stats) {
    LiveCard card = start("temperature", "temp", say::msg("tv.live.temperature", "Temperature"));
    Json temp = stats["temp"];
    if (!temp.valid() ||
        (stats["capabilities"]["thermal"].valid() && !stats["capabilities"]["thermal"].boolean())) {
        nothing(card, say::msg("tv.live.noSensor", "No sensor on this TV"));
        return card;
    }
    double degrees = temp.number();
    numeral(card, degrees, 0,
            "\xc2\xb0"
            "C");
    card.bar = clamp01(degrees / 90);
    card.warn = degrees > 75;
    card.caption = degrees > 75   ? say::msg("tv.live.hot", "Hot")
                   : degrees > 65 ? say::msg("tv.live.warm", "Warm")
                                  : say::msg("tv.live.normal", "Normal");
    return card;
}
LiveCard network(const Json &stats) {
    LiveCard card = start("network", "wifi", say::msg("tv.live.network", "Network"));
    Json wifi = stats["wifi"];
    std::string iface = stats["netTotal"]["iface"].string();
    if (!wifi.valid()) {
        card.text = say::msg("tv.live.wired", "Wired");
        card.word = true;
        card.caption = iface.empty() ? say::msg("tv.live.ethernet", "Ethernet") : iface;
    } else {
        // Some TVs report no link figure; the signal level stands in for it.
        double quality = wifi["link"].number(0);
        if (quality <= 0 && wifi["level"].valid())
            quality = std::max(0.0, std::min(100.0, 2 * (wifi["level"].number() + 100)));
        if (quality <= 0)
            nothing(card, say::msg("tv.live.noReading", "No reading yet"));
        else {
            numeral(card, quality, 0, say::msg("tv.live.link", "% link"));
            card.bar = clamp01(quality / 100);
        }
        std::string name = stats["ssid"].string(say::msg("tv.live.wifi", "Wi-Fi"));
        if (wifi["level"].valid())
            name += kDot + say::msg("tv.live.dbm", "{level} dBm",
                                    vars({{"level", fixed(wifi["level"].number(), 0)}}));
        if (!iface.empty())
            name += kDot + iface;
        card.caption = name;
    }
    if (stats["net"].valid())
        card.caption2 = say::msg("tv.live.rates", "Down {down} · Up {up}",
                                 vars({{"down", rate(stats["net"]["rx"].number(0))},
                                       {"up", rate(stats["net"]["tx"].number(0))}}));
    return card;
}
LiveCard uptime(const Json &stats, int hour, int minute, int weekday) {
    LiveCard card = start("uptime", "timer", say::msg("tv.live.uptime", "Uptime"));
    if (!stats["uptime"].valid()) {
        nothing(card, say::msg("tv.live.noReading", "No reading yet"));
        return card;
    }
    card.word = true;
    long long seconds = static_cast<long long>(stats["uptime"].number());
    long long days = seconds / 86400, hours = seconds % 86400 / 3600, minutes = seconds % 3600 / 60;
    card.text =
        days > 0 ? say::msg("tv.live.upDays", "{d}d {h}h",
                            vars({{"d", std::to_string(days)}, {"h", std::to_string(hours)}}))
                 : say::msg("tv.live.upHours", "{h}h {m}m",
                            vars({{"h", std::to_string(hours)}, {"m", std::to_string(minutes)}}));
    if (days >= 7)
        card.caption = say::msg("tv.live.bootedDays", "Booted {n} days ago",
                                vars({{"n", std::to_string(days)}}));
    else {
        // The minute of the week the TV started, from the TV's own clock.
        long long week = 7 * 24 * 60, now = ((std::max(0, weekday) * 24LL + hour) * 60) + minute;
        long long boot = ((now - seconds / 60) % week + week) % week;
        char time[16];
        std::snprintf(time, sizeof(time), "%02lld:%02lld", boot / 60 % 24, boot % 60);
        card.caption =
            say::msg("tv.live.since", "Since {day} {time}",
                     vars({{"day", dayName(static_cast<int>(boot / 1440))}, {"time", time}}));
    }
    // The last two weeks: a segment is lit for each day the TV has been up.
    long long up = std::min<long long>(kDays, (seconds + 86399) / 86400);
    for (int i = 0; i < kDays; ++i)
        card.days.push_back(i >= kDays - up);
    return card;
}
LiveCard panel(const Json &stats) {
    LiveCard card = start("panel", "display", say::msg("tv.live.panel", "Panel"));
    Json oled = stats["oled"];
    if (oled["panel_hours"].valid()) {
        numeral(card, oled["panel_hours"].number(), 0, say::msg("tv.live.hours", "hours"), true);
        if (oled["hours_until_refresher"].valid())
            card.caption = say::msg(
                "tv.live.refresherIn", "Pixel refresher in {hours} h",
                vars({{"hours", grouped(std::lround(oled["hours_until_refresher"].number()))}}));
        double interval = oled["refresher_interval_hours"].number(0);
        if (interval > 0 && oled["hours_since_refresher"].valid())
            card.bar = clamp01(oled["hours_since_refresher"].number() / interval);
        return card;
    }
    double hertz = std::atof(stats["hardware"]["refresh_rate"].string().c_str());
    if (hertz > 0) {
        numeral(card, hertz, 0, "Hz");
        card.caption = say::msg("tv.live.refreshRate", "Panel refresh rate");
    } else
        nothing(card, say::msg("tv.live.noReading", "No reading yet"));
    return card;
}
LiveCard input(const Json &stats) {
    LiveCard card = start("input", "hdmi", say::msg("tv.live.input", "Input"));
    card.word = true;
    card.text = stats["app_name"].string(stats["powerState"]["label"].string(kDash));
    std::string title = stats["display_title"].string();
    size_t open = title.find('('), close = title.find(')');
    if (open != std::string::npos && close != std::string::npos && close > open + 1)
        card.caption = spaced(title.substr(open + 1, close - open - 1));
    int width = 0, height = 0;
    double hertz = 0;
    std::string signal = stats["signal"].string();
    if (std::sscanf(signal.c_str(), "%dx%d @ %lfHz", &width, &height, &hertz) >= 2) {
        card.chips.push_back(width >= 7680    ? "8K"
                             : width >= 3840  ? "4K"
                             : height >= 1080 ? "1080p"
                                              : "720p");
        if (hertz > 0)
            card.chips.push_back(say::msg("tv.live.hz", "{n} Hz", vars({{"n", fixed(hertz, 0)}})));
    }
    std::string range = stats["picture_engine"]["hdr_mode"].string();
    if (range == "hdr10")
        card.chips.push_back("HDR10");
    else if (range == "hlg")
        card.chips.push_back("HLG");
    else if (range == "dolbyvision" || range == "dolby_vision")
        card.chips.push_back("Dolby Vision");
    if (flagged(stats["hdmi_diag"]["vrr"]))
        card.chips.push_back("VRR");
    if (flagged(stats["hdmi_diag"]["allm"]))
        card.chips.push_back("ALLM");
    if (card.chips.size() > 4)
        card.chips.resize(4);
    return card;
}
} // namespace

double liveLoad(const Json &stats) {
    return stats["load"].isNumber() ? stats["load"].number() : -1;
}
std::string liveNumber(const LiveCard &card, double value) {
    if (card.grouped)
        return grouped(std::lround(value));
    return fixed(value, card.decimals);
}
LiveView liveView(const Json &stats, int hour, int minute, int weekday) {
    LiveView view;
    view.title = stats["device"]["name"].string(say::msg("tv.live.thisTv", "This TV"));
    std::string model = stats["device"]["model"].string();
    model = model.substr(0, model.find('.'));
    view.subtitle = model;
    if (stats["system"]["webos"].valid())
        view.subtitle += (model.empty() ? "" : kDot) +
                         say::msg("tv.live.webos", "webOS {version}",
                                  vars({{"version", stats["system"]["webos"].string()}}));
    if (!stats.valid())
        return view;
    view.cards = {processor(stats),
                  memory(stats),
                  temperature(stats),
                  network(stats),
                  uptime(stats, hour, minute, weekday),
                  panel(stats),
                  input(stats)};
    return view;
}
} // namespace nativetv
