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
} // namespace kit
} // namespace nativetv
