#pragma once
#include "ui/kit/tokens.hpp"

// The previous kit's colour names and light theme, mapped onto the new palette so the screens
// not yet rebuilt on widgets.hpp keep compiling. Delete with components.* once none uses them.
namespace nativetv {
namespace kit {
namespace color {
// The names that follow the theme are references, like the tokens they alias.
static const Color &background = ink, &surface = surface1, &container = surface2, &high = surface3;
static const Color &top = surface3, &variant = pillRest;
static const Color &foreground = onSurface, &secondary = onSurface2, &outline = onSurface3;
static const Color &focus = focusFill, &focusText = onFocus, &listFocusText = onFocus;
static const Color &primary = accent;
constexpr Color primaryContainer = rgb(0x0842a0);
constexpr Color white = rgb(0xffffff);
static const Color &keyAlternate = surface3;
constexpr Color selectedContainer = rgb(0x004a77, .4f), selectedForeground = rgb(0xc2e7ff);
constexpr Color selectedTrail = rgb(0x7fcfff), lightSelectedContainer = rgb(0xc2e7ff);
constexpr Color lightSelectedForeground = rgb(0x001d35);
} // namespace color
struct Theme {
    bool light = false;
    Color background() const {
        return light ? rgb(0xf5f5f5) : color::background;
    }
    Color container() const {
        return light ? rgb(0xf0f4f9) : color::container;
    }
    Color foreground() const {
        return light ? rgb(0x1f1f1f) : color::foreground;
    }
    Color secondary() const {
        return light ? rgb(0x444746) : color::secondary;
    }
    Color focus() const {
        return light ? rgb(0x303030) : color::focus;
    }
    Color focusText() const {
        return light ? rgb(0xf2f2f2) : color::focusText;
    }
    Color selectedContainer() const {
        return light ? color::lightSelectedContainer : color::selectedContainer;
    }
    Color selectedForeground() const {
        return light ? color::lightSelectedForeground : color::selectedForeground;
    }
};
} // namespace kit
} // namespace nativetv
