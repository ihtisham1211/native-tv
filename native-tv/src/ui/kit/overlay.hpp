#pragma once
#include "ui/kit/widgets.hpp"

// Parts the Quick drawer, Profiles, Details and the editors share beyond the Home kit: a button,
// a radio mark, the veil under a sheet and a message panel. Same rules as widgets.hpp: pure
// functions of the animated values, nothing kept.
namespace nativetv {
namespace kit {

struct PillButton {
    Rect rest;
    std::string id, label, icon;
    bool primary = false; // accent fill at rest: the action OK is for
    float focus = 0;
    Enter enter;
};
// Rest fill, focus fill with ring and shadow (scale 1.06), icon and label centred as a pair.
void pillButton(Paint paint, const PillButton &button);

// A 28 px choice mark: ring, with a dot when `on`. `rowFocus` darkens it once its row turns light.
void radio(Paint paint, Rect box, float on, float rowFocus = 0);

// Full-screen black under a drawer or sheet; one quad, nothing at alpha 0.
void veil(Paint paint, float alpha);

// What a region shows instead of its content: loading, empty or failed.
struct StatusPanel {
    Rect rest;
    std::string icon, title, message;
    bool error = false;
    double loadingMs = -1; // the frame's clock while a spinner turns in the panel, or < 0 for none
    Enter enter;
};
void statusPanel(Paint paint, const StatusPanel &panel);

// A small menu laid over a page beside the item it is for (an app's menu on Home): a panel in
// `surface3` with the `lg` radius and a raised shadow, one row per item with its icon and label in
// `labelL`. The focused row takes `focusFill` with `onFocus` ink and the row scale, as a settings row
// does. `appear` 0..1 fades it in and grows it from `scale::press` about the edge nearest its item.
struct MenuItem {
    std::string id, icon, label;
    float focus = 0;
};
struct PopupMenu {
    float x = 0, y = 0; // the panel's top left; menuSize() gives its size
    std::vector<MenuItem> items;
    float appear = 1;
    bool above = false; // the panel is above its item, so it grows up from its bottom edge
};
// The panel's width and height for `items`: as wide as its longest label needs.
Rect menuSize(Paint paint, const std::vector<MenuItem> &items);
void popupMenu(Paint paint, const PopupMenu &menu);

// A "key cap" and what it does, for the hint line at the foot of a page; returns the width used.
float keyHint(Paint paint, float x, float y, const std::string &key, const std::string &action,
             const std::string &id = "");

} // namespace kit
} // namespace nativetv
