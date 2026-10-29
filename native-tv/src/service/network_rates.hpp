#pragma once

#include "core/json.hpp"
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace nativetv {
namespace service {

struct NetworkCounters {
    std::string iface;
    uint64_t rx = 0;
    uint64_t tx = 0;
};

class SystemNetworkRates {
  public:
    Json sample(int64_t atMs, const std::string &reader, int64_t minimumWindowMs,
                const NetworkCounters &counters);

  private:
    struct Previous {
        int64_t atMs = 0;
        NetworkCounters counters;
    };
    struct Window {
        bool hasPrevious = false;
        Previous previous;
        Json lastRate;
    };

    std::mutex mutex_;
    std::map<std::string, Window> windows_;
};

} // namespace service
} // namespace nativetv
