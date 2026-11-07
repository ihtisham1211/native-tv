#include "domain/models.hpp"
#include "domain/say.hpp"
#include <algorithm>
#include <cmath>
#include <map>

namespace nativetv {
Json fields(std::initializer_list<std::pair<std::string, Json>> values) {
    Json result = Json::object();
    for (const auto &pair : values)
        result.set(pair.first, pair.second);
    return result;
}
Command post(Operation operation, const Json &body, bool confirm) {
    Command c;
    c.operation = operation;
    c.body = body;
    c.confirm = confirm;
    return c;
}
Command control(const std::string &action, const Json &value, bool confirm) {
    return post(Operation::Control, fields({{"action", Json::text(action)}, {"value", value}}),
                confirm);
}
Command navigate(const std::string &route) {
    Command c;
    c.route = route;
    return c;
}
std::string artwork(const Json &item, bool hero) {
    std::string path = item[hero ? "backdrop" : "poster"].string();
    if (!hero && !item["backdrop"].string().empty())
        path = item["backdrop"].string();
    if (path.empty())
        return "";
    std::string size = hero ? "w1280" : item["backdrop"].string().empty() ? "w342" : "w500";
    return "tmdb:" + size + ":" + path;
}
// "Film  ·  2026  ·  ★ 6.4": the kit draws the star as an icon (the face has none). The details
// page names the kind on its source line, so it asks for the facts without it.
std::string titleFacts(const Json &item, bool withKind) {
    std::vector<std::string> parts;
    if (withKind && item["type"].string() == "movie")
        parts.push_back("Film");
    else if (withKind && item["type"].string() == "tv")
        parts.push_back("Series");
    if (!item["year"].string().empty())
        parts.push_back(item["year"].string());
    if (item["rating"].number() > 0)
        parts.push_back("★ " + compactNumber(item["rating"].number(), 1));
    if (item["runtime"].number() > 0)
        parts.push_back(compactNumber(item["runtime"].number()) + " min");
    if (item["seasons"].number() > 0)
        parts.push_back(compactNumber(item["seasons"].number()) + " seasons");
    for (const auto &genre : item["genres"].items())
        parts.push_back(genre.string());
    std::string text;
    for (const auto &part : parts) {
        if (!text.empty())
            text += "  ·  ";
        text += part;
    }
    return text;
}
std::string pictureLabel(const Json &picture) {
    if (!picture["ok"].boolean())
        return "";
    std::vector<std::string> parts;
    Json signal = picture["signal"];
    if (signal["width"].number() > 0 && signal["height"].number() > 0) {
        std::string dimensions = signal["width"].string() + " × " + signal["height"].string();
        if (signal["hz"].number() > 0) {
            std::string hz = compactNumber(signal["hz"].number(), 2);
            while (!hz.empty() && hz.back() == '0')
                hz.pop_back();
            if (!hz.empty() && hz.back() == '.')
                hz.pop_back();
            dimensions += " at " + hz + " Hz";
        }
        parts.push_back(dimensions);
        std::string hdr = signal["hdr"].string();
        if (hdr == "hdr10")
            parts.push_back("HDR10");
        if (hdr == "hlg")
            parts.push_back("HLG");
        if (hdr == "dolby_vision")
            parts.push_back("Dolby Vision");
    }
    if (!picture["mode"]["display"].string().empty())
        parts.push_back(picture["mode"]["display"].string());
    std::string text;
    for (const auto &part : parts) {
        if (!text.empty())
            text += "  ·  ";
        text += part;
    }
    return text;
}
std::vector<Tab> homeTabs(const Json &catalogue) {
    std::vector<Tab> tabs = {{"home", "Home", Operation::None}};
    // The tab needs channels: the TV's count when it gave one, else the channels added to Home. A count
    // that did not come (null) is not a count of zero channels, nor a promise of any.
    if (catalogue["layout"]["showLive"].boolean() &&
        (catalogue["sources"]["channels"].number(0) > 0 || catalogue["channels"].size() > 0))
        tabs.push_back({"live", "Live", Operation::None});
    tabs.push_back({"apps", "Apps", Operation::None});
    if (catalogue["layout"]["showInputs"].boolean(true) && catalogue["devices"].size())
        tabs.push_back({"inputs", "Inputs", Operation::None});
    return tabs;
}
// The ids hidden from `list`: "app", "dev" or "ch" are Home's rows, "appsTab" is the Apps tab's own
// apps. A catalogue from before the Apps tab had a list hides there what Home hides.
Json hiddenIds(const Json &catalogue, const std::string &list) {
    Json hidden = catalogue["layout"]["hidden"];
    return list == "appsTab" && !hidden["appsTab"].isArray() ? hidden["app"] : hidden[list];
}
namespace {
const char *const kClientApp = "com.tvweb.dashboard";
bool contains(const Json &values, const std::string &id) {
    for (const auto &value : values.items())
        if (value.string() == id)
            return true;
    return false;
}
// `withHidden`: the hidden apps (never LG Home) follow the others, flagged. `list`: the hidden list
// that applies, the row's own unless given.
CardRow catalogueRow(const std::string &kind, const Json &catalogue, const Json &guide,
                     bool withHidden = false, const std::string &list = std::string()) {
    const Json hiddenList = hiddenIds(catalogue, list.empty() ? kind : list);
    CardRow row;
    row.key = kind;
    row.title = kind == "app" ? "Apps" : kind == "dev" ? "Inputs" : "Channels";
    std::map<std::string, Json> index;
    for (const auto &item : catalogue[kind == "app"   ? "apps"
                                      : kind == "dev" ? "devices"
                                                      : "channels"]
                                .items())
        index[item["id"].string()] = item;
    std::vector<Card> hiddenCards;
    for (const auto &value : catalogue["order"][kind].items()) {
        std::string id = value.string();
        const bool hidden = contains(hiddenList, id);
        if (hidden && !withHidden)
            continue;
        if (id == "lg-home" && !catalogue["layout"]["showLgHome"].boolean(true))
            continue;
        if (!index.count(id) && id != "lg-home")
            continue;
        Json item = index[id];
        Card card;
        card.id = id;
        card.kind = kind;
        card.hidden = hidden;
        card.data = item;
        card.title = id == "lg-home" ? "LG Home" : item["title"].string(item["name"].string(id));
        if (kind == "app" && id != "lg-home")
            card.image = "app:" + id;
        if (kind == "dev")
            card.subtitle =
                item["unused"].boolean() ? "Nothing connected" : "HDMI " + item["port"].string();
        if (kind == "ch") {
            card.subtitle = item["number"].string();
            Json programme = guide[id][static_cast<size_t>(0)];
            if (programme.valid())
                card.subtitle += "  " + programme["title"].string();
        }
        // The Native Tv tile is this client: it is already in front while Home shows it, so a
        // launch would change nothing. It opens the settings the tile opens from LG's launcher.
        card.command = kind == "app" && id == kClientApp
                           ? navigate("dashboard")
                           : post(Operation::OpenHomeItem,
                                  fields({{"kind", Json::text(id == "lg-home" ? "lg" : kind)},
                                          {"id", Json::text(id)}}));
        (hidden ? hiddenCards : row.cards).push_back(card);
    }
    row.cards.insert(row.cards.end(), hiddenCards.begin(), hiddenCards.end());
    return row;
}
// The apps as one row per kind, in the owner's order of the kinds.
// `list` names the hidden apps that are left out: Home's ("app") or the Apps tab's ("appsTab").
std::vector<CardRow> appRows(const Json &catalogue, const Json &guide, const std::string &list) {
    CardRow all = catalogueRow("app", catalogue, guide, false, list);
    std::vector<CardRow> rows;
    for (const auto &kind : catalogue["kinds"].items()) {
        CardRow row;
        row.key = "app";
        row.group = kind["key"].string();
        row.title = kind["label"].string(row.group);
        for (const Card &card : all.cards)
            if (kindOf(card) == row.group)
                row.cards.push_back(card);
        if (!row.cards.empty())
            rows.push_back(row);
    }
    // A catalogue with no kinds (an older daemon's) shows the apps as one row.
    if (rows.empty())
        rows.push_back(all);
    return rows;
}
} // namespace
std::string kindOf(const Card &card) {
    return card.id == "lg-home" ? "tv" : card.data["kind"].string("other");
}
CardRow appsRow(const Json &catalogue, const Json &guide, bool withHidden, const std::string &list) {
    CardRow apps;
    apps.key = "app";
    apps.title = list == "appsTab" ? "Apps" : "Your apps";
    for (const CardRow &row : appRows(catalogue, guide, list))
        apps.cards.insert(apps.cards.end(), row.cards.begin(), row.cards.end());
    if (withHidden)
        for (const Card &card : catalogueRow("app", catalogue, guide, true, list).cards)
            if (card.hidden)
                apps.cards.push_back(card);
    return apps;
}
std::vector<std::string> orderAfterMove(const Json &order, const std::vector<std::string> &shown) {
    std::vector<std::string> result;
    size_t next = 0;
    for (const auto &value : order.items()) {
        const std::string id = value.string();
        const bool onScreen = std::find(shown.begin(), shown.end(), id) != shown.end();
        // A card on screen takes the next place held by a card on screen; the rest keep theirs.
        if (onScreen && next < shown.size())
            result.push_back(shown[next++]);
        else
            result.push_back(id);
    }
    // Cards the order did not list (an app installed since) follow.
    for (; next < shown.size(); ++next)
        if (std::find(result.begin(), result.end(), shown[next]) == result.end())
            result.push_back(shown[next]);
    return result;
}
std::vector<CardRow> homeRows(const std::string &tab, const Json &catalogue, const Json &discover,
                              const Json &guide) {
    std::vector<CardRow> result;
    if (tab == "apps") {
        for (const CardRow &row : appRows(catalogue, guide, "appsTab"))
            if (!row.cards.empty())
                result.push_back(row);
        return result;
    }
    if (tab == "inputs" || tab == "live") {
        CardRow row = catalogueRow(tab == "inputs" ? "dev" : "ch", catalogue, guide);
        if (!row.cards.empty())
            result.push_back(row);
        return result;
    }
    const std::map<std::string, std::string> names = {{"trending", "Top picks for you"},
                                                      {"movies", "Trending films this week"},
                                                      {"series", "Trending series this week"},
                                                      {"netflix", "Available on Netflix"},
                                                      {"youtube", "Trending on YouTube"}};
    std::map<std::string, Json> feedRows;
    for (const auto &source : discover["rows"].items())
        feedRows[source["key"].string()] = source;
    // The apps lead, as on Google TV, in their kinds' order when sorted by kind; the top picks
    // follow and the hero is drawn from them. The rows that open in an app (Netflix, YouTube)
    // come next, so they are not four rows down.
    CardRow apps = appsRow(catalogue, guide, false);
    if (!apps.cards.empty())
        result.push_back(apps);
    for (const auto &key : {"trending", "netflix", "youtube", "movies", "series"}) {
        if (!feedRows.count(key))
            continue;
        Json source = feedRows[key];
        CardRow row;
        row.key = source["key"].string();
        row.posters = row.key != "trending" && row.key != "youtube";
        auto name = names.find(row.key);
        row.title = name == names.end() ? source["title"].string(row.key) : name->second;
        for (const auto &item : source["items"].items()) {
            Card card;
            card.id = item["id"].string();
            card.kind = row.key == "youtube" ? "youtube" : row.key == "netflix" ? "netflix" : "title";
            card.data = item;
            card.title = item["title"].string();
            card.subtitle = titleFacts(item);
            // Every card is landscape, so the backdrop is preferred over the portrait poster.
            card.image = artwork(item);
            card.backdrop = artwork(item, true);
            if (card.kind == "netflix") {
                // OK opens the title in Netflix itself, as a YouTube card opens YouTube.
                card.command = post(Operation::OpenHomeItem,
                                    fields({{"kind", Json::text("netflix")}, {"id", item["id"]},
                                            {"type", item["type"]}}));
            } else if (card.kind == "youtube") {
                card.image = "youtube:" + card.id;
                card.command = post(Operation::OpenHomeItem,
                                    fields({{"kind", Json::text("youtube")}, {"id", item["id"]}}));
            } else
                card.command = navigate("details");
            row.cards.push_back(card);
        }
        if (!row.cards.empty())
            result.push_back(row);
    }
    return result;
}
} // namespace nativetv

