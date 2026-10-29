#pragma once

#include <functional>
#include <memory>
#include <string>

namespace nativetv {
namespace daemon {

// The name DNS gives the TV's own address, for the stats and the HA device
// record (legacy lib/telemetry.js localHostname). A caller never waits on DNS:
// name() returns what is cached and looks a stale or missing name up behind it.
// A name, or none, is kept for an hour per address; a lookup that does not
// answer in time is none, and its late answer is dropped, as is the answer for
// an address the TV has since left.
class HostnameCache {
  public:
    using Resolve = std::function<std::string(const std::string &address)>;
    using Clock = std::function<long long()>;

    // resolve blocks and returns the name, or "" when there is none; the
    // defaults are getnameinfo, the wall clock, 3 s and an hour.
    explicit HostnameCache(Resolve resolve = Resolve(), Clock nowMs = Clock(),
                           long long timeoutMs = 3000, long long ttlMs = 3600000);

    // The cached name for address, or "" (none known, or none exists).
    std::string name(const std::string &address);

  private:
    struct State;
    std::shared_ptr<State> state_;
    Resolve resolve_;
    Clock nowMs_;
    long long timeoutMs_;
    long long ttlMs_;
};

// getnameinfo for an IPv4 address; the PTR name without its trailing dot.
std::string reverseLookup(const std::string &address);
// A PTR answer as a name: its trailing dots dropped.
std::string hostnameFromPtr(std::string answer);

} // namespace daemon
} // namespace nativetv
