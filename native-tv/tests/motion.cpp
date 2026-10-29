#include "ui/motion/motion.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace nativetv;
namespace {
void check(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
bool near(double left, double right, double tolerance) {
    return std::fabs(left - right) <= tolerance;
}
constexpr double kFrameMs = 1000.0 / 60;

// A clock the test moves by hand.
struct FakeTime {
    double ms = 0;
    MotionClock clock{[this] { return ms; }};
    void step(double deltaMs) {
        ms += deltaMs;
        clock.tick();
    }
};

// Reference for the closed form: classic RK4 on x'' = -(k x + c x') / m, x measured from the
// target, in small steps.
struct State {
    double position, velocity;
};
State integrate(const SpringParams &params, State state, double seconds) {
    auto acceleration = [&](State s) {
        return -(params.stiffness * s.position + params.damping * s.velocity) / params.mass;
    };
    const double h = 1e-4;
    for (double t = 0; t < seconds - h / 2; t += h) {
        State a = {state.velocity, acceleration(state)};
        State s2 = {state.position + a.position * h / 2, state.velocity + a.velocity * h / 2};
        State b = {s2.velocity, acceleration(s2)};
        State s3 = {state.position + b.position * h / 2, state.velocity + b.velocity * h / 2};
        State c = {s3.velocity, acceleration(s3)};
        State s4 = {state.position + c.position * h, state.velocity + c.velocity * h};
        State d = {s4.velocity, acceleration(s4)};
        state.position += h / 6 * (a.position + 2 * b.position + 2 * c.position + d.position);
        state.velocity += h / 6 * (a.velocity + 2 * b.velocity + 2 * c.velocity + d.velocity);
    }
    return state;
}

struct Regime {
    const char *name;
    SpringParams params;
};
std::vector<Regime> regimes() {
    return {{"underdamped .5", SpringParams::fromDampingRatio(230, .5)},
            {"pill .9", SpringParams::fromDampingRatio(460, .9)},
            {"row .96", SpringParams::fromDampingRatio(230, .96)},
            {"critical", SpringParams::fromDampingRatio(170, 1)},
            {"overdamped 2.5", SpringParams::fromDampingRatio(230, 2.5)},
            {"heavy critical", SpringParams::fromDampingRatio(90, 1, 3)}};
}

void testClock() {
    double real = 1000;
    MotionClock clock([&] { return real; });
    check(clock.nowMs() == 0 && clock.frame() == 0, "A new clock has not ticked");
    clock.tick();
    check(clock.deltaMs() == 0 && clock.nowMs() == 0 && clock.frame() == 1,
          "The first tick has no delta");
    real += 16;
    clock.tick();
    check(clock.deltaMs() == 16 && clock.nowMs() == 16, "A tick measures the source");
    real += 5000;
    clock.tick();
    check(clock.deltaMs() == kMaxFrameStepMs && clock.nowMs() == 16 + kMaxFrameStepMs,
          "A stall advances virtual time by at most one capped step");
    real -= 10;
    clock.tick();
    check(clock.deltaMs() == 0 && clock.frame() == 4, "Time never runs backwards");
    MotionClock steady;
    steady.tick();
    steady.tick();
    check(steady.deltaMs() >= 0 && steady.deltaMs() <= kMaxFrameStepMs,
          "The default clock reads the steady clock");
}

void testEasing() {
    const std::pair<const char *, Easing> curves[] = {{"linear", Easing::linear()},
                                                      {"out", Easing::out()},
                                                      {"emphasized", Easing::emphasized()},
                                                      {"inOut", Easing::inOut()},
                                                      {"ease", Easing::ease()},
                                                      {"standard", Easing::standard()},
                                                      {"decelerate", Easing::decelerate()},
                                                      {"accelerate", Easing::accelerate()}};
    const double points[][4] = {{0, 0, 1, 1},      {.23, 1, .32, 1},  {.2, 0, 0, 1},
                                {.77, 0, .175, 1}, {.25, .1, .25, 1}, {.4, 0, .2, 1},
                                {0, 0, .2, 1},     {.4, 0, 1, 1}};
    for (size_t index = 0; index < 8; ++index) {
        const Easing &curve = curves[index].second;
        const double *p = points[index];
        check(curve(0) == 0 && near(curve(1), 1, 1e-9), curves[index].first);
        double previous = 0;
        for (int i = 1; i <= 2000; ++i) {
            double value = curve(i / 2000.0);
            check(value >= previous - 1e-9 && value <= 1 + 1e-9,
                  "The standard curves are monotonic and stay within 0..1");
            previous = value;
        }
        // Walk the parametric curve itself: (x(t), y(t)) must satisfy y = easing(x).
        for (int i = 0; i <= 100; ++i) {
            double t = i / 100.0, u = 1 - t;
            double x = 3 * u * u * t * p[0] + 3 * u * t * t * p[2] + t * t * t;
            double y = 3 * u * u * t * p[1] + 3 * u * t * t * p[3] + t * t * t;
            check(near(curve(x), y, 2e-6), "The easing matches the bezier it names");
        }
    }
    check(near(Easing::ease()(.5), .802403, 1e-5), "CSS ease is .8024 at the half-way mark");
    check(near(Easing::out()(.1), .398124, 1e-5), "The out curve is .3981 at 10%");
    check(near(Easing(.42, 0, .58, 1)(.5), .5, 1e-7), "A symmetric curve is .5 at .5");
    check(Easing::out()(-1) == 0 && Easing::out()(7) == 1, "Progress outside 0..1 is clamped");
    check(Easing::linear()(.3) == .3, "Linear returns its input");
    // The wallpaper crossfade solver in application.cpp, which Easing::ease() replaces.
    auto easeWallpaper = [](float value) {
        float low = 0, high = 1, t = value;
        for (int i = 0; i < 12; ++i) {
            float inverse = 1 - t;
            float x = 3 * inverse * inverse * t * .25f + 3 * inverse * t * t * .25f + t * t * t;
            if (x < value)
                low = t;
            else
                high = t;
            t = (low + high) * .5f;
        }
        float inverse = 1 - t;
        return 3 * inverse * inverse * t * .1f + 3 * inverse * t * t + t * t * t;
    };
    for (int i = 0; i <= 100; ++i)
        check(near(Easing::ease()(i / 100.0), easeWallpaper(i / 100.0f), 2e-3),
              "Easing::ease() reproduces easeWallpaper");
}

void testSpringParams() {
    SpringParams row = SpringParams::fromDampingRatio(230, .96);
    check(near(row.dampingRatio(), .96, 1e-12) && row.mass == 1 && row.stiffness == 230,
          "fromDampingRatio keeps stiffness and reports its ratio");
    SpringParams response = SpringParams::fromResponse(.5, .8);
    double omega = std::sqrt(response.stiffness / response.mass);
    check(near(2 * 3.14159265358979 / omega, .5, 1e-9) && near(response.dampingRatio(), .8, 1e-12),
          "fromResponse sets the undamped period");
    check(near(SpringParams::fromDampingRatio(90, 1, 3).damping, 2 * std::sqrt(90 * 3.0), 1e-12),
          "Damping scales with mass");
}

void testSpringMatchesIntegration() {
    for (const Regime &regime : regimes()) {
        // From rest, 100 px away.
        Spring spring(regime.params, 100);
        spring.retarget(0);
        double elapsed = 0;
        for (double until : {40.0, 90.0, 170.0, 330.0, 700.0}) {
            spring.advance(until - elapsed);
            elapsed = until;
            State reference = integrate(regime.params, {100, 0}, until / 1000);
            check(near(spring.position(), reference.position, 1e-3) &&
                      near(spring.velocity(), reference.velocity, 1e-2),
                  regime.name);
        }
        // With a starting velocity, from a retarget.
        Spring moving(regime.params, 0);
        moving.retarget(100);
        moving.advance(80);
        State before = {moving.position() - 100, moving.velocity()};
        moving.retarget(40);
        moving.advance(250);
        State reference =
            integrate(regime.params, {before.position + 100 - 40, before.velocity}, .25);
        check(near(moving.position(), 40 + reference.position, 1e-3) &&
                  near(moving.velocity(), reference.velocity, 1e-2),
              regime.name);
    }
}

// Frames from the retarget until settled() is first true, at 60 Hz.
double settleTimeMs(const SpringParams &params, double distance) {
    Spring spring(params, distance);
    spring.retarget(0);
    for (int frame = 1; frame < 600; ++frame) {
        spring.advance(kFrameMs);
        if (spring.settled())
            return frame * kFrameMs;
    }
    return -1;
}
// First time on a 1 ms grid at which the reference integration satisfies the same rest test.
double referenceSettleMs(const SpringParams &params, double distance) {
    State state = {distance, 0};
    for (int ms = 1; ms < 10000; ++ms) {
        state = integrate(params, state, .001);
        if (std::fabs(state.position) < params.restDistance &&
            std::fabs(state.velocity) < params.restSpeed)
            return ms;
    }
    return -1;
}

void testSpringSettles() {
    for (const Regime &regime : regimes()) {
        double settle = settleTimeMs(regime.params, 100);
        double reference = referenceSettleMs(regime.params, 100);
        check(settle > 0 && std::fabs(settle - reference) <= 2 * kFrameMs, regime.name);
    }
    // Overshoot of the underdamped regime: exp(-pi zeta / sqrt(1 - zeta^2)) of the distance.
    Spring bouncy(SpringParams::fromDampingRatio(230, .5), 100);
    bouncy.retarget(0);
    double lowest = 0;
    for (int ms = 0; ms < 500; ++ms) {
        bouncy.advance(1);
        lowest = std::min(lowest, bouncy.position());
    }
    check(near(lowest, -100 * std::exp(-3.14159265358979 * .5 / std::sqrt(.75)), .05),
          "The underdamped spring overshoots by the textbook amount");
    for (const Regime &regime : regimes()) {
        if (regime.params.dampingRatio() < .95)
            continue;
        Spring spring(regime.params, 100);
        spring.retarget(0);
        for (int ms = 0; ms < 2000; ++ms) {
            spring.advance(1);
            check(spring.position() > -.01, "A damped spring from rest does not overshoot");
        }
    }
    // The tokens' own statements: visually on target (within 2 px) by about these times.
    auto within = [](double k, double zeta, double distance, double ms) {
        Spring spring(SpringParams::fromDampingRatio(k, zeta), distance);
        spring.retarget(0);
        spring.advance(ms);
        return std::fabs(spring.position()) < 2;
    };
    check(within(230, .96, 248, 450), "The row spring settles in about 450 ms");
    check(within(170, 1, 212, 600), "The page spring settles in about 600 ms");
    check(within(460, .9, 150, 243), "The pill's leading edge is on target by about 243 ms");
    check(!within(230, .96, 248, 250), "The row spring is not instant");
}

void testSpringSettledNeedsBothEpsilons() {
    Spring spring(SpringParams::fromDampingRatio(230, 1), 0);
    check(spring.settled(), "A spring made at rest is settled");
    spring.retarget(10);
    check(!spring.settled(), "A retargeted spring has to travel");
    spring.advance(60);
    // Target the spot it is passing through: on target but still moving fast.
    spring.retarget(spring.position());
    check(near(spring.position(), spring.target(), 1e-12) && std::fabs(spring.velocity()) > 3 &&
              !spring.settled(),
          "Being on the target while moving is not rest");
    SpringParams loose = SpringParams::fromDampingRatio(230, 1);
    loose.restDistance = 5;
    loose.restSpeed = 1000;
    Spring wide(loose, 0);
    wide.retarget(3);
    check(wide.settled(), "The rest thresholds are the spring's own");
    wide.settle();
    check(wide.position() == 3 && wide.velocity() == 0, "settle() lands on the target");
    Spring broken(SpringParams::fromDampingRatio(0, 1), 0);
    broken.retarget(5);
    broken.advance(16);
    check(broken.position() == 5, "A spring without stiffness arrives instead of producing NaN");
}

void testSpringRetargetKeepsMomentum() {
    for (const Regime &regime : regimes()) {
        Spring spring(regime.params, 0);
        spring.retarget(248);
        double target = 248;
        for (int press = 0; press < 6; ++press) {
            spring.advance(70);
            double position = spring.position(), velocity = spring.velocity();
            check(velocity > 0, regime.name);
            target += 248;
            spring.retarget(target);
            check(near(spring.position(), position, 1e-12) &&
                      near(spring.velocity(), velocity, 1e-12),
                  "Retargeting keeps position and velocity");
            // The next millisecond continues the motion rather than restarting it.
            double acceleration =
                (regime.params.stiffness * (target - position) - regime.params.damping * velocity) /
                regime.params.mass;
            spring.advance(1);
            check(near(spring.position(), position + velocity * .001, .05) &&
                      near((spring.velocity() - velocity) / .001, acceleration,
                           std::fabs(acceleration) * .05 + 1),
                  "The motion continues from the retarget");
        }
        for (int frame = 0; frame < 200; ++frame)
            spring.advance(kFrameMs);
        check(near(spring.position(), target, .2), "The spring ends on the last target");
    }
    // A new spring can take over mid-flight (the pill's two edges swap stiffness by direction).
    Spring pill(SpringParams::fromDampingRatio(250, .9), 0);
    pill.retarget(300);
    pill.advance(90);
    double position = pill.position(), velocity = pill.velocity();
    pill.retarget(300, SpringParams::fromDampingRatio(460, .9));
    check(near(pill.position(), position, 1e-12) && near(pill.velocity(), velocity, 1e-12),
          "New parameters keep position and velocity");
}

void testTween() {
    Tween linear(10, 20, 100, Easing::linear(), 50);
    check(linear.value() == 10 && !linear.finished(), "A tween starts at its from value");
    linear.advance(40);
    check(linear.value() == 10, "A tween holds its from value through the delay");
    linear.advance(20);
    check(near(linear.value(), 11, 1e-9), "A tween moves after the delay");
    linear.advance(40);
    check(near(linear.value(), 15, 1e-9) && !linear.finished(), "A linear tween is linear");
    linear.retarget(0);
    check(near(linear.value(), 15, 1e-9) && linear.target() == 0,
          "Retargeting starts from the current value");
    linear.advance(50);
    check(near(linear.value(), 7.5, 1e-9), "A retargeted tween has no delay");
    linear.advance(500);
    check(linear.value() == 0 && linear.finished(), "A tween ends exactly on its target");
    Tween out(0, 1, 100, Easing::out());
    out.advance(50);
    check(out.value() > .9, "The out curve front-loads the motion");
    Tween instant(3, 9, 0);
    check(instant.value() == 9 && instant.finished(), "A zero-length tween is already there");
    Tween later = Tween(0, 1, 100, Easing::linear(), 30).delayedBy(70);
    check(later.endMs() == 200, "delayedBy adds to the delay");
}

void testTimelineStagger() {
    Motion motion;
    FakeTime time;
    Timeline timeline;
    std::vector<std::string> ids = {"a", "b", "c", "d"};
    // The Home entrance shape: base 50 ms, 40 ms apart, items from the third on share one delay.
    timeline.stagger(ids, Tween(0, 1, 100, Easing::linear(), 50), 40, 2);
    check(timeline.size() == 4 && timeline.durationMs() == 50 + 2 * 40 + 100,
          "The timeline lasts until its last tween ends");
    timeline.play(motion);
    for (const auto &id : ids)
        check(motion.value(id) == 0 && motion.moving(id), "Played tweens wait at their from value");
    double firstMoved[4] = {-1, -1, -1, -1}, finished[4] = {-1, -1, -1, -1};
    time.step(0);
    for (int ms = 1; ms <= 400; ++ms) {
        time.step(1);
        motion.update(time.clock);
        for (size_t i = 0; i < 4; ++i) {
            if (firstMoved[i] < 0 && motion.value(ids[i]) > 0)
                firstMoved[i] = ms;
            if (finished[i] < 0 && motion.value(ids[i]) == 1)
                finished[i] = ms;
        }
    }
    check(near(firstMoved[0], 51, 1) && near(firstMoved[1], 91, 1) && near(firstMoved[2], 131, 1),
          "Each item starts one step after the previous");
    check(firstMoved[0] < firstMoved[1] && firstMoved[1] < firstMoved[2] &&
              finished[0] < finished[1] && finished[1] < finished[2],
          "Items start and finish in order");
    check(firstMoved[3] == firstMoved[2] && finished[3] == finished[2],
          "Items past the cap share the last delay");
    time.step(1);
    check(!motion.update(time.clock), "The entrance ends with nothing left moving");

    Timeline custom;
    custom.add("x", Tween(0, 1, 10, Easing::linear(), 200)).add("y", Tween(0, 1, 500));
    check(custom.durationMs() == 500 && custom.size() == 2, "add() takes each tween's own delay");
}

void testMotion() {
    FakeTime time;
    Motion motion;
    check(motion.value("nothing") == 0 && !motion.moving("nothing"), "An unknown id reads 0");
    motion.set("x", 5);
    check(motion.value("x") == 5 && !motion.moving("x"), "set() jumps");
    time.step(0);
    check(!motion.update(time.clock), "Nothing moves, nothing to draw");

    SpringParams row = SpringParams::fromDampingRatio(230, .96);
    motion.spring("x", 105, row);
    check(motion.moving("x") && motion.value("x") == 5, "A spring starts from where it is");
    int frames = 0;
    bool lastFrameMoved = false;
    double previous = 5;
    for (; frames < 200; ++frames) {
        time.step(kFrameMs);
        bool moved = motion.update(time.clock);
        if (!moved)
            break;
        lastFrameMoved = true;
        check(motion.value("x") >= previous - 1e-9, "The row spring glides one way");
        previous = motion.value("x");
    }
    check(lastFrameMoved && frames > 20 && frames < 60,
          "A spring settles after a few dozen frames");
    check(motion.value("x") == 105 && !motion.moving("x"),
          "A settled spring sits exactly on its target");
    time.step(kFrameMs);
    check(!motion.update(time.clock) && motion.value("x") == 105,
          "update() is false once everything has settled");

    // The frame that finishes is reported, so the exact end value is drawn.
    motion.tween("o", 1, 100, Easing::linear());
    bool sawFinishFrame = false;
    for (int i = 0; i < 20; ++i) {
        time.step(kFrameMs);
        bool moved = motion.update(time.clock);
        if (motion.value("o") == 1 && moved)
            sawFinishFrame = true;
        if (!moved)
            break;
    }
    check(sawFinishFrame && motion.value("o") == 1, "The frame that reaches the end is drawn");

    // Restating the target every frame must not restart a tween.
    motion.set("t", 0);
    motion.tween("t", 1, 100, Easing::linear());
    for (int i = 0; i < 4; ++i) {
        time.step(25);
        motion.update(time.clock);
        motion.tween("t", 1, 100, Easing::linear());
    }
    check(motion.value("t") == 1, "Restating a tween's target keeps it running");
    motion.tween("t", 1, 100, Easing::linear());
    check(!motion.moving("t"), "A tween to the value it already has does not start");
    motion.spring("steady", 0, row);
    time.step(kFrameMs);
    check(!motion.update(time.clock) && !motion.moving("steady"),
          "A spring to the value it already has does not start");

    // Two ticks' worth in one frame is one tick's worth.
    FakeTime a, b;
    Motion once, twice;
    for (Motion *m : {&once, &twice}) {
        m->set("v", 0);
        m->tween("v", 1, 200, Easing::linear());
    }
    a.step(0);
    b.step(0);
    a.step(40);
    b.step(40);
    once.update(a.clock);
    twice.update(b.clock);
    twice.update(b.clock);
    check(once.value("v") == twice.value("v"), "A second update() in a tick does not advance");

    // A stall costs one capped step, not the whole gap.
    FakeTime stall;
    Motion stalled;
    stalled.set("s", 0);
    stalled.tween("s", 1, 340, Easing::linear());
    stall.step(0);
    stall.step(60000);
    stalled.update(stall.clock);
    check(near(stalled.value("s"), kMaxFrameStepMs / 340, 1e-9),
          "An animation started before a stall does not skip ahead");

    // A spring retargeted through Motion matches one retargeted directly.
    FakeTime follow;
    Motion through;
    Spring direct(row, 0);
    through.spring("p", 100, row);
    direct.retarget(100);
    follow.step(0);
    for (int frame = 0; frame < 12; ++frame) {
        follow.step(kFrameMs);
        through.update(follow.clock);
        direct.advance(kFrameMs);
    }
    through.spring("p", 40, row);
    direct.retarget(40);
    check(near(through.value("p"), direct.position(), 1e-12), "Motion::spring keeps position");
    for (int frame = 0; frame < 6; ++frame) {
        follow.step(kFrameMs);
        through.update(follow.clock);
        direct.advance(kFrameMs);
        check(near(through.value("p"), direct.position(), 1e-9), "Motion::spring keeps velocity");
    }

    // A tween hands over to a spring from its current value.
    FakeTime handoff;
    Motion mixed;
    mixed.set("m", 0);
    mixed.tween("m", 10, 100, Easing::linear());
    handoff.step(0);
    for (int i = 0; i < 2; ++i) {
        handoff.step(25);
        mixed.update(handoff.clock);
    }
    mixed.spring("m", 0, row);
    check(near(mixed.value("m"), 5, 1e-9), "A spring takes over from the tween's value");

    // Ids by prefix.
    Motion pages;
    pages.set("home:a", 1);
    pages.set("home:b", 2);
    pages.set("apps:a", 3);
    pages.erasePrefix("home:");
    check(pages.value("home:a") == 0 && pages.value("home:b") == 0 && pages.value("apps:a") == 3,
          "erasePrefix drops one page's values");
}

void testAmbient() {
    FakeTime time;
    Motion motion;
    motion.set("kenburns", 1);
    motion.tween("kenburns", 1.09, 14000, Easing::linear());
    motion.ambient("kenburns");
    time.step(0);
    time.step(kFrameMs);
    check(motion.update(time.clock) && motion.ambientOnly(),
          "Ken Burns alone is ambient and still counts as active");
    motion.spring("focus", 1, SpringParams::fromDampingRatio(560, 1));
    time.step(kFrameMs);
    check(motion.update(time.clock) && !motion.ambientOnly(),
          "A focus spring makes the frame foreground");
    bool backToAmbient = false;
    for (int i = 0; i < 60; ++i) {
        time.step(kFrameMs);
        motion.update(time.clock);
        if (motion.ambientOnly())
            backToAmbient = true;
    }
    check(backToAmbient && motion.value("focus") == 1,
          "Once the focus settles only the ambient value is left");
    for (int i = 0; i < 1000; ++i) {
        time.step(kFrameMs);
        if (!motion.update(time.clock))
            break;
    }
    check(!motion.ambientOnly() && motion.value("kenburns") == 1.09,
          "Ken Burns ends on its target and the loop can go idle");
}
} // namespace

int main() {
    try {
        testClock();
        testEasing();
        testSpringParams();
        testSpringMatchesIntegration();
        testSpringSettles();
        testSpringSettledNeedsBothEpsilons();
        testSpringRetargetKeepsMomentum();
        testTween();
        testTimelineStagger();
        testMotion();
        testAmbient();
        std::puts("Native motion engine checks passed");
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