namespace nativetv {
namespace {
std::string grouped(long value) {
    const std::string digits = std::to_string(value < 0 ? -value : value);
    std::string text;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i && (digits.size() - i) % 3 == 0)
            text += ',';
        text += digits[i];
    }
    return (value < 0 ? "-" : "") + text;
}
// One of the panel's cards: `value` and `unit` are the big figure, `bar` 0..1 or -1 for none.
Card panelCard(const std::string &id, const std::string &title, const std::string &value,
               const std::string &unit, const std::string &caption, double bar) {
    Card card;
    card.id = id;
    card.kind = "oled";
    card.title = title;
    card.subtitle = caption;
    card.data = fields(
        {{"value", Json::text(value)}, {"unit", Json::text(unit)}, {"bar", Json::numeric(bar)}});
    card.command = navigate("dashboard:oled");
    return card;
}
} // namespace

std::vector<CardRow> compactHomeRows(const Json &catalogue, const Json &guide, const Json &stats) {
    std::vector<CardRow> rows;
    CardRow apps = appsRow(catalogue, guide, false);
    if (!apps.cards.empty())
        rows.push_back(apps);
    if (catalogue["layout"]["showInputs"].boolean(true)) {
        CardRow inputs = catalogueRow("dev", catalogue, guide);
        if (!inputs.cards.empty())
            rows.push_back(inputs);
    }
    const Json oled = stats["oled"];
    if (!oled["panel_hours"].valid())
        return rows;
    CardRow panel;
    panel.key = "oled";
    panel.title = say::msg("tv.home.oledCare", "OLED Care");
    // The panel's hours, with how far the compensation cycle has run.
    const double interval = oled["comp_interval_hours"].number(0),
                 since = oled["hours_since_comp"].number(-1);
    const bool cycle = interval > 0 && since >= 0;
    const double done = cycle ? std::min(1.0, since / interval) : -1;
    Json percent = Json::object();
    percent.set("pct", Json::text(std::to_string(static_cast<int>(done * 100 + .5))));
    panel.cards.push_back(panelCard(
        "oled:hours", say::msg("tv.home.panelHours", "Panel hours"),
        grouped(std::lround(oled["panel_hours"].number())), "h",
        cycle ? say::msg("tv.home.compCycle", "Compensation cycle {pct}%", percent) : std::string(),
        done));
    // The Pixel Refresher: the hours until it runs, and how far on its schedule the panel is.
    if (oled["hours_until_refresher"].valid()) {
        const double every = oled["refresher_interval_hours"].number(0),
                     last = oled["hours_since_refresher"].number(-1);
        const long cycles = std::lround(oled["refresher_cycles"].number(0));
        Json count = Json::object();
        count.set("n", Json::text(std::to_string(cycles)));
        panel.cards.push_back(panelCard(
            "oled:refresher", say::msg("tv.home.refresher", "Pixel Refresher"),
            grouped(std::lround(oled["hours_until_refresher"].number())), "h",
            cycles == 1 ? say::msg("tv.home.refresherOne", "1 cycle done")
                        : say::msg("tv.home.refresherCycles", "{n} cycles done", count),
            every > 0 && last >= 0 ? std::min(1.0, last / every) : -1));
    }
    rows.push_back(panel);
    return rows;
}
} // namespace nativetv
