#include "service/process_metrics.hpp"
#include "service/kernel_files.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <dirent.h>
#include <limits>
#include <sstream>
#include <thread>
#include <unistd.h>

namespace nativetv {
namespace service {
namespace {
bool positivePid(const char *value, int &pid) {
    if (!*value)
        return false;
    uint64_t number = 0;
    for (const char *p = value; *p; ++p) {
        if (*p < '0' || *p > '9')
            return false;
        number = number * 10 + (*p - '0');
        if (number > static_cast<uint64_t>(std::numeric_limits<int>::max()))
            return false;
    }
    pid = static_cast<int>(number);
    return pid > 0;
}
bool statProcess(const std::string &base, Process &process, bool cpu) {
    std::string stat;
    if (!readKernelFile(base + "/stat", stat))
        return false;
    size_t open = stat.find('('), close = stat.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close <= open)
        return false;
    process.comm = stat.substr(open + 1, close - open - 1);
    if (cpu) {
        std::istringstream fields(stat.substr(close + 1));
        std::vector<std::string> values;
        for (std::string field; fields >> field;)
            values.push_back(field);
        uint64_t user, system;
        if (values.size() < 20 || !parseUnsigned(values[11], user) ||
            !parseUnsigned(values[12], system) || !parseUnsigned(values[19], process.started) ||
            user > UINT64_MAX - system)
            return false;
        process.ticks = user + system;
    } else {
        std::string statm, size, resident;
        if (!readKernelFile(base + "/statm", statm))
            return false;
        std::istringstream fields(statm);
        if (!(fields >> size >> resident) || !parseUnsigned(resident, process.residentPages))
            return false;
    }
    readKernelFile(base + "/cmdline", process.args);
    while (!process.args.empty() && process.args.back() == '\0')
        process.args.pop_back();
    std::replace(process.args.begin(), process.args.end(), '\0', ' ');
    if (process.args.empty() && !cpu)
        process.args = "[" + process.comm + "]";
    return true;
}
bool list(const std::string &root, bool cpu, std::vector<Process> &processes) {
    DIR *directory = opendir(root.c_str());
    if (!directory)
        return false;
    processes.clear();
    while (auto *entry = readdir(directory)) {
        Process process;
        if (positivePid(entry->d_name, process.pid) &&
            statProcess(root + "/" + entry->d_name, process, cpu))
            processes.push_back(std::move(process));
    }
    closedir(directory);
    // Numeric order also makes equal-size rows deterministic across procfs listings.
    std::sort(processes.begin(), processes.end(),
              [](const Process &a, const Process &b) { return a.pid < b.pid; });
    return !processes.empty();
}
Json error(const std::string &message) {
    Json value = Json::object();
    value.set("ok", Json::flag(false));
    value.set("error", Json::text(message));
    return value;
}
double tenth(double value) {
    return std::round(value * 10) / 10;
}
} // namespace

std::string processName(const Process &process) {
    std::istringstream command(process.args);
    std::string bin;
    command >> bin;
    auto slash = bin.rfind('/');
    if (slash != std::string::npos)
        bin = bin.substr(slash + 1);
    if (bin == "WebAppMgr") {
        const std::string applications = "/usr/palm/applications/";
        size_t at = process.args.find(applications);
        if (at != std::string::npos) {
            at += applications.size();
            auto app = process.args.substr(at, process.args.find_first_of("/ \t\r\n", at) - at);
            const std::string prefix = "com.webos.app.";
            if (app.compare(0, prefix.size(), prefix) == 0)
                app.erase(0, prefix.size());
            if (!app.empty())
                return "WebAppMgr (" + app + ")";
        }
        const std::string type = "--type=";
        at = process.args.find(type);
        std::string role = "browser";
        if (at != std::string::npos) {
            at += type.size();
            size_t end = process.args.find_first_not_of(
                "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_", at);
            if (end != at)
                role = process.args.substr(at, end - at);
        }
        return "WebAppMgr (" + role + ")";
    }
    return !process.comm.empty() && process.comm.size() < 15 ? process.comm
           : !bin.empty()                                    ? bin
                                                             : process.comm;
}
bool readProcesses(const std::string &root, std::vector<Process> &result) {
    return list(root, false, result);
}
bool sampleCpu(const std::string &root, CpuSample &result) {
    std::string text;
    if (!readKernelFile(root + "/stat", text))
        return false;
    std::istringstream line(text.substr(0, text.find('\n')));
    std::string name, field;
    if (!(line >> name) || name != "cpu")
        return false;
    result = CpuSample();
    unsigned count = 0;
    while (line >> field) {
        uint64_t ticks;
        if (!parseUnsigned(field, ticks) || result.total > UINT64_MAX - ticks)
            return false;
        result.total += ticks;
        ++count;
    }
    if (count < 4)
        return false;
    std::vector<Process> processes;
    if (!list(root, true, processes))
        return false;
    for (const auto &process : processes)
        result.processes[process.pid] = process;
    return true;
}
Json memorySummary(const std::vector<Process> &processes, bool all, long pageBytes) {
    if (pageBytes <= 0 || processes.empty())
        return error("could not read process list");
    struct Row {
        const Process *process;
        double mb;
    };
    std::vector<Row> rows;
    double total = 0;
    for (const auto &process : processes) {
        double mb = static_cast<double>(process.residentPages) * pageBytes / 1048576;
        total += mb;
        rows.push_back({&process, tenth(mb)});
    }
    std::stable_sort(rows.begin(), rows.end(),
                     [](const Row &a, const Row &b) { return a.mb > b.mb; });
    Json top = Json::array();
    for (size_t i = 0; i < rows.size() && (all || i < 10); ++i) {
        Json row = Json::object();
        row.set("pid", Json::numeric(rows[i].process->pid));
        row.set("name", Json::text(processName(*rows[i].process)));
        row.set("mb", Json::numeric(rows[i].mb));
        top.add(row);
    }
    Json result = Json::object();
    result.set("ok", Json::flag(true));
    result.set("count", Json::numeric(rows.size()));
    result.set("totalMb", Json::numeric(std::round(total)));
    result.set("top", top);
    return result;
}
Json cpuSummary(const CpuSample &first, const CpuSample &second, unsigned windowMs) {
    if (second.total <= first.total)
        return error("the CPU counters did not move");
    struct Row {
        const Process *process;
        double pct;
    };
    std::vector<Row> rows;
    double busy = 0;
    for (const auto &pair : second.processes) {
        auto was = first.processes.find(pair.first);
        const auto &process = pair.second;
        // A recycled PID must not inherit CPU time from the process that previously held it.
        if (was == first.processes.end() || was->second.started != process.started ||
            process.ticks <= was->second.ticks)
            continue;
        double pct = static_cast<double>(process.ticks - was->second.ticks) /
                     (second.total - first.total) * 100;
        busy += pct;
        rows.push_back({&process, tenth(pct)});
    }
    std::stable_sort(rows.begin(), rows.end(),
                     [](const Row &a, const Row &b) { return a.pct > b.pct; });
    Json top = Json::array();
    for (size_t i = 0; i < rows.size() && i < 10; ++i) {
        Json row = Json::object();
        row.set("pid", Json::numeric(rows[i].process->pid));
        row.set("name", Json::text(processName(*rows[i].process)));
        row.set("pct", Json::numeric(rows[i].pct));
        top.add(row);
    }
    Json result = Json::object();
    result.set("ok", Json::flag(true));
    result.set("windowMs", Json::numeric(windowMs));
    result.set("active", Json::numeric(rows.size()));
    result.set("busy", Json::numeric(tenth(busy)));
    result.set("top", top);
    return result;
}
Json collectMemory(const std::string &root, bool all) {
    std::vector<Process> processes;
    if (!readProcesses(root, processes))
        return error("could not read process list");
    return memorySummary(processes, all, sysconf(_SC_PAGESIZE));
}
Json collectCpu(const std::string &root, unsigned windowMs) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        CpuSample first, second;
        if (!sampleCpu(root, first))
            return error("could not read /proc");
        std::this_thread::sleep_for(std::chrono::milliseconds(windowMs));
        if (!sampleCpu(root, second))
            return error("could not read /proc");
        if (second.total > first.total)
            return cpuSummary(first, second, windowMs);
    }
    return error("the CPU counters did not move");
}
} // namespace service
} // namespace nativetv
