#include "ui/splash.hpp"
#include "ui/motion/motion.hpp"
#include <cmath>

namespace nativetv {
namespace {
constexpr float kPi = 3.14159265358979f;
constexpr Color kGreen = {0x30 / 255.0f, 0xde / 255.0f, 0x61 / 255.0f, 1};
// Where the dot sits in icons/mark.svg, as fractions of its square view box.
constexpr float kDotX = .5f, kDotY = .8797f, kDotRadius = .0902f;
constexpr float kMarkSide = 150, kDotsGap = 56, kDotSide = 10, kDotPitch = 26;
constexpr double kBreathMs = 1400;
} // namespace

void drawMark(kit::Paint paint, Rect box, float alpha) {
    paint.glyphs.icon(paint.scene, box, "mark", kit::focus::withAlpha(kit::color::onSurface, alpha));
    const float radius = kDotRadius * box.w;
    paint.scene.shape({box.x + kDotX * box.w - radius, box.y + kDotY * box.h - radius, 2 * radius,
                       2 * radius},
                      kit::focus::withAlpha(kGreen, alpha), radius);
}

void drawSplash(kit::Paint paint, double nowMs) {
    static const Easing out = Easing::out();
    Scene &scene = paint.scene;
    scene.background = kit::color::ink;
    const float fade = static_cast<float>(out(std::fmin(1.0, nowMs / kit::motion::ms::boot)));
    const Rect box = {(kit::canvas::width - kMarkSide) / 2, (kit::canvas::height - kMarkSide) / 2 - 24,
                      kMarkSide, kMarkSide};
    drawMark(paint, box, fade);
    const float dotsY = box.y + box.h + kDotsGap;
    for (int i = 0; i < 3; ++i) {
        // Each dot lags the one before it, so the breath travels left to right.
        double phase = nowMs / kBreathMs - i * .16;
        float lit = static_cast<float>(.5 + .5 * std::sin(2 * kPi * phase));
        scene.shape({kit::canvas::width / 2 + (i - 1) * kDotPitch - kDotSide / 2, dotsY, kDotSide,
                     kDotSide},
                    kit::focus::withAlpha(kit::color::accent, fade * (.2f + .8f * lit)),
                    kDotSide / 2);
    }
}
} // namespace nativetv
