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

// A "key cap" and what it does, for the hint line at the foot of a page; returns the width used.
float keyHint(Paint paint, float x, float y, const std::string &key, const std::string &action,
             const std::string &id = "");

} // namespace kit
} // namespace nativetv
