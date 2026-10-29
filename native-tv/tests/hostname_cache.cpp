// The TV's own name from DNS, cached per address (test/test-hostname.js).
#include "daemon/hostname_cache.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using nativetv::daemon::HostnameCache;
using nativetv::daemon::hostnameFromPtr;

namespace {
int failures = 0;
void check(bool condition, const char *what) {
    std::printf("%s %s\n", condition ? "  ok" : "FAIL", what);
    if (!condition) ++failures;
}
bool waitFor(const std::function<bool()> &condition, int ms = 2000) {
    for (int waited = 0; waited < ms; waited += 5) {
        if (condition()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return condition();
}

// A resolver the test answers by hand: lookups wait until answered.
struct Dns {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::string> asked;
    std::map<size_t, std::string> answers;
    std::string resolve(const std::string &address) {
        std::unique_lock<std::mutex> lock(mutex);
        const size_t index = asked.size();
        asked.push_back(address);
        changed.notify_all();
        changed.wait_for(lock, std::chrono::seconds(5), [&]() { return answers.count(index) != 0; });
        return answers.count(index) ? answers[index] : std::string();
    }
    void answer(size_t index, const std::string &name) {
        std::lock_guard<std::mutex> lock(mutex);
        answers[index] = name;
        changed.notify_all();
    }
    size_t count() {
        std::lock_guard<std::mutex> lock(mutex);
        return asked.size();
    }
    std::string address(size_t index) {
        std::lock_guard<std::mutex> lock(mutex);
        return asked[index];
    }
};
} // namespace

int main() {
    check(hostnameFromPtr("office-tv.example.net.") == "office-tv.example.net" &&
              hostnameFromPtr("tv.local") == "tv.local" && hostnameFromPtr("odd..") == "odd",
          "a PTR answer's trailing dots are dropped");

    // The cache's lookup threads outlive a block (they are detached): what they call is
    // shared and held by value, not by reference to the stack.
    const std::shared_ptr<std::atomic<long long>> clockHolder =
        std::make_shared<std::atomic<long long>>(1000000);
    std::atomic<long long> &now = *clockHolder;
    {
        const std::shared_ptr<Dns> dnsHolder = std::make_shared<Dns>();
        Dns &dns = *dnsHolder;
        HostnameCache cache([dnsHolder](const std::string &a) { return dnsHolder->resolve(a); },
                            [clockHolder]() { return clockHolder->load(); }, 3000);

        check(cache.name("").empty() && dns.count() == 0, "no address, no lookup");
        check(cache.name("192.0.2.10").empty(), "nothing known yet");
        check(cache.name("192.0.2.10").empty() && waitFor([&]() { return dns.count() == 1; }) && dns.count() == 1,
              "one lookup while one is pending");
        dns.answer(0, "tv.example.net");
        check(waitFor([&]() { return cache.name("192.0.2.10") == "tv.example.net"; }),
              "a PTR answer gives the name, without the caller waiting");
        now += 3599000;
        check(cache.name("192.0.2.10") == "tv.example.net" && dns.count() == 1, "cached within an hour");
        now += 1000;
        check(cache.name("192.0.2.10") == "tv.example.net" && waitFor([&]() { return dns.count() == 2; }),
              "a stale name is kept while it is redone, after an hour");
        dns.answer(1, "");
        check(waitFor([&]() { return cache.name("192.0.2.10").empty(); }), "NXDOMAIN: renamed out of DNS, no name");

        // No name is cached as long as a name is.
        now += 1800000;
        (void)cache.name("192.0.2.10");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        check(dns.count() == 2, "no name is not asked again within an hour");

        // A new address is looked up at once, and an answer for the old one is dropped.
        now += 1801000;
        (void)cache.name("192.0.2.10");
        check(waitFor([&]() { return dns.count() == 3; }), "after an hour the old address is asked again");
        check(cache.name("192.0.2.11").empty() && waitFor([&]() { return dns.count() == 4; }) &&
                  dns.address(3) == "192.0.2.11",
              "a new address is looked up at once, with the old lookup pending");
        dns.answer(2, "old.example.net");
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        check(cache.name("192.0.2.11").empty(), "an answer for an address the TV has left is not kept");
        dns.answer(3, "new.example.net");
        check(waitFor([&]() { return cache.name("192.0.2.11") == "new.example.net"; }), "the new address gets its name");
    }

    {   // No answer within the timeout is no name, and a late answer is dropped.
        const std::shared_ptr<Dns> dnsHolder = std::make_shared<Dns>();
        Dns &dns = *dnsHolder;
        HostnameCache cache([dnsHolder](const std::string &a) { return dnsHolder->resolve(a); },
                            [clockHolder]() { return clockHolder->load(); }, 20);
        (void)cache.name("192.0.2.4");
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        dns.answer(0, "late.example.net");
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        check(cache.name("192.0.2.4").empty() && dns.count() == 1,
              "no answer within the timeout gives no name, and a late one is dropped");
    }

    {   // The caller never waits on a lookup that is still unanswered.
        const std::shared_ptr<Dns> dnsHolder = std::make_shared<Dns>();
        Dns &dns = *dnsHolder;
        HostnameCache cache([dnsHolder](const std::string &a) { return dnsHolder->resolve(a); },
                            [clockHolder]() { return clockHolder->load(); }, 3000);
        const auto started = std::chrono::steady_clock::now();
        for (int i = 0; i < 20; ++i) (void)cache.name("192.0.2.30");
        const long long took = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        check(took < 100 && waitFor([&]() { return dns.count() == 1; }),
              "asking again and again neither waits on the lookup nor starts another");
        dns.answer(0, "x.example.net");
    }

    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    return 0;
}
