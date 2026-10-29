#include "service/cpu_windows.hpp"
#include <algorithm>
#include <cmath>

namespace nativetv {
namespace service {

Json SystemCpuWindows::sample(unsigned totalCores, int64_t atMs,
                              const std::string &reader, int64_t minimumWindowMs,
                              const std::vector<CpuCoreTicks> &cpus) {
    std::lock_guard<std::mutex> lock(mutex_);
    Window &window = windows_[reader];
    if (window.hasPrevious && atMs - window.previous.atMs < minimumWindowMs) {
        Json result = Json::object();
        result.set("cores", window.last.cores);
        result.set("overall", window.last.overall);
        return result;
    }

    std::map<std::string, CpuCoreTicks> current;
    for (const CpuCoreTicks &cpu : cpus)
        current[cpu.id] = cpu;
    if (current.empty()) {
        Json result = Json::object();
        result.set("cores", window.last.cores);
        result.set("overall", window.last.overall);
        return result;
    }

    const bool hadPrevious = window.hasPrevious;
    const Previous previous = window.previous;
    window.previous.atMs = atMs;
    window.previous.cpus = std::move(current);
    window.hasPrevious = true;
    if (!hadPrevious) {
        Json result = Json::object();
        result.set("cores", window.last.cores);
        result.set("overall", window.last.overall);
        return result;
    }

    Json cores = Json::object();
    double busy = 0;
    uint64_t elapsed = 0;
    for (const auto &entry : window.previous.cpus) {
        const auto old = previous.cpus.find(entry.first);
        if (old == previous.cpus.end())
            continue;
        const CpuCoreTicks &now = entry.second;
        const CpuCoreTicks &before = old->second;
        if (now.total <= before.total || now.idle < before.idle)
            continue;
        const uint64_t deltaTotal = now.total - before.total;
        const uint64_t deltaIdle = now.idle - before.idle;
        if (deltaIdle > deltaTotal)
            continue;
        const double percent = std::max(
            0.0, std::min(100.0, std::round(100.0 *
                                            static_cast<double>(deltaTotal - deltaIdle) /
                                            static_cast<double>(deltaTotal))));
        cores.set(entry.first, Json::numeric(percent));
        busy += static_cast<double>(deltaTotal - deltaIdle);
        elapsed = std::max(elapsed, deltaTotal);
    }

    Json overall;
    const unsigned online = std::max(totalCores,
                                     static_cast<unsigned>(window.previous.cpus.size()));
    if (elapsed > 0 && online > 0) {
        const double percent = std::max(
            0.0, std::min(100.0, std::round(100.0 * busy /
                                            (static_cast<double>(elapsed) * online))));
        overall = Json::numeric(percent);
    }
    window.last.cores = cores;
    window.last.overall = overall;

    Json result = Json::object();
    result.set("cores", window.last.cores);
    result.set("overall", window.last.overall);
    return result;
}

} // namespace service
} // namespace nativetv
