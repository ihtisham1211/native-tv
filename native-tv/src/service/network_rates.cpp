#include "service/network_rates.hpp"
#include <cmath>

namespace nativetv {
namespace service {
namespace {
double legacyRound(double value) {
    return std::floor(value + 0.5);
}
} // namespace

Json SystemNetworkRates::sample(int64_t atMs, const std::string &reader,
                                int64_t minimumWindowMs,
                                const NetworkCounters &counters) {
    std::lock_guard<std::mutex> lock(mutex_);
    Window &window = windows_[reader];
    if (window.hasPrevious && atMs - window.previous.atMs < minimumWindowMs)
        return window.lastRate;

    const Previous previous = window.previous;
    const bool hadPrevious = window.hasPrevious;
    window.previous.atMs = atMs;
    window.previous.counters = counters;
    window.hasPrevious = true;
    if (!hadPrevious || atMs <= previous.atMs ||
        counters.iface != previous.counters.iface || counters.rx < previous.counters.rx) {
        window.lastRate = Json();
        return window.lastRate;
    }

    const double seconds = static_cast<double>(atMs - previous.atMs) / 1000.0;
    if (seconds <= 0) {
        window.lastRate = Json();
        return window.lastRate;
    }
    Json rate = Json::object();
    rate.set("rx", Json::numeric(legacyRound(
                          static_cast<double>(counters.rx - previous.counters.rx) / seconds)));
    const double txDelta = static_cast<double>(counters.tx) -
                           static_cast<double>(previous.counters.tx);
    rate.set("tx", Json::numeric(legacyRound(txDelta / seconds)));
    window.lastRate = rate;
    return window.lastRate;
}

} // namespace service
} // namespace nativetv
