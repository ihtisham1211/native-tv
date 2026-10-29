#include "ui/kit/entry.hpp"
#include "ui/qr_code.hpp"
#include <algorithm>
#include <cmath>

namespace nativetv {
namespace kit {
namespace {
constexpr float fieldInset = 28, caretWidth = 3, caretHeight = 40, bulletPitch = 22, bulletSize = 12;
constexpr TypeStyle capStyle = {16, 20, 600, 0};

size_t characters(const std::string &value) {
    size_t count = 0;
    for (unsigned char c : value)
        if ((c & 0xc0) != 0x80)
            ++count;
    return count;
}
} // namespace

Layer::Layer(Scene &scene, const Enter &enter, const Transform *scale)
    : scene_(scene), fade_(enter.alpha < 1), lift_(enter.dy != 0), scale_(scale != nullptr) {
    if (fade_)
        scene.pushOpacity(enter.alpha);
    if (lift_)
        scene.pushTransform(Transform::translate(0, enter.dy));
    if (scale_)
        scene.pushTransform(*scale);
}
Layer::~Layer() {
    if (scale_)
        scene_.popTransform();
    if (lift_)
        scene_.popTransform();
    if (fade_)
        scene_.popOpacity();
}

bool caretLit(double nowMs, double lastEditMs) {
    double since = std::fmod(std::max(0.0, nowMs - lastEditMs), 1100.0);
    return since < 550.0;
}

void textField(Paint paint, const TextField &field) {
    if (field.enter.alpha <= .002f)
        return;
    Scene &scene = paint.scene;
    Layer layer(scene, field.enter);
    Rect box = field.rest;
    Node &frame = scene.shape(box, color::surface1, radius::lg);
    frame.ring = {2, 0, alpha::outlineStrong};
    const float left = box.x + fieldInset, inner = box.w - 2 * fieldInset;
    const float textY = box.y + (box.h - type::fieldText.line) / 2 + 2,
                caretY = box.y + (box.h - caretHeight) / 2 + 2;
    scene.pushClip({box.x + 4, box.y, box.w - 8, box.h});
    float caretX = left;
    if (field.value.empty()) {
        paint.glyphs.text(scene, {left, textY, inner, type::fieldText.line}, field.placeholder,
                          type::fieldText, color::onSurface3);
    } else if (field.secret) {
        const size_t count = characters(field.value);
        const size_t before = characters(field.value.substr(0, field.cursor));
        float dotsWidth = count * bulletPitch;
        float shift = std::min(0.0f, inner - dotsWidth - caretWidth);
        for (size_t i = 0; i < count; ++i)
            scene.shape({left + shift + i * bulletPitch + (bulletPitch - bulletSize) / 2 - 2,
                         box.y + (box.h - bulletSize) / 2, bulletSize, bulletSize},
                        color::onSurface, bulletSize / 2);
        caretX = left + shift + before * bulletPitch;
    } else {
        float whole = paint.glyphs.measure(field.value, type::fieldText);
        float before = paint.glyphs.measure(field.value.substr(0, field.cursor), type::fieldText);
        // The text scrolls left when the caret would pass the right edge.
        float shift = std::min(0.0f, inner - caretWidth - 2 - before);
        paint.glyphs.text(scene, {left + shift, textY, whole + 24, type::fieldText.line},
                          field.value, type::fieldText, color::onSurface);
        caretX = left + shift + before + 2;
    }
    if (field.caret > .002f)
        scene.shape({caretX, caretY, caretWidth, caretHeight}, focus::withAlpha(color::accent, field.caret));
    scene.popClip();
    if (!field.id.empty())
        scene.hits.push_back({box, field.id});
}

void stepDots(Paint paint, const StepDots &dots) {
    if (dots.enter.alpha <= .002f)
        return;
    Scene &scene = paint.scene;
    Layer layer(scene, dots.enter);
    float x = dots.origin.x;
    const float y = dots.origin.y, side = 8, gap = 10;
    for (size_t i = 0; i < dots.on.size(); ++i) {
        // A lit dot is a 28 px pill while it is the latest; one that is passed stays a dot.
        bool latest = dots.on[i] > .5f && (i + 1 == dots.on.size() || dots.on[i + 1] <= .5f);
        float width = side + (latest ? 20.0f : 0.0f) * focus::clamp01(dots.on[i]);
        scene.shape({x, y, width, side},
                    focus::mix(color::surface3, color::accent, focus::clamp01(dots.on[i])), side / 2);
        x += width + gap;
    }
}

bool qrTile(Paint paint, const QrTile &tile) {
    if (tile.enter.alpha <= .002f)
        return true;
    Scene &scene = paint.scene;
    Layer layer(scene, tile.enter);
    Node &base = scene.shape(tile.rest, rgb(0xFFFFFF), radius::lg);
    base.shadow = {
        0, elevation::raised.dy, elevation::raised.blur, 0, {0, 0, 0, elevation::raised.alpha}};
    QrLayout layout = qrLayout(tile.text, tile.rest);
    if (layout.fits)
        scene.image(layout.image, layout.value);
    if (tile.sweep >= 0 && tile.sweepAlpha > .002f) {
        float y = tile.rest.y + tile.sweep * (tile.rest.h - 3);
        scene.shape({tile.rest.x, y, tile.rest.w, 3}, focus::withAlpha(color::accent, tile.sweepAlpha));
    }
    return layout.fits;
}

void hintBar(Paint paint, Rect origin, const std::vector<Hint> &hints, Enter enter) {
    if (enter.alpha <= .002f)
        return;
    Scene &scene = paint.scene;
    Layer layer(scene, enter);
    float x = origin.x;
    for (const Hint &hint : hints) {
        float capWidth = hint.cap.empty() ? 106 : paint.glyphs.measure(hint.cap, capStyle) + 22;
        scene.shape({x, origin.y, capWidth, 30}, color::surface3, radius::sm);
        if (hint.cap.empty()) {
            const char *arrows[] = {"arrowleft", "arrowup", "arrowdown", "arrowright"};
            for (int i = 0; i < 4; ++i)
                paint.glyphs.icon(scene, {x + 8 + i * 23.0f, origin.y + 6, 18, 18}, arrows[i],
                                  color::onSurface3);
        } else
            paint.glyphs.text(scene, {x, origin.y + 5, capWidth, capStyle.line}, hint.cap, capStyle,
                              color::onSurface2, Node::Align::Center);
        x += capWidth + 12;
        float labelWidth = paint.glyphs.measure(hint.label, type::labelM);
        paint.glyphs.text(scene, {x, origin.y + 3, labelWidth + 16, type::labelM.line}, hint.label,
                          type::labelM, color::onSurface3);
        x += labelWidth + 28;
    }
}
} // namespace kit
} // namespace nativetv
