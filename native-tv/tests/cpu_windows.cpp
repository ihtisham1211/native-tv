#include "service/cpu_windows.hpp"
#include <cstdio>
#include <stdexcept>
#include <vector>

using namespace nativetv;
using namespace nativetv::service;

namespace {
unsigned checks = 0;
void check(bool value, const char *message) {
    ++checks;
    if (!value)
        throw std::runtime_error(message);
}

std::vector<CpuCoreTicks> pair(uint64_t total0, uint64_t idle0,
                               uint64_t total1, uint64_t idle1) {
    return {{"0", total0, idle0}, {"1", total1, idle1}};
}
} // namespace

int main() {
    try {
        SystemCpuWindows windows;
        Json result = windows.sample(2, 0, "stats", 5000, pair(100, 80, 100, 60));
        check(result["cores"].isObject() && result["cores"].size() == 0 &&
                  !result["overall"].isNumber(),
              "First sample establishes the baseline");

        result = windows.sample(2, 5000, "stats", 5000, pair(200, 130, 300, 160));
        check(result["cores"]["0"].number() == 50 &&
                  result["cores"]["1"].number() == 50 &&
                  result["overall"].number() == 38,
              "Per-core and overall values match the Node CPU window calculation");

        Json held = windows.sample(2, 7500, "stats", 5000, pair(260, 150, 370, 220));
        check(held.dump() == result.dump(), "Short reads reuse the last complete window");
        result = windows.sample(2, 10000, "stats", 5000, pair(300, 160, 400, 210));
        check(result["cores"]["0"].number() == 70 &&
                  result["cores"]["1"].number() == 50 &&
                  result["overall"].number() == 60,
              "A held read does not move the sampling baseline");

        Json publish = windows.sample(2, 2000, "publish", 0, pair(10, 8, 10, 6));
        check(!publish["overall"].isNumber(), "Publish reader has its own baseline");
        publish = windows.sample(2, 3000, "publish", 0, pair(110, 58, 110, 56));
        check(publish["overall"].number() == 50,
              "Home Assistant can keep a separate CPU sampling window");

        result = windows.sample(4, 11000, "stats", 0,
                                {{"0", 400, 250,}, {"2", 200, 100}});
        check(result["cores"].has("0") && !result["cores"].has("1") &&
                  result["cores"]["0"].number() == 10 &&
                  !result["cores"].has("2") && result["overall"].number() == 3,
              "Newly online cores are skipped and total core count remains the denominator");
        result = windows.sample(4, 12000, "stats", 0,
                                {{"0", 10, 5}, {"2", 220, 110}});
        check(!result["cores"].has("0") && result["cores"]["2"].number() == 50,
              "Reset counters are skipped while a newly sampled core becomes measurable");

        Json last = windows.sample(4, 13000, "stats", 0, {});
        check(last.dump() == result.dump(), "Empty reads preserve the last valid sample");
        std::printf("%u CPU window checks passed\n", checks);
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
