#include "daemon/home_probe_gate.hpp"

#include "daemon/child_process.hpp"
#include "daemon/json_file.hpp"
#include "domain/say.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <thread>

extern char **environ;

namespace nativetv {
namespace daemon {
namespace {
const char *const kClientApp = "com.tvweb.native";
const char *const kClientDirectory = "/media/developer/apps/usr/palm/applications/com.tvweb.native";

std::string rooted(const std::string &root, const std::string &path) {
    if (root.empty() || root == "/") return path;
    size_t end = root.size();
    while (end > 1 && root[end - 1] == '/') --end;
    return root.substr(0, end) + path;
}

std::string trimmed(std::string text) {
    const char *const blank = " \t\r\n";
    const size_t first = text.find_first_not_of(blank);
    if (first == std::string::npos) return std::string();
    return text.substr(first, text.find_last_not_of(blank) - first + 1);
}

std::string readText(const std::string &path) {
    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
    std::ostringstream text;
    if (input) text << input.rdbuf();
    return text.str();
}

// A verdict a rerun would not change by itself: it is kept across restarts. The
// rest (no display yet, a timeout, a crash) may be the boot's timing.
bool settled(const Json &verdict) {
    const std::string name = verdict["class"].string();
    return name == "ok" || name == "gl" || name == "readback";
}

Json verdictOf(const std::string &name, int exitCode) {
    Json verdict = Json::object();
    verdict.set("ok", Json::flag(name == "ok"));
    verdict.set("exit", Json::numeric(exitCode));
    verdict.set("class", Json::text(name));
    return verdict;
}

// The exit codes are platform/probe.hpp's.
Json classify(const ChildResult &result) {
    Json verdict;
    if (!result.error.empty()) {
        verdict = verdictOf("failed", -1);
        verdict.set("error", Json::text(result.error));
    } else if (result.timedOut) {
        verdict = verdictOf("timeout", -1);
    } else if (result.signal != 0) {
        verdict = verdictOf("crashed", -1);
        verdict.set("signal", Json::numeric(result.signal));
    } else {
        const char *name = "failed";
        switch (result.exitCode) {
        case 0: name = "ok"; break;
        case 10: name = "display"; break;
        case 11: name = "context"; break;
        case 12: name = "gl"; break;
        case 13: name = "readback"; break;
        default: break;
        }
        verdict = verdictOf(name, result.exitCode);
    }
    // The probe prints one JSON line (renderer, texture limit, ...) on stdout.
    const std::string output = trimmed(result.standardOutput);
    const size_t lastLine = output.rfind('\n');
    const Json report = Json::parse(lastLine == std::string::npos ? output : output.substr(lastLine + 1));
    if (report.isObject()) verdict.set("report", report);
    return verdict;
}

std::string reasonOf(const Json &verdict) {
    Json variables = Json::object();
    variables.set("code", Json::text(verdict["class"].string() == "crashed"
                                         ? std::to_string(static_cast<int>(verdict["signal"].number()))
                                         : std::to_string(static_cast<int>(verdict["exit"].number()))));
    const std::string name = verdict["class"].string();
    if (name == "missing")
        return say::msg("srv.home.probe.missing",
                        "The Native Tv app is not installed on this TV, so Native Tv Home cannot take LG's Home's place.");
    if (name == "timeout")
        return say::msg("srv.home.probe.timeout",
                        "The Native Tv app's start-up check did not finish in time, so Native Tv Home cannot take LG's Home's place.");
    if (name == "display")
        return say::msg("srv.home.probe.display",
                        "The Native Tv app could not reach this TV's display (check exit {code}), so Native Tv Home cannot take LG's Home's place.",
                        variables);
    if (name == "context")
        return say::msg("srv.home.probe.context",
                        "The Native Tv app could not open a graphics window on this TV (check exit {code}), so Native Tv Home cannot take LG's Home's place.",
                        variables);
    if (name == "gl")
        return say::msg("srv.home.probe.gl",
                        "This TV's graphics driver cannot run the Native Tv app (check exit {code}), so Native Tv Home cannot take LG's Home's place.",
                        variables);
    if (name == "readback")
        return say::msg("srv.home.probe.readback",
                        "This TV's graphics driver drew the Native Tv app's check frame wrongly (check exit {code}), so Native Tv Home cannot take LG's Home's place.",
                        variables);
    if (name == "crashed")
        return say::msg("srv.home.probe.crashed",
                        "The Native Tv app crashed in its start-up check (signal {code}), so Native Tv Home cannot take LG's Home's place.",
                        variables);
    return say::msg("srv.home.probe.failed",
                    "The Native Tv app's start-up check failed (exit {code}), so Native Tv Home cannot take LG's Home's place.",
                    variables);
}
} // namespace

HomeProbeOptions homeProbeOptions(const Json &config, const std::string &systemRoot,
                                  const std::string &dataRoot) {
    const Json section = config["nativeClient"];
    HomeProbeOptions options;
    options.clientPath = section["path"].isString() && !section["path"].string().empty()
                             ? section["path"].string()
                             : rooted(systemRoot, std::string(kClientDirectory) + "/nativetv-tv");
    if (section["probeCommand"].isArray()) {
        for (const Json &argument : section["probeCommand"].items())
            if (argument.isString()) options.command.push_back(argument.string());
    } else {
        // Started as deploy.py starts it: the jail's own launcher and the display socket.
        const std::string jailer = rooted(systemRoot, "/usr/bin/jailer");
        if (access(jailer.c_str(), X_OK) == 0) {
            const size_t slash = options.clientPath.rfind('/');
            options.command = {jailer, "-t", "native_devmode", "-i", kClientApp, "-p",
                               slash == std::string::npos ? "." : options.clientPath.substr(0, slash),
                               options.clientPath, "--probe"};
            options.environment.push_back("XDG_RUNTIME_DIR=/tmp/xdg");
        }
    }
    if (section["probeTimeoutMs"].isNumber()) {
        const double wanted = section["probeTimeoutMs"].number();
        options.timeout = std::chrono::milliseconds(
            static_cast<long long>(wanted < 1000 ? 1000 : (wanted > 120000 ? 120000 : wanted)));
    }
    options.identity = trimmed(readText(rooted(systemRoot, "/etc/issue")));
    options.cachePath = (dataRoot.empty() ? std::string("/var/lib/tvweb") : dataRoot) + "/native-probe.json";
    return options;
}

HomeProbeGate::HomeProbeGate(ChildProcessRunner &children, HomeProbeOptions options)
    : children_(children), options_(std::move(options)) {}

// What identifies the client file: its size and time. Empty where it cannot be run.
std::string HomeProbeGate::clientKey() const {
    struct stat info;
    if (stat(options_.clientPath.c_str(), &info) != 0 || !S_ISREG(info.st_mode) ||
        access(options_.clientPath.c_str(), X_OK) != 0)
        return std::string();
    return std::to_string(static_cast<long long>(info.st_size)) + ":" +
           std::to_string(static_cast<long long>(info.st_mtime)) + ":" + options_.identity;
}

Json HomeProbeGate::known() const {
    const std::string key = clientKey();
    std::lock_guard<std::mutex> lock(mutex_);
    return last_.valid() && lastKey_ == key ? last_ : Json();
}

Json HomeProbeGate::run() {
    ChildOptions child;
    child.arguments = options_.command.empty()
                          ? std::vector<std::string>{options_.clientPath, "--probe"}
                          : options_.command;
    if (!options_.environment.empty()) {
        // A child given an environment gets only that one: the daemon's own, with the extras over it.
        for (char **entry = environ; entry && *entry; ++entry) {
            const std::string line(*entry);
            bool overridden = false;
            for (const std::string &extra : options_.environment)
                overridden = overridden || line.compare(0, extra.find('=') + 1, extra, 0, extra.find('=') + 1) == 0;
            if (!overridden) child.environment.push_back(line);
        }
        child.environment.insert(child.environment.end(), options_.environment.begin(),
                                 options_.environment.end());
    }
    child.timeout = options_.timeout;
    child.maximumOutputBytes = 16 * 1024;
    Json verdict;
    for (int attempt = 1;; ++attempt) {
        verdict = classify(children_.run(child));
        const int exitCode = static_cast<int>(verdict["exit"].number(-1));
        if ((exitCode != 10 && exitCode != 11) || attempt >= options_.attempts) return verdict;
        std::this_thread::sleep_for(options_.retryGap);
    }
}

Json HomeProbeGate::verdict(bool force) {
    std::lock_guard<std::mutex> serial(runMutex_);
    const std::string key = clientKey();
    const auto remember = [this, &key](const Json &verdict) {
        std::lock_guard<std::mutex> lock(mutex_);
        last_ = verdict;
        lastKey_ = key;
    };
    if (key.empty()) {
        remember(verdictOf("missing", -1));
        return known();
    }
    JsonFile cache(options_.cachePath, 64 * 1024);
    if (!force) {
        Json kept = known();
        if (!kept.valid() || !settled(kept)) {
            kept = Json();
            if (!options_.cachePath.empty()) {
                const JsonFileResult stored = cache.load();
                if (stored.found && stored.value["key"].string() == key && settled(stored.value["verdict"]))
                    kept = stored.value["verdict"];
            }
        }
        if (kept.valid()) {
            remember(kept);
            Json answer = Json::parse(kept.dump());
            answer.set("cached", Json::flag(true));
            return answer;
        }
    }
    const Json fresh = run();
    remember(fresh);
    if (!options_.cachePath.empty()) {
        if (settled(fresh)) {
            Json entry = Json::object();
            entry.set("key", Json::text(key));
            entry.set("verdict", fresh);
            std::string error;
            (void)cache.save(entry, &error);
        } else {
            // JsonFile keeps the value it replaced beside the file, and reads it when the file is gone.
            (void)unlink(options_.cachePath.c_str());
            (void)unlink((options_.cachePath + ".last-good").c_str());
        }
    }
    Json answer = Json::parse(fresh.dump());
    answer.set("cached", Json::flag(false));
    return answer;
}

Json homeSupportedWithProbe(const Json &supported, const Json &verdict) {
    if (!supported["ok"].boolean() || !verdict.isObject() || verdict["ok"].boolean()) return supported;
    Json refused = Json::parse(supported.dump());
    refused.set("ok", Json::flag(false));
    refused.set("reason", Json::text(reasonOf(verdict)));
    refused.set("probe", verdict);
    return refused;
}
} // namespace daemon
} // namespace nativetv
