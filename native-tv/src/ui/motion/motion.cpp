#include "ui/motion/motion.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iterator>
#include <utility>

namespace nativetv {
namespace {
double steadyMilliseconds() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
} // namespace

MotionClock::MotionClock() : source_(steadyMilliseconds) {}
MotionClock::MotionClock(Source source) : source_(std::move(source)) {}
void MotionClock::tick() {
    double real = source_();
    // The first tick has nothing to measure against.
    delta_ = frame_ == 0 ? 0 : std::max(0.0, std::min(real - last_, kMaxFrameStepMs));
    last_ = real;
    now_ += delta_;
    ++frame_;
}

Easing::Easing(double x1, double y1, double x2, double y2)
    : cx_(3 * x1), cy_(3 * y1), linear_(x1 == y1 && x2 == y2) {
    bx_ = 3 * (x2 - x1) - cx_;
    ax_ = 1 - cx_ - bx_;
    by_ = 3 * (y2 - y1) - cy_;
    ay_ = 1 - cy_ - by_;
}
double Easing::operator()(double progress) const {
    progress = std::max(0.0, std::min(1.0, progress));
    if (linear_)
        return progress;
    auto sampleX = [&](double t) { return ((ax_ * t + bx_) * t + cx_) * t; };
    auto sampleY = [&](double t) { return ((ay_ * t + by_) * t + cy_) * t; };
    // Solve x(t) = progress: Newton first, bisection when the slope is too flat to trust.
    double t = progress;
    for (int i = 0; i < 8; ++i) {
        double error = sampleX(t) - progress;
        if (std::fabs(error) < 1e-7)
            return sampleY(t);
        double slope = (3 * ax_ * t + 2 * bx_) * t + cx_;
        if (std::fabs(slope) < 1e-6)
            break;
        t -= error / slope;
    }
    double low = 0, high = 1;
    t = progress;
    for (int i = 0; i < 40; ++i) {
        double error = sampleX(t) - progress;
        if (std::fabs(error) < 1e-7)
            break;
        (error < 0 ? low : high) = t;
        t = (low + high) / 2;
    }
    return sampleY(t);
}
Easing Easing::linear() {
    return Easing(0, 0, 1, 1);
}
Easing Easing::out() {
    return Easing(.23, 1, .32, 1);
}
Easing Easing::emphasized() {
    return Easing(.2, 0, 0, 1);
}
Easing Easing::inOut() {
    return Easing(.77, 0, .175, 1);
}
Easing Easing::ease() {
    return Easing(.25, .1, .25, 1);
}
Easing Easing::standard() {
    return Easing(.4, 0, .2, 1);
}
Easing Easing::decelerate() {
    return Easing(0, 0, .2, 1);
}
Easing Easing::accelerate() {
    return Easing(.4, 0, 1, 1);
}

double SpringParams::dampingRatio() const {
    return damping / (2 * std::sqrt(stiffness * mass));
}
SpringParams SpringParams::fromDampingRatio(double stiffness, double dampingRatio, double mass) {
    SpringParams params;
    params.stiffness = stiffness;
    params.mass = mass;
    params.damping = 2 * dampingRatio * std::sqrt(stiffness * mass);
    return params;
}
SpringParams SpringParams::fromResponse(double responseSeconds, double dampingRatio) {
    double omega = 2 * 3.14159265358979323846 / responseSeconds;
    return fromDampingRatio(omega * omega, dampingRatio);
}

Spring::Spring(const SpringParams &params, double position)
    : params_(params), target_(position), position_(position) {}
void Spring::advance(double dtMs) {
    if (dtMs <= 0)
        return;
    elapsed_ += dtMs / 1000;
    evaluate();
}
void Spring::retarget(double target) {
    retarget(target, params_);
}
void Spring::retarget(double target, const SpringParams &params) {
    // The displacement and velocity now become the new curve's initial conditions.
    params_ = params;
    offset_ = position_ - target;
    speed_ = velocity_;
    target_ = target;
    elapsed_ = 0;
}
void Spring::settle() {
    position_ = target_;
    velocity_ = offset_ = speed_ = elapsed_ = 0;
}
bool Spring::settled() const {
    return std::fabs(position_ - target_) < params_.restDistance &&
           std::fabs(velocity_) < params_.restSpeed;
}
void Spring::evaluate() {
    const double k = params_.stiffness, m = params_.mass, t = elapsed_;
    if (!(k > 0 && m > 0)) { // no restoring force: arrive at once rather than produce NaN
        position_ = target_;
        velocity_ = 0;
        return;
    }
    const double omega = std::sqrt(k / m);
    const double zeta = std::max(0.0, params_.damping) / (2 * std::sqrt(k * m));
    double x, v;
    if (std::fabs(zeta - 1) < 1e-6) {
        double decay = std::exp(-omega * t), c = speed_ + omega * offset_;
        x = (offset_ + c * t) * decay;
        v = (speed_ - omega * c * t) * decay;
    } else if (zeta < 1) {
        double rate = zeta * omega, damped = omega * std::sqrt(1 - zeta * zeta);
        double decay = std::exp(-rate * t), cosine = std::cos(damped * t),
               sine = std::sin(damped * t);
        x = decay * (offset_ * cosine + (speed_ + rate * offset_) / damped * sine);
        v = decay * (speed_ * cosine - (rate * speed_ + omega * omega * offset_) / damped * sine);
    } else {
        double spread = omega * std::sqrt(zeta * zeta - 1);
        double slow = -zeta * omega + spread, fast = -zeta * omega - spread;
        double fastShare = (speed_ - slow * offset_) / (fast - slow),
               slowShare = offset_ - fastShare;
        double slowDecay = std::exp(slow * t), fastDecay = std::exp(fast * t);
        x = slowShare * slowDecay + fastShare * fastDecay;
        v = slowShare * slow * slowDecay + fastShare * fast * fastDecay;
    }
    position_ = target_ + x;
    velocity_ = v;
}

Tween::Tween(double from, double to, double durationMs, Easing easing, double delayMs)
    : from_(from), to_(to), duration_(std::max(0.0, durationMs)), delay_(std::max(0.0, delayMs)),
      easing_(easing) {}
void Tween::advance(double dtMs) {
    if (dtMs > 0)
        elapsed_ += dtMs;
}
void Tween::retarget(double to) {
    from_ = value();
    to_ = to;
    delay_ = elapsed_ = 0;
}
bool Tween::finished() const {
    return elapsed_ >= endMs();
}
double Tween::value() const {
    if (finished())
        return to_;
    double time = elapsed_ - delay_;
    return time <= 0 ? from_ : from_ + (to_ - from_) * easing_(time / duration_);
}
Tween Tween::delayedBy(double extraMs) const {
    Tween copy = *this;
    copy.delay_ += extraMs;
    return copy;
}

Timeline &Timeline::add(const std::string &id, const Tween &tween) {
    steps_.push_back({id, tween});
    return *this;
}
Timeline &Timeline::stagger(const std::vector<std::string> &ids, const Tween &first, double stepMs,
                            size_t maxIndex) {
    for (size_t index = 0; index < ids.size(); ++index)
        add(ids[index], first.delayedBy((index < maxIndex ? index : maxIndex) * stepMs));
    return *this;
}
double Timeline::durationMs() const {
    double end = 0;
    for (const auto &step : steps_)
        end = std::max(end, step.tween.endMs());
    return end;
}
void Timeline::play(Motion &motion) const {
    for (const auto &step : steps_)
        motion.play(step.id, step.tween);
}

double Motion::value(const std::string &id) const {
    auto found = tracks_.find(id);
    return found == tracks_.end() ? 0 : found->second.value;
}
bool Motion::moving(const std::string &id) const {
    auto found = tracks_.find(id);
    return found != tracks_.end() && found->second.kind != Track::Kind::Rest;
}
void Motion::set(const std::string &id, double value) {
    Track &track = tracks_[id];
    track.kind = Track::Kind::Rest;
    track.value = value;
}
void Motion::spring(const std::string &id, double target, const SpringParams &params) {
    Track &track = tracks_[id];
    if (track.kind == Track::Kind::Rest && track.value == target)
        return;
    if (track.kind != Track::Kind::Spring) {
        track.spring = Spring(params, track.value);
        track.kind = Track::Kind::Spring;
    }
    track.spring.retarget(target, params);
}
void Motion::tween(const std::string &id, double to, double durationMs, const Easing &easing,
                   double delayMs) {
    Track &track = tracks_[id];
    bool running = track.kind == Track::Kind::Tween && !track.tween.finished();
    // Callers may restate the target every frame; restarting would freeze the motion.
    if (running ? track.tween.target() == to : track.value == to)
        return;
    play(id, Tween(track.value, to, durationMs, easing, delayMs));
}
void Motion::play(const std::string &id, const Tween &tween) {
    Track &track = tracks_[id];
    track.kind = Track::Kind::Tween;
    track.tween = tween;
    track.value = tween.value();
}
void Motion::ambient(const std::string &id, bool on) {
    tracks_[id].ambient = on;
}
void Motion::erasePrefix(const std::string &prefix) {
    for (auto track = tracks_.begin(); track != tracks_.end();)
        track = track->first.compare(0, prefix.size(), prefix) == 0 ? tracks_.erase(track)
                                                                    : std::next(track);
}
bool Motion::update(const MotionClock &clock) {
    // A second call in the same tick must not advance twice.
    double dt = clock.frame() == lastFrame_ ? 0 : clock.deltaMs();
    lastFrame_ = clock.frame();
    ambientMoving_ = foregroundMoving_ = false;
    // ponytail: visits every value each frame; index the moving ones if this reaches thousands.
    for (auto &entry : tracks_) {
        Track &track = entry.second;
        if (track.kind == Track::Kind::Rest)
            continue;
        (track.ambient ? ambientMoving_ : foregroundMoving_) = true;
        if (track.kind == Track::Kind::Spring) {
            track.spring.advance(dt);
            track.value = track.spring.position();
            if (track.spring.settled()) {
                track.spring.settle();
                track.value = track.spring.target();
                track.kind = Track::Kind::Rest;
            }
        } else {
            track.tween.advance(dt);
            track.value = track.tween.value();
            if (track.tween.finished())
                track.kind = Track::Kind::Rest;
        }
    }
    return ambientMoving_ || foregroundMoving_;
}

} // namespace nativetv
