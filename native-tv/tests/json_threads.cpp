// One answer shared between threads, as a cached /api/stats is: copied, read, dumped
// and dropped by many request threads at once. json-c's reference counts are plain
// integers in the usual build, so unguarded this corrupts the heap ("double free").
#include "core/json.hpp"
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

using nativetv::Json;

int main() {
    const Json shared = Json::parse(
        R"({"a":{"b":[1,2,3],"c":"text"},"d":[{"x":1},{"y":2}],"e":"str","f":{"g":{"h":[true,null,1.5]}}})");
    if (!shared.valid()) {
        std::printf("json threads test failed: the fixture does not parse\n");
        return 1;
    }
    std::atomic<int> bad(0);
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&shared, &bad]() {
            for (int i = 0; i < 30000; ++i) {
                Json copy = shared;
                const Json a = copy["a"];
                const Json d = copy["d"];
                const Json b = a["b"];
                if (b.size() != 3 || a["c"].string() != "text" || d[size_t(1)]["y"].number() != 2 ||
                    copy.dump().empty() || copy["f"]["g"]["h"].size() != 3)
                    ++bad;
                Json mine = Json::object();
                mine.set("shared", copy);
                mine.set("n", Json::numeric(i));
                if (!mine["shared"].has("e") || mine.keys().size() != 2) ++bad;
            }
        });
    }
    for (std::thread &thread : threads) thread.join();
    if (bad != 0) {
        std::printf("json threads test failed: %d wrong reads\n", bad.load());
        return 1;
    }
    std::printf("json shared across threads passed\n");
    return 0;
}
