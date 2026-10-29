#pragma once

#include "core/json.hpp"
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace nativetv {
namespace service {

struct CpuCoreTicks {
    std::string id;
    uint64_t total = 0;
    uint64_t idle = 0;
};

class SystemCpuWindows {
  public:
    Json sample(unsigned totalCores, int64_t atMs, const std::string &reader,
                int64_t minimumWindowMs, const std::vector<CpuCoreTicks> &cpus);

  private:
    struct Sample {
        Json cores = Json::object();
        Json overall;
    };
    struct Previous {
        int64_t atMs = 0;
        std::map<std::string, CpuCoreTicks> cpus;
    };
    struct Window {
        bool hasPrevious = false;
        Previous previous;
        Sample last;
    };

    std::mutex mutex_;
    std::map<std::string, Window> windows_;
};

} // namespace service
} // namespace nativetv
