#pragma once
#include "domain/models.hpp"
#include "ui/keyboard.hpp"
#include "ui/kit/legacy.hpp"

namespace nativetv {
namespace kit {
enum class ButtonState { Default, Focused, Pressed, Disabled };
enum class ListItemState { Default, Selected, Focused, Pressed, Disabled };

Rect scaled(Rect rect, float scale);
void button(Scene &scene, Rect rect, const std::string &label, const std::string &id,
            ButtonState state, const Theme &theme = {});
inline void button(Scene &scene, Rect rect, const std::string &label, const std::string &id,
                   bool focused, const Theme &theme = {}) {
    button(scene, rect, label, id, focused ? ButtonState::Focused : ButtonState::Default, theme);
}
void chip(Scene &scene, Rect rect, const std::string &label, const std::string &id, bool focused);
void field(Scene &scene, Rect rect, const std::string &label, const std::string &value,
           const std::string &support, bool active);
void keyboard(Scene &scene, float x, float y, const Keyboard &keyboard, size_t row, size_t column,
              bool active);
void listItem(Scene &scene, Rect rect, const MenuRow &item, const std::string &id,
              ListItemState state, const Theme &theme = {}, bool dense = true);
inline void listItem(Scene &scene, Rect rect, const MenuRow &item, const std::string &id,
                     bool focused, const Theme &theme = {}, bool dense = true) {
    listItem(scene, rect, item, id,
             focused ? ListItemState::Focused : ListItemState::Default, theme, dense);
}
void card(Scene &scene, Rect media, const Card &card, const std::string &id, bool focused,
          bool metadata = true);
void drawer(Scene &scene, const std::string &title, const std::vector<MenuRow> &rows,
            size_t focused, const Theme &theme = {},
            const std::vector<ListItemState> &states = {});
void gallery(Scene &scene, const std::string &component);
} // namespace kit
} // namespace nativetv
