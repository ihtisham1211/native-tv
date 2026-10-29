#pragma once
#include "ui/kit/tokens.hpp"

// Focus 0..1 comes from the motion engine; these turn it into geometry and alphas so
// views stay thin and every component lifts the same way.
namespace nativetv {
namespace kit {
namespace focus {
constexpr float clamp01(float value) {
    return value < 0 ? 0 : value > 1 ? 1 : value;
}
constexpr float mix(float from, float to, float amount) {
    return from + (to - from) * amount;
}
constexpr Color mix(Color from, Color to, float amount) {
    return {mix(from.r, to.r, amount), mix(from.g, to.g, amount), mix(from.b, to.b, amount),
            mix(from.a, to.a, amount)};
}
constexpr Color withAlpha(Color color, float alpha) {
    return {color.r, color.g, color.b, color.a * alpha};
}

// Where the scale is anchored, as fractions of the rect.
struct Pivot {
    float x, y;
};
constexpr Pivot centre = {.5f, .5f};
constexpr Pivot leftCentre = {0, .5f};

// 1 at rest, `target` (a motion::scale token) at full focus.
constexpr float scaleAt(float target, float focus) {
    return mix(1, target, clamp01(focus));
}
constexpr Rect scaledAbout(Rect rect, float scale, Pivot pivot = centre) {
    return {rect.x - rect.w * (scale - 1) * pivot.x, rect.y - rect.h * (scale - 1) * pivot.y,
            rect.w * scale, rect.h * scale};
}

// The same scale as a scene transform, for pushing around the nodes of one component.
inline Transform transformAt(Rect rest, float scale, Pivot pivot = centre) {
    return Transform::scaleAbout(rest, pivot.x, pivot.y, scale);
}

// The ring and shadow of a focused element, faded by focus alone: the shape never
// changes, only its alpha, so the renderer can reuse one SDF pass.
struct Look {
    Rect rect;
    float scale;
    float ringWidth, ringAlpha;
    float shadowDy, shadowBlur, shadowAlpha;
};
constexpr Look lookAt(Rect rest, float target, float focus, Pivot pivot = centre,
                      ShadowRecipe shadow = elevation::focusShadow) {
    float amount = clamp01(focus), size = scaleAt(target, amount);
    return {scaledAbout(rest, size, pivot),
            size,
            elevation::focusRingWidth,
            amount,
            shadow.dy,
            shadow.blur,
            shadow.alpha * amount};
}

// Rest and focused inks cross-fade: tab labels, round-button glyphs, keys.
constexpr Color inkAt(Color rest, Color focused, float focus) {
    return mix(rest, focused, clamp01(focus));
}

// The app name under a focused circle: opacity = focus, rising 4 px as it appears.
constexpr float labelRise(float focus) {
    return -4 * (1 - clamp01(focus));
}

// Rows other than the focused one dim to 50% while focus is in the content.
constexpr float dimmedRowAlpha(float dim) {
    return mix(1, .5f, clamp01(dim));
}

// A key press dips below rest; `press` is 0 at rest, 1 at the bottom of the dip.
constexpr float keyScale(float focus, float press) {
    return mix(scaleAt(motion::scale::key, focus), motion::scale::press, clamp01(press));
}
} // namespace focus
} // namespace kit
} // namespace nativetv
