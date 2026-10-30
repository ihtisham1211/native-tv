#include "ui/kit/overlay.hpp"
#include <algorithm>

namespace nativetv {
namespace kit {
namespace {
constexpr Color clear = {0, 0, 0, 0};
constexpr Color onFocusRing = {19 / 255.0f, 17 / 255.0f, 24 / 255.0f, .55f};
constexpr float pillScale = 1.06f;

// The entrance and the focus scale, applied to every node added while it lives.
class Group {
  public:
    Group(Scene &scene, const Enter &enter, const Transform *scale)
        : scene_(scene), fade_(enter.alpha < 1), lift_(enter.dy != 0), scale_(scale != nullptr) {
        if (fade_)
            scene.pushOpacity(enter.alpha);
        if (lift_)
            scene.pushTransform(Transform::translate(0, enter.dy));
        if (scale_)
            scene.pushTransform(*scale);
    }
    ~Group() {
        if (scale_)
            scene_.popTransform();
        if (lift_)
            scene_.popTransform();
        if (fade_)
            scene_.popOpacity();
    }
    Group(const Group &) = delete;
    Group &operator=(const Group &) = delete;

  private:
    Scene &scene_;
    bool fade_, lift_, scale_;
};
void text(Paint paint, Rect box, const std::string &value, const TypeStyle &style, Color ink,
          Node::Align align = Node::Align::Left, int maxLines = 1) {
    paint.glyphs.text(paint.scene, box, value, style, ink, align, maxLines);
}
} // namespace

void pillButton(Paint paint, const PillButton &button) {
    if (button.enter.alpha <= .002f)
        return;
    Scene &scene = paint.scene;
    focus::Look look = focus::lookAt(button.rest, pillScale, button.focus);
    Transform scale = focus::transformAt(button.rest, look.scale);
    Group group(scene, button.enter, look.scale != 1 ? &scale : nullptr);
    Color rest = button.primary ? color::accent : color::surface2;
    Node &base = scene.shape(button.rest, focus::mix(rest, color::focusFill, button.focus),
                             button.rest.h / 2);
    if (look.ringAlpha > 0) {
        base.ring = {0, look.ringWidth,
                     focus::withAlpha(elevation::focusRingColor, look.ringAlpha * .6f)};
        base.shadow = {0, look.shadowDy, look.shadowBlur, 0, {0, 0, 0, look.shadowAlpha}};
    }
    Color ink = focus::inkAt(button.primary ? color::onAccent : color::onSurface, color::onFocus,
                             button.focus);
    const TypeStyle &style = button.rest.h >= 64 ? type::title : type::labelL;
    float middle = button.rest.y + button.rest.h / 2;
    float labelWidth = button.label.empty() ? 0 : paint.glyphs.measure(button.label, style);
    float iconSize = button.icon.empty() ? 0 : 28, gap = iconSize && labelWidth ? 12 : 0;
    float x = button.rest.x + (button.rest.w - (iconSize + gap + labelWidth)) / 2;
    if (iconSize)
        paint.glyphs.icon(scene, {x, middle - iconSize / 2, iconSize, iconSize}, button.icon, ink);
    if (labelWidth > 0)
        text(paint, {x + iconSize + gap, middle - style.line / 2, labelWidth + 24, style.line},
             button.label, style, ink);
    if (!button.id.empty())
        scene.hits.push_back({look.rect, button.id});
}

void radio(Paint paint, Rect box, float on, float rowFocus) {
    Scene &scene = paint.scene;
    on = focus::clamp01(on);
    Node &ring = scene.shape(box, clear, box.h / 2);
    ring.ring = {2.5f, 0, focus::inkAt(color::onSurface3, onFocusRing, rowFocus)};
    if (on > 0) {
        float dot = box.h * .5f;
        scene.shape({box.x + (box.w - dot) / 2, box.y + (box.h - dot) / 2, dot, dot},
                    focus::withAlpha(focus::inkAt(color::accent, color::onAccent, rowFocus), on),
                    dot / 2);
    }
}

void veil(Paint paint, float alpha) {
    if (alpha <= .002f)
        return;
    paint.scene.shape({0, 0, canvas::width, canvas::height}, {0, 0, 0, alpha});
}

void statusPanel(Paint paint, const StatusPanel &panel) {
    if (panel.enter.alpha <= .002f)
        return;
    Scene &scene = paint.scene;
    Group group(scene, panel.enter, nullptr);
    Rect box = panel.rest;
    Node &base = scene.shape(box, color::surface1, radius::lg);
    base.ring = {1, 0, alpha::outline};
    Color tone = panel.error ? color::bad : color::accent;
    Rect well = {box.x + 36, box.y + 36, layout::panelWell, layout::panelWell};
    scene.shape(well, focus::withAlpha(tone, .16f), well.h / 2);
    paint.glyphs.icon(scene, {well.x + 17, well.y + 17, 38, 38}, panel.icon, tone);
    text(paint, {box.x + 36, well.y + well.h + 24, box.w - 72, type::headline.line}, panel.title,
         type::headline, color::onSurface);
    text(paint, {box.x + 36, well.y + well.h + 24 + type::headline.line + 8, box.w - 72,
                 type::body.line * 2},
         panel.message, type::body, color::onSurface2, Node::Align::Left, 2);
    if (panel.loadingMs >= 0)
        spinner(paint, {box.x + 36, box.y + box.h - 36 - 40, 40, 40}, panel.loadingMs, color::onSurface2);
}

float keyHint(Paint paint, float x, float y, const std::string &key, const std::string &action,
              const std::string &id) {
    Scene &scene = paint.scene;
    float cap = std::max(30.0f, paint.glyphs.measure(key, type::cap) + 24);
    scene.shape({x, y, cap, 30}, color::surface3, radius::sm);
    text(paint, {x, y + (30 - type::cap.line) / 2, cap, type::cap.line}, key, type::cap,
         color::onSurface2, Node::Align::Center);
    float width = paint.glyphs.measure(action, type::labelM);
    text(paint, {x + cap + 12, y + (30 - type::labelM.line) / 2, width + 24, type::labelM.line},
         action, type::labelM, color::onSurface3);
    float total = cap + 12 + width;
    if (!id.empty())
        scene.hits.push_back({{x, y, total, 30}, id});
    return total;
}

} // namespace kit
} // namespace nativetv
