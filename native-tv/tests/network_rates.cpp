#include "service/network_rates.hpp"
#include <cstdio>
#include <stdexcept>

using namespace nativetv;
using namespace nativetv::service;

namespace {
unsigned checks = 0;
void check(bool value, const char *message) {
    ++checks;
    if (!value)
        throw std::runtime_error(message);
}
} // namespace

int main() {
    try {
        SystemNetworkRates rates;
        NetworkCounters first{"eth0", 1000, 2000};
        Json value = rates.sample(0, "stats", 1500, first);
        check(!value.valid(), "The first read establishes a network baseline");

        value = rates.sample(1000, "stats", 1500, {"eth0", 2000, 3000});
        check(!value.valid(), "Reads inside the legacy cache window do not move the baseline");
        value = rates.sample(2000, "stats", 1500, {"eth0", 3000, 4000});
        check(value["rx"].number() == 1000 && value["tx"].number() == 1000,
              "Rates use the full elapsed time since the last accepted sample");
        Json held = rates.sample(3000, "stats", 1500, {"eth0", 5000, 6000});
        check(held.dump() == value.dump(), "Short reads reuse the last completed rate");
        value = rates.sample(4000, "stats", 1500, {"eth0", 5000, 3500});
        check(value["rx"].number() == 1000 && value["tx"].number() == -250,
              "A held read does not move the baseline and TX resets preserve legacy behavior");
        value = rates.sample(6000, "stats", 1500, {"eth0", 6000, 3499});
        check(value["rx"].number() == 500 && value["tx"].number() == 0,
              "Negative half rates use JavaScript-compatible rounding");

        value = rates.sample(8000, "stats", 1500, {"wlan0", 100, 100});
        check(!value.valid(), "Changing interfaces resets the rate");
        value = rates.sample(10000, "stats", 1500, {"wlan0", 50, 200});
        check(!value.valid(), "RX counter resets suppress the rate and establish a new baseline");
        value = rates.sample(12000, "stats", 1500, {"wlan0", 150, 400});
        check(value["rx"].number() == 50 && value["tx"].number() == 100,
              "Rate sampling resumes after a counter reset");

        value = rates.sample(1000, "publish", 0, {"eth0", 10, 20});
        check(!value.valid(), "Publish and API readers keep independent baselines");
        value = rates.sample(2000, "publish", 0, {"eth0", 1010, 2020});
        check(value["rx"].number() == 1000 && value["tx"].number() == 2000,
              "The publish reader can sample independently");

        std::printf("%u network rate checks passed\n", checks);
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
