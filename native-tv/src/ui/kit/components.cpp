#include "ui/kit/components.hpp"
#include "ui/kit/widgets.hpp"
#include <algorithm>

namespace nativetv {
namespace kit {
namespace {
// The previous kit sized everything in dp (2 px each) for a 960x540 design.
constexpr float dp(float value) {
    return value * 2;
}
namespace size {
constexpr float fieldHeight = dp(48);
constexpr float keyboardWidth = dp(358), keyboardHeight = dp(205), keyWidth = dp(28),
                keyHeight = dp(32), keyGap = dp(6);
constexpr float drawerWidth = dp(280), drawerInset = dp(24), drawerRadius = dp(16);
constexpr float denseRowHeight = dp(40);
constexpr float buttonScale = 1.1f, listScale = 1.05f, cardScale = 1.1f;
} // namespace size
Color withOpacity(Color value, float opacity) {
    value.a *= opacity;
    return value;
}
} // namespace

Rect scaled(Rect rect, float scale) {
    return {rect.x - rect.w * (scale - 1) / 2, rect.y - rect.h * (scale - 1) / 2, rect.w * scale,
            rect.h * scale};
}
void button(Scene &scene, Rect rect, const std::string &label, const std::string &id,
            ButtonState state, const Theme &theme) {
    bool focused = state == ButtonState::Focused;
    bool emphasized = focused || state == ButtonState::Pressed;
    bool disabled = state == ButtonState::Disabled;
    Rect painted = focused ? scaled(rect, size::buttonScale) : rect;
    Color fill = disabled ? (theme.light ? rgb(0xe1e3e1, .36f) : rgb(0x444746, .4f))
                 : emphasized ? theme.focus()
                 : theme.light ? rgb(0xe1e3e1, .9f)
                               : rgb(0x444746, .8f);
    Color ink = disabled ? (theme.light ? rgb(0x1f1f1f, .4f) : rgb(0xe3e3e3, .4f))
                : emphasized ? theme.focusText()
                : theme.light ? theme.foreground()
                              : rgb(0xe3e3e3, .8f);
    scene.rect(painted, fill, painted.h / 2);
    int height = focused ? 44 : 40;
    scene.text({painted.x + dp(16), painted.y + (painted.h - height) / 2, painted.w - dp(32),
                static_cast<float>(height)},
               label, focused ? 31 : 28, ink, height, 500, Node::Align::Center);
    if (!disabled)
        scene.hits.push_back({painted, id});
}
void chip(Scene &scene, Rect rect, const std::string &label, const std::string &id, bool focused) {
    Rect painted = focused ? scaled(rect, 1.1f) : rect;
    scene.rect(painted, focused ? color::focus : color::outline, dp(8));
    if (!focused)
        scene.rect({painted.x + 2, painted.y + 2, painted.w - 4, painted.h - 4}, color::background,
                   dp(8) - 2);
    scene.text({painted.x + 32, painted.y + (painted.h - 40) / 2, painted.w - 64, 40}, label, 28,
               focused ? color::focusText : color::secondary, 40, 500, Node::Align::Center);
    scene.hits.push_back({painted, id});
}
void field(Scene &scene, Rect rect, const std::string &label, const std::string &value,
           const std::string &support, bool active) {
    scene.text({rect.x, rect.y, rect.w, 40}, label, 28, color::secondary, 40, 500);
    Rect input{rect.x, rect.y + 52, rect.w, size::fieldHeight};
    scene.rect(input, active ? color::primary : color::outline, dp(8));
    float border = active ? 2 : 1;
    scene.rect({input.x + border, input.y + border, input.w - 2 * border, input.h - 2 * border},
               rgb(0x303030), dp(8) - border);
    scene.text({input.x + 32, input.y + 24, input.w - 64, 48}, value, 32, color::foreground, 48,
               500);
    scene.text({rect.x, input.y + input.h + 12, rect.w, 32}, support, 24, color::secondary, 32);
}
void keyboard(Scene &scene, float x, float y, const Keyboard &model, size_t row, size_t column,
              bool active) {
    scene.rect({x, y, size::keyboardWidth, size::keyboardHeight}, color::surface, dp(8));
    scene.rect({x, y + 16, size::keyboardWidth, size::keyboardHeight - 16}, color::surface);
    const auto keys = model.rows();
    for (size_t ri = 0; ri < keys.size(); ++ri) {
        float left = x + 24, top = y + 20 + static_cast<float>(ri) * 76;
        for (size_t ci = 0; ci < keys[ri].size(); ++ci) {
            const std::string &key = keys[ri][ci];
            float width = key == "space" ? 192 : key == "done" ? 124 : size::keyWidth;
            Rect rect{left, top, width, size::keyHeight};
            bool focused = active && ri == row && ci == column;
            Color fill = key == "done" ? focused ? color::primary : color::primaryContainer
                         : focused     ? color::white
                         : ri == 0 || key == "shift" || key == "delete" || key == "symbols"
                             ? color::keyAlternate
                             : color::high;
            scene.rect(rect, fill, dp(4));
            Color ink = focused ? rgb(0x1f1f1f) : color::white;
            bool symbol = key == "shift" || key == "delete" || key == "done" || key == "left" ||
                          key == "right" || key == "space";
            if (symbol)
                scene.image({left + (width - 32) / 2, top + 16, 32, 32},
                            "local:icons/key-" + key + ".svg", 0, false, ink);
            else
                scene.text({left, top + 12, width, 40}, model.label(key),
                           key == "symbols" ? 18 : 32, ink, 40, 500, Node::Align::Center);
            scene.hits.push_back({rect, "key:" + std::to_string(ri) + ":" + std::to_string(ci)});
            left += width + size::keyGap;
        }
    }
}
void listItem(Scene &scene, Rect rect, const MenuRow &item, const std::string &id,
              ListItemState state, const Theme &theme, bool dense) {
    bool focused = state == ListItemState::Focused;
    bool pressed = state == ListItemState::Pressed;
    bool selected = state == ListItemState::Selected;
    bool disabled = state == ListItemState::Disabled;
    Rect painted = focused ? scaled(rect, size::listScale) : rect;
    if (focused)
        scene.rect(painted, theme.focus(), dp(8) * size::listScale);
    else if (selected)
        scene.rect(rect, theme.selectedContainer(), dp(8));
    else if (pressed)
        scene.rect(rect, theme.focus(), dp(8));
    Color ink = (focused || pressed) ? theme.light ? color::white : color::listFocusText
                : selected       ? theme.selectedForeground()
                                 : theme.foreground();
    if (disabled)
        ink = withOpacity(ink, .4f);
    float padding = dense ? 24 : 32;
    float valueWidth = item.value.empty() ? 0 : std::min(rect.w * .38f, 300.0f);
    float icon = item.image.empty() ? 0 : 72;
    if (icon)
        scene.image({rect.x + padding, rect.y + (rect.h - 48) / 2, 48, 48}, item.image, 0, false,
                    ink);
    scene.text({rect.x + padding + icon, rect.y + (rect.h - 40) / 2,
                rect.w - 2 * padding - valueWidth - icon, 40},
               item.label, dense ? 28 : 32, ink, 40, 500);
    if (valueWidth) {
        Color valueInk = focused || pressed ? theme.light ? theme.secondary() : rgb(0x444746)
                         : selected ? color::selectedTrail
                                    : color::outline;
        if (disabled)
            valueInk = withOpacity(valueInk, .4f);
        scene.text(
            {rect.x + rect.w - padding - valueWidth, rect.y + (rect.h - 40) / 2, valueWidth, 40},
            item.value, 28, valueInk, 40, 500, Node::Align::Right);
    }
    if (!disabled)
        scene.hits.push_back({painted, id});
}
void card(Scene &scene, Rect media, const Card &item, const std::string &id, bool focused,
          bool metadata) {
    Rect painted = focused ? scaled(media, size::cardScale) : media;
    if (focused)
        scene.rect({painted.x - 4, painted.y - 4, painted.w + 8, painted.h + 8}, color::focus,
                   dp(8) + 4);
    scene.rect(painted, color::top, dp(8));
    scene.image(painted, item.image, dp(8));
    if (metadata) {
        float y = media.y + media.h + 12;
        scene.text({media.x, y, media.w, 36}, item.title, 28, color::foreground, 36, 500,
                   Node::Align::Center);
        scene.text({media.x, y + 52, media.w, 32}, item.subtitle, 24, color::secondary, 32, 400,
                   Node::Align::Center);
    }
    scene.hits.push_back({painted, id});
}
void drawer(Scene &scene, const std::string &title, const std::vector<MenuRow> &rows,
            size_t focused, const Theme &theme, const std::vector<ListItemState> &states) {
    scene.rect({0, 0, 1920, 1080}, rgb(0x000000, .6f));
    Rect panel{1920 - size::drawerInset - size::drawerWidth, size::drawerInset, size::drawerWidth,
               1080 - size::drawerInset * 2};
    scene.rect({panel.x - 6, panel.y + 2, panel.w + 12, panel.h + 8}, rgb(0x000000, .3f),
               size::drawerRadius + 6);
    scene.rect(panel, theme.container(), size::drawerRadius);
    scene.text({panel.x + 72, panel.y + 56, panel.w - 144, 56}, title, 44, theme.foreground(), 56);
    float top = panel.y + 152;
    size_t first = focused > 7 ? focused - 7 : 0;
    for (size_t i = first; i < rows.size(); ++i) {
        float height = size::denseRowHeight;
        if (top + height > panel.y + panel.h - 32)
            break;
        ListItemState state = i == focused ? ListItemState::Focused
                               : i < states.size() ? states[i]
                                                   : ListItemState::Default;
        listItem(scene, {panel.x + 40, top, panel.w - 80, height}, rows[i],
                 "menu:" + std::to_string(i), state, theme);
        top += height + 8;
    }
}
void gallery(Scene &scene, const std::string &component) {
    if (designGallery(scene, component))
        return;
    if (component == "keyboard") {
        Keyboard model;
        model.reset("", 80);
        keyboard(scene, 0, 0, model, 1, 0, true);
    } else if (component == "field")
        field(scene, {0, 0, 480, 192}, "Label", "Input", "Supporting text", true);
    else if (component == "button") {
        scene.background = color::background;
        scene.text({240, 144, 320, 52}, "Default", 32, color::secondary, 40, 500);
        scene.text({600, 144, 320, 52}, "Focused", 32, color::secondary, 40, 500);
        scene.text({960, 144, 320, 52}, "Disabled", 32, color::secondary, 40, 500);
        button(scene, {240, 224, 240, 80}, "Button", "button:default", ButtonState::Default);
        button(scene, {600, 224, 240, 80}, "Button", "button:focused", ButtonState::Focused);
        button(scene, {960, 224, 240, 80}, "Button", "button:disabled", ButtonState::Disabled);
    }
    else if (component == "drawer") {
        std::vector<MenuRow> rows(4);
        rows[0].label = "Default";
        rows[0].value = "Action";
        rows[1].label = "Selected";
        rows[1].value = "On";
        rows[2].label = "Focused";
        rows[2].value = "Action";
        rows[3].label = "Disabled";
        rows[3].value = "Unavailable";
        std::vector<ListItemState> states = {ListItemState::Default, ListItemState::Selected,
                                             ListItemState::Default, ListItemState::Disabled};
        drawer(scene, "List states", rows, 2, Theme{}, states);
    }
}
} // namespace kit
} // namespace nativetv
