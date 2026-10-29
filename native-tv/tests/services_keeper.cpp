// What the legacy server's services module does besides the toggle route: init (retire old
// ids, drop the old init script, rewrite the boot hook, unmask, bind stubs) and
// enforce (stop a switched-off service found running).
#include "core/json.hpp"
#include "daemon/services_keeper.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using nativetv::daemon::ServicesKeeper;
using nativetv::daemon::ServicesKeeperDeps;
using nativetv::service::BackgroundServiceInfo;

namespace {
int failures = 0;

void check(bool condition, const char *what) {
    std::printf("%s %s\n", condition ? "  ok" : "FAIL", what);
    if (!condition) ++failures;
}
bool exists(const std::string &path) {
    struct stat info;
    return lstat(path.c_str(), &info) == 0;
}
void touch(const std::string &path, const std::string &text = "x") { std::ofstream(path) << text; }
std::string slurp(const std::string &path) {
    std::ifstream input(path);
    std::stringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}
void makeDirectories(const std::string &root, std::initializer_list<const char *> folders) {
    for (const char *folder : folders) {
        std::string path = root;
        std::string rest = folder;
        size_t at = 0;
        while (at < rest.size()) {
            const size_t next = rest.find('/', at + 1);
            path += rest.substr(at, next == std::string::npos ? std::string::npos : next - at);
            mkdir(path.c_str(), 0755);
            if (next == std::string::npos) break;
            at = next;
        }
    }
}

struct Recorder {
    std::vector<std::string> runs;
    std::vector<std::string> bound;
    std::vector<int> killed;
    std::vector<std::function<void()>> later;
    std::vector<long long> laterMs;
    std::vector<std::string> logs;
};

ServicesKeeperDeps depsFor(const std::string &root, Recorder &seen) {
    ServicesKeeperDeps deps;
    deps.systemRoot = root;
    deps.dataRoot = root + "/data";
    deps.procRoot = root + "/proc";
    deps.run = [&seen, root](const std::string &program, const std::vector<std::string> &arguments) {
        std::string line = program.substr(root.size());
        for (const std::string &argument : arguments) line += " " + argument;
        seen.runs.push_back(line);
        return true;
    };
    deps.bindStubs = [&seen](const BackgroundServiceInfo &info, const std::string &id) {
        seen.bound.push_back(id + ":" + info.bin);
        return true;
    };
    deps.terminate = [&seen](int pid) { seen.killed.push_back(pid); };
    deps.later = [&seen](std::chrono::milliseconds delay, std::function<void()> task) {
        seen.laterMs.push_back(delay.count());
        seen.later.push_back(std::move(task));
    };
    deps.log = [&seen](const std::string &message) { seen.logs.push_back(message); };
    return deps;
}

std::string newRoot() {
    char pattern[] = "/tmp/nativetv-keeper-XXXXXX";
    const std::string root = mkdtemp(pattern);
    makeDirectories(root, {"/data", "/proc", "/bin", "/sbin", "/lib/systemd/system",
                           "/run/systemd/transient", "/var/lib/webosbrew/init.d", "/etc/init"});
    return root;
}
void removeTree(const std::string &root) {
    const std::string command = "rm -rf '" + root + "'";
    (void)!system(command.c_str());
}
} // namespace

