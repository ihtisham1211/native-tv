#include "ui/kit/widgets.hpp"
#include <cmath>

// Static frames of the designed screens at fixed animated values (`--screen kit:home`, `kit:apps`,
// `kit:live`, `kit:settings`, `kit:setup`), for capture review against the prototype's shots.
namespace nativetv {
namespace kit {
namespace {
struct Sample {
    const char *name, *icon;
    unsigned color;
};
const Sample kApps[] = {{"Streaming", "play", 0xC0392B},  {"Music", "music", 0x8E44AD},
                        {"Games", "game", 0x2980B9},      {"Photos", "camera", 0xD35400},
                        {"Weather", "cloud", 0x2471A3},   {"News", "news", 0x566573},
                        {"Fitness", "fitness", 0x148F77}, {"Shop", "cart", 0xB03A2E},
                        {"Movies", "movie", 0x7D3C98},    {"Audio", "headphones", 0x1F618D},
                        {"Books", "book", 0x9A7D0A},      {"Radio", "network", 0xCA6F1E},
                        {"HDMI", "hdmi", 0x34495E},       {"Web", "globe", 0x2E86C1},
                        {"Home", "home", 0x5D6D7E},       {"Camera", "eye", 0x1ABC9C},
                        {"Cloud", "cloud", 0x3498DB},     {"Lock", "lock", 0x7F8C8D},
                        {"Phone", "phone", 0xE67E22},     {"Remote", "remote", 0x95A5A6},
                        {"Clock", "clock", 0x16A085},     {"Access", "access", 0x6C3483},
                        {"Info", "info", 0x2874A6},       {"Language", "language", 0xAF601A}};
const char *kPosters[] = {"Glass Season", "Tin Kingdom", "Slow Rivers", "Midnight Ledger",
                          "Lantern Row",  "Salt Road",   "Kite Hour",   "Deep Field"};

TopBar bar(int selected, bool onSettings) {
    TopBar top;
    top.avatarInitial = "I";
    top.tabs = {"Home", "Apps", "Live"};
    top.selected = selected;
    top.settingsSelected = onSettings;
    top.clock = "10:11";
    return top;
}
void aimPill(Paint paint, TopBar &top) {
    std::vector<Rect> tabs = tabRects(paint.glyphs, top.tabs);
    if (top.selected < 0)
        return;
    top.pillLeft = tabs[top.selected].x;
    top.pillRight = tabs[top.selected].x + tabs[top.selected].w;
}

void home(Paint paint) {
    TopBar top = bar(0, false);
    aimPill(paint, top);
    Hero wall;
    wall.slot[0].opacity = 1;
    wall.slot[0].tint = rgb(0x12303C);
    wall.source = "Harbor Originals";
    wall.title = "Harbor Lights";
    wall.tag = "Trending";
    wall.description =
        "A ferry pilot and a lighthouse keeper trade letters across one long winter.";
    hero(paint, wall);
    Pagination dots;
    dots.count = 6;
    dots.current = {1, 0, 0, 0, 0, 0};
    dots.progress = .4f;
    pagination(paint, dots);
    topBar(paint, top);

    Row posters;
    posters.index = 0;
    posters.label = "Top picks for you";
    row(paint, posters);
    for (int i = 0; i < 8; ++i) {
        PosterCard poster;
        poster.rest = geometry::card(0, i);
        poster.id = "card:" + std::to_string(i);
        poster.title = kPosters[i];
        poster.focus = i == 0 ? 1.0f : 0.0f;
        card(paint, poster);
    }
    Row apps;
    apps.index = 1;
    apps.label = "Your apps";
    row(paint, apps);
    for (int i = 0; i < 14; ++i) {
        AppCircle circle;
        circle.rest = geometry::appCircle(1, i);
        circle.id = "app:" + std::to_string(i);
        circle.name = kApps[i].name;
        circle.icon = kApps[i].icon;
        circle.color = rgb(kApps[i].color);
        appCircle(paint, circle);
    }
}

void apps(Paint paint) {
    ambientWall(paint, 1, rgb(0x12303C));
    TopBar top = bar(1, false);
    aimPill(paint, top);
    topBar(paint, top);
    pageHead(paint, {"Apps", "24 installed", {}});
    for (int i = 0; i < 24; ++i) {
        AppTile tile;
        tile.rest = geometry::appTile(i);
        tile.id = "tile:" + std::to_string(i);
        tile.name = kApps[i].name;
        tile.icon = kApps[i].icon;
        tile.color = rgb(kApps[i].color);
        tile.focus = i == 0 ? 1.0f : 0.0f;
        appTile(paint, tile);
    }
}

void live(Paint paint) {
    ambientWall(paint, 1, rgb(0x12303C));
    TopBar top = bar(2, false);
    aimPill(paint, top);
    topBar(paint, top);
    pageHead(paint, {"Living Room", "OLED C3 - webOS 10.3 - updated just now", {}});
    std::vector<float> samples;
    for (int i = 0; i < 30; ++i)
        samples.push_back(.45f + .3f * std::sin(i * .45f) + .1f * std::sin(i * 1.7f));
    auto stat = [&](int index, const char *icon, const char *label, const char *value,
                    const char *unit, const char *caption) {
        StatCard item;
        item.rest = geometry::statCard(index);
        item.id = "stat:" + std::to_string(index);
        item.icon = icon;
        item.label = label;
        item.value = value;
        item.unit = unit;
        item.caption = caption;
        return item;
    };
    StatCard processor = stat(0, "cpu", "Processor", "38", "%", "4 cores - 1.4 GHz");
    processor.spark = samples;
    processor.focus = 1;
    StatCard memory = stat(1, "memory", "Memory", "1.7", "of 2.9 GB", "Swap off");
    memory.bar = .58f;
    StatCard temperature = stat(2, "bolt", "Temperature", "61", "C", "Normal");
    temperature.bar = .61f;
    StatCard network = stat(3, "network", "Network", "92", "% link", "Wi-Fi - -52 dBm");
    network.bar = .92f;
    StatCard uptime = stat(4, "clock", "Uptime", "3d 4h", "", "Since Sunday 13:07");
    uptime.textValue = true;
    uptime.days.assign(14, true);
    uptime.days[5] = false;
    StatCard panel = stat(5, "display", "Panel", "4,210", "hours", "Pixel refresh in 2 days");
    panel.bar = .42f;
    StatCard input = stat(6, "hdmi", "Input", "HDMI 1", "", "Game Console");
    input.textValue = true;
    input.caption2 = "4K 120 Hz - VRR";
    for (const StatCard &item : {processor, memory, temperature, network, uptime, panel, input})
        statCard(paint, item);
}

void settings(Paint paint) {
    ambientWall(paint, 1, rgb(0x12303C));
    TopBar top = bar(-1, true);
    topBar(paint, top);
    pageHead(paint, {"Settings", "Picture, sound and the TV itself", {}});
    struct Entry {
        const char *icon, *title, *value;
        bool toggle, on;
    };
    const Entry entries[] = {{"display", "Picture mode", "Cinema", false, false},
                             {"headphones", "Sound output", "TV speakers", false, false},
                             {"bolt", "Energy saving", "", true, true},
                             {"eye", "Screen shift", "", true, false},
                             {"network", "Network", "Wi-Fi", false, false},
                             {"lock", "Privacy", "", true, true},
                             {"language", "Language", "English", false, false},
                             {"clock", "Sleep timer", "Off", false, false},
                             {"info", "About this TV", "", false, false}};
    paint.scene.pushClip(geometry::settingsViewport());
    int index = 0;
    for (const Entry &entry : entries) {
        SettingsRow item;
        item.rest = geometry::settingsRow(index);
        item.id = "setting:" + std::to_string(index);
        item.icon = entry.icon;
        item.title = entry.title;
        item.value = entry.value;
        item.toggle = entry.toggle;
        item.on = entry.on ? 1.0f : 0.0f;
        item.focus = index == 1 ? 1.0f : 0.0f;
        settingsRow(paint, item);
        ++index;
    }
    paint.scene.popClip();
    DetailPanel panel;
    panel.icon = "headphones";
    panel.title = "Sound output";
    panel.body = "Choose where the TV plays sound: its own speakers, a soundbar or headphones.";
    panel.chips = {"TV speakers", "ARC", "Bluetooth"};
    panel.action = "Change";
    detailPanel(paint, panel);
}

void setup(Paint paint) {
    Scene &scene = paint.scene;
    ambientWall(paint, 1, rgb(0x12303C));
    scene.shape({80, 176 + 9, 10, 10}, color::accent, 5);
    paint.glyphs.text(scene, {102, 176, 738, 28}, "Step 1 of 3", type::labelL, color::accent);
    paint.glyphs.text(scene, metrics::setup::title, "Name this TV", type::setupTitle,
                      color::onSurface, Node::Align::Left, 2);
    paint.glyphs.text(scene, metrics::setup::lead, "Scan the code with a phone to finish setup.",
                      type::body, color::onSurface2, Node::Align::Left, 2);
    Node &tile = scene.shape(metrics::setup::qrTile, rgb(0xFFFFFF), radius::lg);
    tile.shadow = {
        0, elevation::raised.dy, elevation::raised.blur, 0, {0, 0, 0, elevation::raised.alpha}};
    paint.glyphs.text(scene, metrics::setup::url, "192.168.1.42:3000/s/7Q4K9X", type::url,
                      color::onSurface);
    Node &field = scene.shape(metrics::setup::field, color::surface1, radius::lg);
    field.ring = {2, 0, alpha::outlineStrong};
    paint.glyphs.text(scene, {972, 250, 840, 44}, "Living Room", type::fieldText, color::onSurface);
    scene.shape({972 + paint.glyphs.measure("Living Room", type::fieldText) + 2, 252, 3, 40},
                color::accent);
    const char *rows[] = {"1234567890", "qwertyuiop", "asdfghjkl'"};
    for (int r = 0; r < 3; ++r) {
        std::vector<std::string> keys;
        for (const char *c = rows[r]; *c; ++c)
            keys.push_back(std::string(1, *c));
        std::vector<Rect> rects = geometry::keyRow(944, 340, r, keys);
        for (size_t i = 0; i < keys.size(); ++i) {
            KeyCap cap;
            cap.rest = rects[i];
            cap.id = "key:" + std::to_string(r) + ":" + std::to_string(i);
            cap.label = keys[i];
            cap.focus = r == 1 && i == 2 ? 1.0f : 0.0f;
            keyCap(paint, cap);
        }
    }
    std::vector<std::string> shiftRow = {"shift", "z", "x", "c", "v", "b", "n", "m", "delete"};
    std::vector<Rect> shiftRects = geometry::keyRow(944, 340, 3, shiftRow);
    for (size_t i = 0; i < shiftRow.size(); ++i) {
        KeyCap cap;
        cap.rest = shiftRects[i];
        cap.id = "key:3:" + std::to_string(i);
        cap.icon = shiftRow[i] == "shift" ? "shift" : shiftRow[i] == "delete" ? "backspace" : "";
        cap.label = cap.icon.empty() ? shiftRow[i] : "";
        keyCap(paint, cap);
    }
    std::vector<Rect> last = geometry::keyRow(944, 340, 4, {"space", "done"});
    KeyCap space;
    space.rest = last[0];
    space.id = "key:4:0";
    space.label = "space";
    space.hint = true;
    keyCap(paint, space);
    KeyCap done;
    done.rest = last[1];
    done.id = "key:4:1";
    done.label = "Done";
    done.icon = "check";
    done.accent = true;
    keyCap(paint, done);
}
} // namespace

bool designGallery(Paint paint, const std::string &name) {
    if (name == "home")
        home(paint);
    else if (name == "apps")
        apps(paint);
    else if (name == "live")
        live(paint);
    else if (name == "settings")
        settings(paint);
    else if (name == "setup")
        setup(paint);
    else
        return false;
    paint.scene.background = color::ink;
    return true;
}
bool designGallery(Scene &scene, const std::string &name) {
    static LegacyGlyphs glyphs;
    return designGallery(Paint{scene, glyphs}, name);
}
} // namespace kit
} // namespace nativetv
