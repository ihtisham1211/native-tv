#pragma once
#include "ui/kit/tokens.hpp"
#include "ui/motion/motion.hpp"

// The motion tokens as the engine's own types, for views that retarget springs and tweens.
namespace nativetv {
namespace kit {
inline SpringParams springParams(SpringTuning tuning) {
    return SpringParams::fromDampingRatio(tuning.stiffness, tuning.dampingRatio);
}
inline Easing easing(Bezier curve) {
    return Easing(curve.x1, curve.y1, curve.x2, curve.y2);
}
// How far a loading indicator has turned, 0..1, at a constant rate: one linear ambient tween per
// indicator (so the frame loop can run at half rate), started again whenever it ends. Call it
// from the frame that draws the indicator.
constexpr int spinnerTurnMs = 1400;
inline float spinnerPhase(Motion &motion, const std::string &id) {
    if (!motion.moving(id)) {
        motion.ambient(id);
        motion.play(id, Tween(0, 1, spinnerTurnMs, Easing::linear()));
    }
    return static_cast<float>(motion.value(id));
}
} // namespace kit
} // namespace nativetv
