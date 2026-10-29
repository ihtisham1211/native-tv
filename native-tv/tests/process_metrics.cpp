#include "service/process_metrics.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
using namespace nativetv;
using namespace nativetv::service;
namespace {
unsigned checks = 0;
void check(bool value, const char *message) {
    ++checks;
    if (!value)
        throw std::runtime_error(message);
}
void write(const std::string &path, const std::string &value) {
    std::ofstream file(path, std::ios::binary);
    file << value;
}
struct Fixture {
    std::string root;
    std::vector<std::string> paths;
    Fixture() {
        char pattern[] = "/tmp/nativetv-metrics-XXXXXX";
        char *folder = mkdtemp(pattern);
        if (!folder)
            throw std::runtime_error("Could not create fixture");
        root = folder;
    }
    void process(int pid, const std::string &comm, const std::string &args, unsigned pages,
                 unsigned user = 100, unsigned system = 50, unsigned started = 10) {
        std::string base = root + "/" + std::to_string(pid);
        if (mkdir(base.c_str(), 0700) == 0)
            paths.push_back(base);
        std::string stat = std::to_string(pid) + " (" + comm + ") S";
        for (unsigned field = 1; field < 20; ++field)
            stat += " " + std::to_string(field == 11   ? user
                                         : field == 12 ? system
                                         : field == 19 ? started
                                                       : 0);
        write(base + "/stat", stat);
        write(base + "/statm", "99999 " + std::to_string(pages) + " 0 0");
        write(base + "/cmdline", args);
    }
    ~Fixture() {
        for (const auto &base : paths) {
            for (const char *name : {"/stat", "/statm", "/cmdline"})
                std::remove((base + name).c_str());
            rmdir(base.c_str());
        }
        std::remove((root + "/stat").c_str());
        rmdir(root.c_str());
    }
};
} // namespace
int main() {
    try {
        Fixture f;
        f.process(3794, "hal-gal", std::string("/usr/sbin/hal-gal\0-d\0", 21), 10240);
        f.process(4121, "a (b) c", "/usr/bin/long-process-name --room living room", 3840);
        f.process(2, "kthreadd", "", 0);
        f.process(55, "gone", "", 0);
        std::remove((f.root + "/55/stat").c_str());
        f.process(56, "bad", "", 0);
        write(f.root + "/56/statm", "9999 -100");
        std::vector<Process> processes;
        check(readProcesses(f.root, processes), "Readable proc fixture");
        check(processes.size() == 3, "Malformed and disappeared processes skipped");
        check(processes[0].args == "[kthreadd]", "Kernel thread fallback");
        check(processes[2].comm == "a (b) c", "Parentheses in comm");
        Json memory = memorySummary(processes, true, 4096);
        check(memory["count"].number() == 3 && memory["totalMb"].number() == 55, "Memory totals");
        check(memory["top"][size_t(0)]["pid"].number() == 3794, "Largest process first");
        check(memory["top"][size_t(0)]["mb"].number() == 40, "Pages converted to MB");
        check(memory["top"][size_t(1)]["name"].string() == "a (b) c", "Short comm preferred");
        check(memorySummary(processes, true, 16384)["totalMb"].number() == 220,
              "Runtime page size");
        Process fractional;
        fractional.pid = 1;
        fractional.comm = "fractional";
        fractional.residentPages = 345;
        Json rounded = memorySummary({fractional}, true, 4096);
        check(rounded["top"][size_t(0)]["mb"].number() == 1.3, "Rows round to tenths of MB");
        std::vector<Process> small(4, fractional);
        for (size_t i = 0; i < small.size(); ++i) {
            small[i].pid = static_cast<int>(i + 1);
            small[i].residentPages = 38;
        }
        Json total = memorySummary(small, true, 4096);
        check(total["top"][size_t(0)]["mb"].number() == 0.1 &&
                  total["totalMb"].number() == 1,
              "Total rounds from unrounded pages rather than rounded rows");
        for (int i = 0; i < 12; ++i) {
            Process process;
            process.pid = 100 + i;
            process.comm = "worker";
            process.residentPages = i;
            processes.push_back(process);
        }
        check(memorySummary(processes, false, 4096)["top"].size() == 10, "Default ten rows");
        check(memorySummary(processes, true, 4096)["top"].size() == 15, "All rows");
        check(!memorySummary(processes, true, 0)["ok"].boolean(), "Unknown page size fails");
        Process web;
        web.comm = "WebAppMgr";
        web.args = "/usr/bin/WebAppMgr /usr/palm/applications/com.webos.app.browser/index.html";
        check(processName(web) == "WebAppMgr (browser)", "Application name");
        web.args = "/usr/bin/WebAppMgr --type=renderer";
        check(processName(web) == "WebAppMgr (renderer)", "Renderer name");
        web.args = "/usr/bin/WebAppMgr";
        check(processName(web) == "WebAppMgr (browser)", "Browser fallback");
        web.comm = "long-process-na";
        web.args = "/usr/sbin/long-process-name --daemon";
        check(processName(web) == "long-process-name", "Truncated comm resolved");
        web.comm = "long-kernel-thread";
        web.args.clear();
        check(processName(web) == "long-kernel-thread", "CPU kernel thread name without argv");
        CpuSample first, second;
        write(f.root + "/stat", "cpu 500 100 300 100 0 0 0 0 0 0\n");
        check(sampleCpu(f.root, first), "First CPU sample");
        check(first.total == 1000 && first.processes[3794].ticks == 150, "CPU stat fields");
        f.process(3794, "hal-gal", "/usr/sbin/hal-gal", 10240, 350, 50);
        f.process(4121, "a (b) c", "", 3840, 150, 50);
        f.process(2, "recycled", "", 0, 500, 50, 20);
        f.process(99, "new", "", 0, 500, 50);
        write(f.root + "/stat", "cpu 1000 100 800 100 0 0 0 0 0 0\n");
        check(sampleCpu(f.root, second), "Second CPU sample");
        Json cpu = cpuSummary(first, second, 5000);
        check(cpu["busy"].number() == 30 && cpu["active"].number() == 2,
              "New/recycled PIDs excluded");
        check(cpu["top"][size_t(0)]["pid"].number() == 3794 &&
                  cpu["top"][size_t(0)]["pct"].number() == 25,
              "CPU ranking");
        check(cpu["windowMs"].number() == 5000, "CPU window contract");
        check(!cpuSummary(first, first, 5000)["ok"].boolean(), "Stuck counters fail");
        second.total = 1;
        check(!cpuSummary(first, second, 5000)["ok"].boolean(), "Counter reset fails");
        write(f.root + "/stat", "cpu 0 nope 0 0\n");
        check(!sampleCpu(f.root, second), "Malformed total rejected");
        check(!readProcesses(f.root + "/missing", processes), "Unreadable proc fails");
        check(!collectMemory(f.root + "/missing", true)["ok"].boolean(), "API error contract");
        std::printf("%u native metrics checks passed\n", checks);
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
