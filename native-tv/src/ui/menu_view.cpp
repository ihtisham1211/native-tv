#include "ui/application.hpp"
#include "ui/kit/components.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>

namespace nativetv {
namespace {
void dashboard(Scene &scene, const ViewState &state) {
    const auto &menu = state.menu;
    kit::Theme theme{state.light};
    scene.background = theme.background();
    char clock[16];
    std::snprintf(clock, sizeof(clock), "%02d:%02d", state.hour, state.minute);
    scene.text({154, 54, 400, 50}, clock, 48, theme.secondary(), 50);
    scene.image({1800, 54, 43, 50}, "local:icons/logo.svg", 0, true, theme.foreground());
    auto tabs = dashboardTabs();
    size_t firstTab = menu.tab > 3 ? menu.tab - 3 : 0;
    float x = 122;
    for (size_t i = firstTab; i < tabs.size() && x < 1800; ++i) {
        float width = 72 + static_cast<float>(tabs[i].title.size()) * 20;
        if (i == menu.tab)
            scene.rect({x, 136, width, 72},
                       state.light ? kit::rgb(0xc2e7ff) : kit::rgb(0x004a77, .4f), 36);
        Color ink = i == menu.tab ? state.light ? kit::rgb(0x001d35) : kit::rgb(0xc2e7ff)
                                  : theme.foreground();
        if (i != menu.tab)
            ink.a = .4f;
        scene.text({x + 28, 148, width - 56, 48}, tabs[i].title, 36, ink, 48, 500,
                   Node::Align::Center);
        scene.hits.push_back({{x, 136, width, 72}, "dash:" + std::to_string(i)});
        x += width;
    }
    float top = 240;
    size_t first = menu.selected > 5 ? menu.selected - 5 : 0;
    for (size_t i = first; i < menu.rows.size(); ++i) {
        const auto &row = menu.rows[i];
        float height = row.heading ? 58 : 82;
        if (top + height > 1014)
            break;
        if (row.heading) {
            std::string label = row.label;
            std::transform(label.begin(), label.end(), label.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            scene.text({178, top + 18, 920, 32}, label, 24, theme.secondary(), 32, 500);
        } else {
            bool focused = i == menu.selected;
            Rect rect{154, top, 1000, height};
            if (focused)
                scene.rect(kit::scaled(rect, 1.05f), theme.focus(), 16);
            Color ink = focused ? theme.focusText() : theme.foreground();
            float icon = row.image.empty() ? 0 : 64;
            if (icon)
                scene.image({202, top + 17, 48, 48}, row.image, 12, true);
            if (!row.command.empty())
                scene.rect(
                    {178, top + 35, 12, 12},
                    focused ? kit::rgb(state.light ? 0xa8c7fa : 0x0b57d0) : kit::color::outline, 6);
            scene.text({214 + icon, top + 17, 540 - icon, 48}, row.label, 32, ink, 48, 500);
            scene.text({770, top + 17, 360, 48}, row.value, 32,
                       focused ? state.light ? kit::color::secondary : kit::rgb(0x444746)
                               : theme.secondary(),
                       48, 400, Node::Align::Right);
            scene.hits.push_back(
                {kit::scaled(rect, focused ? 1.05f : 1), "menu:" + std::to_string(i)});
        }
        top += height;
    }
    if (menu.selected < menu.rows.size()) {
        const auto &row = menu.rows[menu.selected];
        float body = std::min(320.0f, 40.0f * (1 + static_cast<float>(row.hint.size() / 27)));
        float title = row.label.size() > 20 ? 112 : 56;
        float height = 96 + title + 16 + body + 32 + 32;
        float y = 240 + (774 - height) / 2;
        scene.rect({1260, y, 500, height}, state.light ? kit::rgb(0xe9eef6) : theme.container(),
                   32);
        scene.text({1308, y + 48, 404, title}, row.label, 44, theme.foreground(), 56);
        scene.text({1308, y + 48 + title + 16, 404, body}, row.hint, 28, theme.secondary(), 40);
        scene.text({1308, y + height - 80, 404, 32},
                   state.busy            ? "WORKING…"
                   : row.command.empty() ? "READ ONLY"
                                         : "PRESS OK TO CHOOSE",
                   24,
                   row.command.empty() ? kit::color::outline
                   : state.light       ? kit::rgb(0x0b57d0)
                                       : kit::color::primary,
                   32, 500);
    }
}
} // namespace
void drawMenu(Scene &scene, const ViewState &state) {
    const MenuState &menu = state.menu;
    if (state.screen == Screen::Dashboard) {
        dashboard(scene, state);
        return;
    }
    Color ink = state.light ? Color{0.12f, 0.12f, 0.12f, 1} : Color{};
    Color muted = state.light ? Color{0.32f, 0.33f, 0.35f, 1} : Color{0.68f, 0.69f, 0.71f, 1};
    if (state.light)
        scene.background = {0.96f, 0.96f, 0.96f, 1};
    float left = 96, width = 1100, top = 280;
    std::string title;
    if (state.screen == Screen::Setup)
        title = setupTitle(menu.level);
    else if (state.screen == Screen::Catalog)
        title = "Homebrew applications";
    else if (state.screen == Screen::Qr)
        title = "Open on a phone";
    else
        title = menu.level;
    scene.text({left, 172, width, 86}, title, 48, ink);
    if (!menu.sub.empty())
        scene.text({left, 240, width, 36}, "Back to return  ·  " + menu.sub, 24, muted);
    size_t first = menu.selected > 4 ? menu.selected - 4 : 0;
    for (size_t i = first; i < menu.rows.size() && top < 1030; ++i) {
        const MenuRow &row = menu.rows[i];
        float height = row.heading ? 58 : 82;
        if (top + height > 950)
            break;
        bool selected = i == menu.selected && !row.heading;
        if (selected)
            scene.rect({left - 16, top, width + 32, height - 4}, {0.89f, 0.89f, 0.89f, 1}, 12);
        Color color = selected ? Color{0.12f, 0.12f, 0.12f, 1} : row.heading ? muted : ink;
        scene.text({left + 12, top + 18, width * 0.63f, height - 16}, row.label,
                   row.heading ? 24 : 30, color);
        scene.text({left + width * 0.64f, top + 20, width * 0.34f, height - 18}, row.value, 26,
                   color);
        if (!row.heading)
            scene.hits.push_back(
                {{left - 16, top, width + 32, height - 4}, "menu:" + std::to_string(i)});
        top += height;
    }
    if (menu.selected < menu.rows.size()) {
        const MenuRow &row = menu.rows[menu.selected];
        scene.text({1320, 320, 490, 100}, row.label, 36, ink);
        scene.text({1320, 440, 490, 380}, row.hint, 28, muted);
        if (!row.command.empty())
            scene.text({1320, 856, 490, 100}, state.busy ? "Working…" : "Press OK to choose", 26,
                       ink);
    }
    if (!state.qr.empty() && (state.screen == Screen::Qr ||
                              (state.screen == Screen::Setup && menu.level == "handoff"))) {
        scene.rect({1290, 370, 480, 480}, {1, 1, 1, 1}, 12);
        scene.image({1310, 390, 440, 440}, "qr:" + state.qr);
        scene.text({1290, 880, 500, 120}, "Scan with a phone camera", 28, ink);
    }
    scene.button({96, 972, 160, 72}, "Back", "back", false, state.light);
}
} // namespace nativetv
