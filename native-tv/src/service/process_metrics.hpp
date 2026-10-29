#pragma once
#include "core/json.hpp"
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace nativetv {
namespace service {
struct Process {
    int pid = 0;
    std::string comm, args;
    uint64_t residentPages = 0, ticks = 0, started = 0;
};
struct CpuSample {
    uint64_t total = 0;
    std::map<int, Process> processes;
};
std::string processName(const Process &process);
bool readProcesses(const std::string &root, std::vector<Process> &result);
bool sampleCpu(const std::string &root, CpuSample &result);
Json memorySummary(const std::vector<Process> &processes, bool all, long pageBytes);
Json cpuSummary(const CpuSample &first, const CpuSample &second, unsigned windowMs);
Json collectMemory(const std::string &root, bool all);
Json collectCpu(const std::string &root, unsigned windowMs = 5000);
} // namespace service
} // namespace nativetv
