#include "daemon/home_crash_breaker.hpp"
#include <cstdio>
#include <stdexcept>
#include <string>

using namespace nativetv;
using namespace nativetv::daemon;

namespace {
void check(bool value, const std::string &message) {
    if (!value) throw std::runtime_error(message);
}

void threeWithinTheWindowTrip() {
    HomeCrashBreaker breaker(3, 120000);
    check(!breaker.recordExit(0, "a"), "one exit does not trip");
    check(!breaker.recordExit(40000, "b"), "two do not");
    check(!breaker.tripped(), "and it is not tripped");
    check(breaker.recordExit(119999, "c"), "the third within 120 s trips it, on that call");
    check(breaker.tripped(), "and stays tripped");
    const Json state = breaker.state(119999);
    check(state["tripped"].boolean() && state["recentExits"].number() == 3 &&
              state["limit"].number() == 3 && state["windowSeconds"].number() == 120 &&
              state["lastExit"].string() == "c",
          "the state says why: " + state.dump());
}

void twoDoNotTrip() {
    HomeCrashBreaker breaker(3, 120000);
    check(!breaker.recordExit(0, "a") && !breaker.recordExit(1000, "b"), "two exits");
    check(!breaker.tripped(), "two in two minutes leave it alone");
    check(!breaker.state(2000)["tripped"].boolean(), "and the state agrees");
}

void exitsOutsideTheWindowAreForgotten() {
    HomeCrashBreaker breaker(3, 120000);
    check(!breaker.recordExit(0, "a"), "first");
    check(!breaker.recordExit(60000, "b"), "second");
    // The first is exactly one window old: out.
    check(!breaker.recordExit(120000, "c"), "a third whose first is a window old does not trip");
    check(breaker.recentExits(120000) == 2, "two are left in the window");
    check(!breaker.tripped(), "still ok");
    check(breaker.recordExit(130000, "d"), "the next, with b and c still inside, does");

    HomeCrashBreaker slow(3, 120000);
    for (int exits = 0; exits < 10; ++exits)
        check(!slow.recordExit(exits * 61000LL, "slow"), "one exit a minute never trips three in two");
    check(!slow.tripped(), "however many there are");
}

void aTrippedBreakerStaysTripped() {
    HomeCrashBreaker breaker(3, 120000);
    breaker.recordExit(0, "a");
    breaker.recordExit(1, "b");
    check(breaker.recordExit(2, "c"), "tripped");
    check(!breaker.recordExit(3, "d"), "later exits do not trip it again");
    check(breaker.tripped(), "it stays tripped, so Home is not taken again");
    // Long after the window: still tripped; only a reset (a restart, Home switched on) clears it.
    check(!breaker.recordExit(10 * 60 * 1000, "e") && breaker.tripped(), "time does not clear it");
    breaker.reset();
    check(!breaker.tripped() && breaker.recentExits(10 * 60 * 1000) == 0, "reset clears it and the exits");
    check(!breaker.recordExit(10 * 60 * 1000 + 1, "f") && !breaker.recordExit(10 * 60 * 1000 + 2, "g"),
          "and it counts from nothing");
    check(breaker.recordExit(10 * 60 * 1000 + 3, "h"), "three more trip it again");
}

void aLimitOfOneTripsAtOnce() {
    HomeCrashBreaker breaker(0, 0);
    check(breaker.limit() == 1 && breaker.windowMs() == 1, "out-of-range options are raised to 1");
    check(breaker.recordExit(5, "a"), "a limit of one trips on the first exit");
}
} // namespace

int main() {
    try {
        threeWithinTheWindowTrip();
        twoDoNotTrip();
        exitsOutsideTheWindowAreForgotten();
        aTrippedBreakerStaysTripped();
        aLimitOfOneTripsAtOnce();
    } catch (const std::exception &error) {
        std::fprintf(stderr, "home crash breaker test failed: %s\n", error.what());
        return 1;
    }
    std::puts("home crash breaker tests passed");
    return 0;
}