int main() {
    // init: retired ids go, the old script goes, the hook is rewritten, the
    // masks an earlier version left are lifted, the stubs are bound.
    {
        const std::string root = newRoot();
        touch(root + "/bin/systemctl");
        touch(root + "/lib/systemd/system/uploadd.service");
        touch(root + "/data/disabled_services.json", "[\"alwaysready\",\"uploadd\",\"uploadd\"]");
        touch(root + "/var/lib/webosbrew/init.d/20-services.sh");
        touch(root + "/run/systemd/transient/tvdataexchanger.service");
        touch(root + "/run/systemd/transient/alwaysready.service");
        Recorder seen;
        ServicesKeeper keeper(depsFor(root, seen));
        const std::vector<std::string> list = keeper.start();
        check(list.size() == 1 && list[0] == "uploadd", "the retired id is dropped and a repeat counted once");
        check(slurp(root + "/data/disabled_services.json").find("alwaysready") == std::string::npos,
              "the list file is written without it");
        check(!exists(root + "/var/lib/webosbrew/init.d/20-services.sh"), "the old init script is removed");
        check(exists(root + "/var/lib/webosbrew/init.d/20-tvweb-services"), "the boot hook is written");
        check(!exists(root + "/run/systemd/transient/alwaysready.service") &&
                  !exists(root + "/run/systemd/transient/tvdataexchanger.service"),
              "the masks on the retired units are lifted");
        check(seen.bound.size() == 1 && seen.bound[0] == "uploadd:uploadd", "the stubs of what is off are bound");
        check(seen.runs.size() == 2 && seen.runs[0] == "/bin/systemctl daemon-reload" &&
                  seen.runs[1] == "/bin/systemctl start tvdataexchanger.service alwaysready.service",
              "a lifted mask takes a reload and a start");
        removeTree(root);
    }
    // Nothing retired and nothing masked: no reload, and a list of none removes the hook.
    {
        const std::string root = newRoot();
        touch(root + "/bin/systemctl");
        touch(root + "/data/disabled_services.json", "[]");
        touch(root + "/var/lib/webosbrew/init.d/20-tvweb-services");
        Recorder seen;
        ServicesKeeper keeper(depsFor(root, seen));
        check(keeper.start().empty(), "an empty list stays empty");
        check(seen.runs.empty() && seen.bound.empty(), "nothing is run or bound");
        check(!exists(root + "/var/lib/webosbrew/init.d/20-tvweb-services"), "the boot hook of a past list goes");
        removeTree(root);
    }
    // Without systemctl the list and hook are still kept, and no stub is bound.
    {
        const std::string root = newRoot();
        touch(root + "/data/disabled_services.json", "[\"uploadd\"]");
        Recorder seen;
        ServicesKeeper keeper(depsFor(root, seen));
        check(keeper.start().size() == 1, "the list is returned");
        check(seen.bound.empty() && seen.runs.empty(), "no stub without systemctl, as in services.js");
        check(exists(root + "/var/lib/webosbrew/init.d/20-tvweb-services"), "the hook is written");
        removeTree(root);
    }
    // A list that cannot be read is not guessed at.
    {
        const std::string root = newRoot();
        touch(root + "/data/disabled_services.json", "{not json");
        Recorder seen;
        ServicesKeeper keeper(depsFor(root, seen));
        check(keeper.start().empty() && !seen.logs.empty(), "a malformed list is reported and nothing is done");
        check(seen.runs.empty() && seen.bound.empty(), "and nothing is run");
        check(keeper.enforce() == 0, "and nothing is enforced");
        removeTree(root);
    }
    // enforce: the unit or job is stopped first, what is left is signalled 3 s later.
    {
        const std::string root = newRoot();
        touch(root + "/bin/systemctl");
        touch(root + "/sbin/initctl");
        touch(root + "/lib/systemd/system/uploadd.service");
        touch(root + "/etc/init/crashreportd.conf");
        touch(root + "/data/disabled_services.json",
              "[\"uploadd\",\"crashreportd\",\"rdxd\",\"voiceconductor\"]");
        makeDirectories(root, {"/proc/4242", "/proc/4243", "/proc/4244", "/proc/4245"});
        (void)!symlink("/usr/sbin/uploadd", (root + "/proc/4242/exe").c_str());
        (void)!symlink("/usr/sbin/crashreportd", (root + "/proc/4243/exe").c_str());
        (void)!symlink("/usr/bin/voiceconductor", (root + "/proc/4244/exe").c_str());
        (void)!symlink("/usr/bin/other", (root + "/proc/4245/exe").c_str());
        Recorder seen;
        ServicesKeeper keeper(depsFor(root, seen));
        check(keeper.enforce() == 3, "three switched-off services were found running; one not on the TV is skipped");
        // uploadd has both a unit and a job, and services.js stops each.
        check(seen.runs.size() == 4 && seen.runs[0] == "/bin/systemctl stop uploadd.service" &&
                  seen.runs[1] == "/sbin/initctl stop uploadd" &&
                  seen.runs[2] == "/sbin/initctl stop crashreportd" &&
                  seen.runs[3] == "/bin/systemctl stop voiceconductor.service",
              "a unit is stopped through systemctl, a job through initctl, a hidden one too");
        check(seen.laterMs.size() == 3 && seen.laterMs[0] == 3000 && seen.killed.empty(),
              "the signal waits 3 s and nothing is signalled yet");
        for (auto &task : seen.later) task();
        check(seen.killed.size() == 3, "what is still running then is signalled");
        removeTree(root);
    }
    // Nothing running, or nothing off: nothing is done.
    {
        const std::string root = newRoot();
        touch(root + "/bin/systemctl");
        touch(root + "/lib/systemd/system/uploadd.service");
        touch(root + "/data/disabled_services.json", "[\"uploadd\"]");
        Recorder seen;
        ServicesKeeper keeper(depsFor(root, seen));
        check(keeper.enforce() == 0 && seen.runs.empty() && seen.later.empty(), "a service that is not running is left");
        touch(root + "/data/disabled_services.json", "[]");
        makeDirectories(root, {"/proc/9"});
        (void)!symlink("/usr/sbin/uploadd", (root + "/proc/9/exe").c_str());
        check(keeper.enforce() == 0 && seen.runs.empty(), "one that is not switched off is left running");
        removeTree(root);
    }
    // The boot hook is the text services.js builds (recorded in tests/fixtures/boot-scripts.json).
    {
        const nativetv::Json fixture = nativetv::Json::parse(
            slurp(std::string(NATIVETV_FIXTURES) + "/boot-scripts.json"));
        for (const nativetv::Json &item : fixture["cases"].items()) {
            const std::string root = newRoot();
            for (const nativetv::Json &program : fixture["programs"].items()) {
                const std::string path = program.string();
                makeDirectories(root, {path.substr(0, path.find_last_of('/')).c_str()});
                touch(root + path);
            }
            std::string list = "[";
            for (const nativetv::Json &id : item["list"].items())
                list += std::string(list.size() > 1 ? "," : "") + "\"" + id.string() + "\"";
            touch(root + "/data/disabled_services.json", list + "]");
            Recorder seen;
            ServicesKeeper keeper(depsFor(root, seen));
            (void)keeper.start();
            std::string hook = slurp(root + "/var/lib/webosbrew/init.d/20-tvweb-services");
            for (size_t at; (at = hook.find(root + "/data")) != std::string::npos;)
                hook.replace(at, root.size() + 5, "/var/lib/tvweb");
            check(hook == item["script"].string(), ("the hook for " + list + "] is services.js's text").c_str());
            removeTree(root);
        }
    }
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
