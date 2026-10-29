#pragma once
// Animation engine for the TV client: pure C++14, no GL, no SDL.
//
// Times are milliseconds. Values are in the caller's units (px, 0..1); a spring's velocity is
// units per second. Everything advances by the frame delta of one MotionClock tick and never by
// wall time, so a stall or the sleep between idle frames cannot make an animation skip ahead.
#include <cstddef>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace nativetv {

// A hitch longer than this slows the animation down instead of skipping part of it.
constexpr double kMaxFrameStepMs = 34;

// One timestamp per frame. nowMs() is virtual time: it advances by the real time since the last
// tick, capped at kMaxFrameStepMs.
class MotionClock {
  public:
    using Source = std::function<double()>; // monotonic milliseconds
    MotionClock();                          // std::chrono::steady_clock
    explicit MotionClock(Source source);
    void tick();
    double nowMs() const {
        return now_;
    }
    double deltaMs() const {
        return delta_;
    }
    unsigned long frame() const {
        return frame_;
    }

  private:
    Source source_;
    double last_ = 0, now_ = 0, delta_ = 0;
    unsigned long frame_ = 0;
};

// CSS cubic-bezier(x1, y1, x2, y2) with x1 and x2 in 0..1: maps time progress 0..1 to eased
// progress.
class Easing {
  public:
    Easing(double x1, double y1, double x2, double y2);
    double operator()(double progress) const;
    static Easing linear();
    static Easing out();        // (.23, 1, .32, 1): things the person causes
    static Easing emphasized(); // (.2, 0, 0, 1), Material 3 standard
    static Easing inOut();      // (.77, 0, .175, 1): things that move on their own
    static Easing ease();       // CSS ease (.25, .1, .25, 1), the wallpaper crossfade
    static Easing standard();   // Material (.4, 0, .2, 1)
    static Easing decelerate(); // Material (0, 0, .2, 1)
    static Easing accelerate(); // Material (.4, 0, 1, 1)

  private:
    double ax_, bx_, cx_, ay_, by_, cy_;
    bool linear_;
};

struct SpringParams {
    double stiffness = 170, damping = 26.08, mass = 1; // critically damped
    double restDistance = .15, restSpeed = 3;          // when settled() is true
    double dampingRatio() const;
    static SpringParams fromDampingRatio(double stiffness, double dampingRatio, double mass = 1);
    // response is the period in seconds of the undamped oscillation (the SwiftUI convention).
    static SpringParams fromResponse(double responseSeconds, double dampingRatio);
};

// Damped spring in closed form (under-, critically and over-damped). Retargeting continues from
// the current position and velocity, so a held key glides instead of restarting.
class Spring {
  public:
    explicit Spring(const SpringParams &params = SpringParams(), double position = 0);
    void advance(double dtMs);
    void retarget(double target);
    void retarget(double target, const SpringParams &params); // keeps position and velocity
    void settle();                                            // jump to the target at rest
    double position() const {
        return position_;
    }
    double velocity() const {
        return velocity_;
    }
    double target() const {
        return target_;
    }
    bool settled() const;

  private:
    void evaluate();
    SpringParams params_;
    double target_, offset_ = 0, speed_ = 0, elapsed_ = 0, position_, velocity_ = 0;
};

class Tween {
  public:
    Tween(double from = 0, double to = 0, double durationMs = 0, Easing easing = Easing::out(),
          double delayMs = 0);
    void advance(double dtMs);
    // Restarts from the current value toward `to` over the same duration, without the delay.
    void retarget(double to);
    double value() const;
    double target() const {
        return to_;
    }
    bool finished() const;
    double endMs() const {
        return delay_ + duration_;
    }
    Tween delayedBy(double extraMs) const;

  private:
    double from_, to_, duration_, delay_, elapsed_ = 0;
    Easing easing_;
};

class Motion;

// A batch of delayed tweens started together, for page entrances and hero text changes.
class Timeline {
  public:
    static constexpr size_t kNoCap = static_cast<size_t>(-1);
    // The tween's own delay is its start offset.
    Timeline &add(const std::string &id, const Tween &tween);
    // Item n starts first.delay + min(n, maxIndex) * stepMs after the timeline starts.
    Timeline &stagger(const std::vector<std::string> &ids, const Tween &first, double stepMs,
                      size_t maxIndex = kNoCap);
    double durationMs() const; // when the last tween ends
    size_t size() const {
        return steps_.size();
    }
    void play(Motion &motion) const;

  private:
    struct Step {
        std::string id;
        Tween tween;
    };
    std::vector<Step> steps_;
};

// Animated values keyed by node id: "focus:card:3", "row:1:x", "pill:x0". The scene reads
// value(); handlers change targets with spring(), tween() and play(); the frame loop calls
// update() once per tick. An id nobody has set reads 0 and starts from 0.
class Motion {
  public:
    double value(const std::string &id) const;
    bool moving(const std::string &id) const;
    void set(const std::string &id, double value); // jump, cancelling any animation
    // Retargets a spring. Position and velocity carry over, also when the value was tweening
    // (a tween hands over with no velocity).
    void spring(const std::string &id, double target, const SpringParams &params);
    // Tweens from the current value. A tween already heading to `to` is left alone.
    void tween(const std::string &id, double to, double durationMs,
               const Easing &easing = Easing::out(), double delayMs = 0);
    void play(const std::string &id, const Tween &tween); // explicit start value
    // Ambient values (Ken Burns, hero progress) are slow and need no 60 Hz.
    void ambient(const std::string &id, bool on = true);
    void erasePrefix(const std::string &prefix);
    // Advances every moving value by the clock's delta once per tick. True while anything moved
    // this frame, including the frame it came to rest, so the exact end value gets drawn.
    bool update(const MotionClock &clock);
    // True when what moved this frame was all ambient: the loop can swap every other frame.
    bool ambientOnly() const {
        return ambientMoving_ && !foregroundMoving_;
    }

  private:
    struct Track {
        enum class Kind { Rest, Spring, Tween } kind = Kind::Rest;
        double value = 0;
        Spring spring;
        Tween tween;
        bool ambient = false;
    };
    std::unordered_map<std::string, Track> tracks_;
    unsigned long lastFrame_ = static_cast<unsigned long>(-1);
    bool ambientMoving_ = false, foregroundMoving_ = false;
};

} // namespace nativetv
