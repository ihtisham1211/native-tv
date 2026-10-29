#pragma once

#include <cstdint>

namespace nativetv {

// The time the application sees, sampled once per loop iteration.
//
// Fixed (`--time`): frame N is at N * 1000 / 60 ms whatever the machine does,
// so two runs of a script draw identical frames.
// Real: milliseconds since the start minus the time spent paused, so a window
// that lost focus resumes where it stopped instead of jumping ahead.
class AppClock {
    static constexpr uint64_t kFixedFramesPerSecond = 60;
    bool fixed_;
    bool paused_ = false;
    uint32_t start_, pausedAt_ = 0, pausedTotal_ = 0;
    uint64_t frame_ = 0;

  public:
    AppClock(bool fixed, uint32_t realNow) : fixed_(fixed), start_(realNow) {}
    uint32_t tick(uint32_t realNow) {
        if (fixed_)
            return static_cast<uint32_t>(frame_++ * 1000 / kFixedFramesPerSecond);
        return (paused_ ? pausedAt_ : realNow) - start_ - pausedTotal_;
    }
    void pause(uint32_t realNow) {
        if (!fixed_ && !paused_) {
            paused_ = true;
            pausedAt_ = realNow;
        }
    }
    void resume(uint32_t realNow) {
        if (paused_) {
            paused_ = false;
            pausedTotal_ += realNow - pausedAt_;
        }
    }
};

} // namespace nativetv
