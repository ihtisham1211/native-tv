#include "daemon/child_process.hpp"
#include "daemon/home_probe_gate.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace nativetv;
using namespace nativetv::daemon;

namespace {
void check(bool value, const std::string &message) {
    if (!value) throw std::runtime_error(message);
}

std::string root;

void put(const std::string &path, const std::string &text) {
    {
        std::ofstream out(path.c_str(), std::ios::trunc);
        out << text;
    }
    chmod(path.c_str(), 0755);
}

std::string read(const std::string &path) {
    std::ifstream in(path.c_str());
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

int runs(const std::string &client) {
    const std::string text = read(client + ".count");
    int lines = 0;
    for (char character : text) lines += character == '\n';
    return lines;
}

// A stand-in client: counts its runs, then exits as the file beside it says.
std::string standIn(const std::string &name, int exitCode) {
    const std::string client = root + "/" + name;
    put(client + ".exit", std::to_string(exitCode) + "\n");
    put(client, "#!/bin/sh\necho \"$@\" >> \"$0.count\"\n"
                "echo '{\"ok\":true,\"renderer\":\"Stand-in GPU\",\"maxTextureSize\":8192}'\n"
                "exit $(cat \"$0.exit\")\n");
    return client;
}

HomeProbeOptions optionsFor(const std::string &client, const std::string &cache = std::string()) {
    HomeProbeOptions options;
    options.clientPath = client;
    options.cachePath = cache;
    options.timeout = std::chrono::milliseconds(3000);
    options.retryGap = std::chrono::milliseconds(5);
    return options;
}

ChildRunnerOptions quickRunner() {
    ChildRunnerOptions options;
    options.startupGap = std::chrono::milliseconds(0);
    options.regularGap = std::chrono::milliseconds(0);
    return options;
}

void exitCodesAreClassified() {
    struct Case {
        int exitCode;
        const char *name;
    };
    const Case cases[] = {{0, "ok"},      {10, "display"}, {11, "context"},
                          {12, "gl"},     {13, "readback"}, {1, "failed"}, {77, "failed"}};
    for (const Case &c : cases) {
        ChildProcessRunner runner(quickRunner());
        const std::string client = standIn("client-" + std::to_string(c.exitCode), c.exitCode);
        HomeProbeOptions options = optionsFor(client);
        options.attempts = 1;
        HomeProbeGate gate(runner, options);
        const Json verdict = gate.verdict(false);
        check(verdict["class"].string() == c.name && verdict["exit"].number() == c.exitCode &&
                  verdict["ok"].boolean() == (c.exitCode == 0),
              "exit " + std::to_string(c.exitCode) + " is " + c.name + ": " + verdict.dump());
        check(read(client + ".count") == "--probe\n", "the probe is run with --probe only");
        check(verdict["report"]["renderer"].string() == "Stand-in GPU",
              "the probe's JSON line is kept: " + verdict.dump());
    }
}

void aMissingClientIsRefused() {
    ChildProcessRunner runner(quickRunner());
    HomeProbeGate gate(runner, optionsFor(root + "/no-such-client"));
    const Json verdict = gate.verdict(false);
    check(!verdict["ok"].boolean() && verdict["class"].string() == "missing", verdict.dump());
    put(root + "/not-executable", "#!/bin/sh\nexit 0\n");
    chmod((root + "/not-executable").c_str(), 0644);
    HomeProbeGate plain(runner, optionsFor(root + "/not-executable"));
    check(plain.verdict(false)["class"].string() == "missing", "a file that cannot run is missing");
}

void aHungOrCrashedProbeIsRefused() {
    ChildProcessRunner runner(quickRunner());
    const std::string hung = root + "/hung";
    put(hung, "#!/bin/sh\nexec sleep 20\n");
    HomeProbeOptions options = optionsFor(hung);
    options.timeout = std::chrono::milliseconds(300);
    HomeProbeGate slow(runner, options);
    const Json timedOut = slow.verdict(false);
    check(!timedOut["ok"].boolean() && timedOut["class"].string() == "timeout", timedOut.dump());

    const std::string crashed = root + "/crashed";
    put(crashed, "#!/bin/sh\nkill -SEGV $$\n");
    HomeProbeGate crash(runner, optionsFor(crashed));
    const Json signalled = crash.verdict(false);
    check(!signalled["ok"].boolean() && signalled["class"].string() == "crashed" &&
              signalled["signal"].number() == 11,
          signalled.dump());
}

void noDisplayYetIsTriedAgain() {
    ChildProcessRunner runner(quickRunner());
    const std::string client = root + "/late-display";
    put(client, "#!/bin/sh\necho x >> \"$0.count\"\n"
                "[ \"$(wc -l < \"$0.count\" | tr -d ' ')\" -ge 3 ] && exit 0\nexit 10\n");
    HomeProbeGate gate(runner, optionsFor(client));
    const Json verdict = gate.verdict(false);
    check(verdict["ok"].boolean() && runs(client) == 3, "a display that comes up on the third try: " + verdict.dump());

    const std::string never = standIn("never-display", 11);
    HomeProbeGate giving(runner, optionsFor(never));
    check(giving.verdict(false)["class"].string() == "context" && runs(never) == 3,
          "three tries, then the last answer");
    const std::string broken = standIn("broken-shader", 12);
    HomeProbeGate once(runner, optionsFor(broken));
    check(once.verdict(false)["class"].string() == "gl" && runs(broken) == 1,
          "a driver fault is not retried");
}

void aSettledVerdictIsKept() {
    ChildProcessRunner runner(quickRunner());
    const std::string client = standIn("kept", 0);
    const std::string cache = root + "/kept-cache.json";
    {
        HomeProbeGate gate(runner, optionsFor(client, cache));
        check(gate.verdict(false)["cached"].boolean() == false && runs(client) == 1, "the first asks the probe");
        const Json again = gate.verdict(false);
        check(again["ok"].boolean() && again["cached"].boolean() && runs(client) == 1,
              "and the next does not: " + again.dump());
        check(gate.known()["ok"].boolean() && runs(client) == 1, "known() answers without running");
        check(gate.verdict(true)["cached"].boolean() == false && runs(client) == 2, "force asks again");
    }
    {
        HomeProbeGate restarted(runner, optionsFor(client, cache));
        check(!restarted.known().valid(), "nothing is known before the first verdict");
        const Json verdict = restarted.verdict(false);
        check(verdict["ok"].boolean() && verdict["cached"].boolean() && runs(client) == 2,
              "a restart keeps it, so the probe's window is not opened at every boot");
    }
    {
        HomeProbeOptions firmware = optionsFor(client, cache);
        firmware.identity = "webOS TV 10.3.2";
        HomeProbeGate updated(runner, firmware);
        check(updated.verdict(false)["cached"].boolean() == false && runs(client) == 3,
              "another firmware asks again");
    }
    {
        // A new client build, here a different size.
        put(client, read(client) + "# a new build\n");
        HomeProbeGate rebuilt(runner, optionsFor(client, cache));
        check(rebuilt.verdict(false)["cached"].boolean() == false && runs(client) == 4,
              "another client asks again");
    }
    {
        // A fault of the driver stays the answer; one that may be timing does not.
        const std::string broken = standIn("kept-broken", 13);
        const std::string brokenCache = root + "/broken-cache.json";
        HomeProbeGate gate(runner, optionsFor(broken, brokenCache));
        check(gate.verdict(false)["class"].string() == "readback", "readback");
        check(gate.verdict(false)["cached"].boolean() && runs(broken) == 1, "is kept");

        const std::string early = standIn("kept-early", 10);
        const std::string earlyCache = root + "/early-cache.json";
        HomeProbeOptions options = optionsFor(early, earlyCache);
        options.attempts = 1;
        HomeProbeGate timing(runner, options);
        check(timing.verdict(false)["class"].string() == "display", "display");
        check(timing.verdict(false)["cached"].boolean() == false && runs(early) == 2,
              "is asked again, and not kept on disk");
        check(access(earlyCache.c_str(), F_OK) != 0, "no cache file for a verdict that may be timing");
    }
}

// What the status route reports follows the file, so a refusal does not outlive the client it was about.
void knownFollowsTheClientFile() {
    ChildProcessRunner runner(quickRunner());
    const std::string client = standIn("known-gl", 12);
    HomeProbeGate gate(runner, optionsFor(client));
    check(!gate.known().valid(), "nothing before a verdict");
    gate.verdict(false);
    check(gate.known()["class"].string() == "gl", "a verdict is known");
    put(client, read(client) + "# a new build\n");
    check(!gate.known().valid(), "and is not once the file is another");

    HomeProbeGate absent(runner, optionsFor(root + "/appears-later"));
    check(absent.verdict(false)["class"].string() == "missing" && absent.known()["class"].string() == "missing",
          "a missing client is known to be missing");
    standIn("appears-later", 0);
    check(!absent.known().valid(), "until the client is installed");
}


// The Native Tv tile is the one app sam is told of: Home needs it known, asked every time.
void aClientSamDoesNotKnowIsRefused() {
    ChildProcessRunner runner(quickRunner());
    const std::string client = standIn("registered", 0);
    int answer = 1;
    HomeProbeGate gate(runner, optionsFor(client, root + "/registered-cache.json"), [&answer]() { return answer; });
    check(gate.verdict(false)["ok"].boolean() && runs(client) == 1, "a known app passes the probe");
    answer = 0;
    Json refused = gate.verdict(false);
    check(!refused["ok"].boolean() && refused["class"].string() == "unregistered" && runs(client) == 1,
          "taken off the home screen after a kept verdict, it is refused without the probe: " + refused.dump());
    check(gate.known()["class"].string() == "unregistered", "and the status route sees it");
    answer = -1;
    check(gate.verdict(false)["class"].string() == "unasked", "an unanswered question is a refusal too");
    answer = 1;
    Json again = gate.verdict(false);
    check(again["ok"].boolean() && again["cached"].boolean() && runs(client) == 1,
          "put back on it, the verdict kept before still stands: " + again.dump());
    answer = 0;
    check(gate.verdict(true)["class"].string() == "unregistered" && runs(client) == 1,
          "force does not probe a client sam will not open");

    // A missing file is told before sam is asked.
    int asked = 0;
    HomeProbeGate absent(runner, optionsFor(root + "/gone-client"), [&asked]() { ++asked; return 1; });
    check(absent.verdict(false)["class"].string() == "missing" && asked == 0, "no file, no question");

    Json refusal = Json::object();
    refusal.set("ok", Json::flag(false));
    refusal.set("class", Json::text("unregistered"));
    Json supported = Json::object();
    supported.set("ok", Json::flag(true));
    const Json reason = homeSupportedWithProbe(supported, refusal);
    check(!reason["ok"].boolean() && reason["reason"].string().find("home screen") != std::string::npos &&
              reason["reason"].string().find("Server tab") != std::string::npos,
          "the reason says where to switch the tile on: " + reason.dump());
}

void aRefusalIsAppliedToSupported() {
    Json supported = Json::object();
    supported.set("ok", Json::flag(true));
    supported.set("webos", Json::text("10.3.2"));
    supported.set("experimental", Json::flag(true));

    Json good = Json::object();
    good.set("ok", Json::flag(true));
    good.set("class", Json::text("ok"));
    check(homeSupportedWithProbe(supported, good).dump() == supported.dump(), "a passing probe changes nothing");
    check(homeSupportedWithProbe(supported, Json()).dump() == supported.dump(), "no verdict changes nothing");

    const char *const classes[] = {"missing", "unregistered", "unasked", "timeout", "display", "context", "gl", "readback", "crashed", "failed"};
    for (const char *name : classes) {
        Json bad = Json::object();
        bad.set("ok", Json::flag(false));
        bad.set("exit", Json::numeric(12));
        bad.set("class", Json::text(name));
        const Json refused = homeSupportedWithProbe(supported, bad);
        check(!refused["ok"].boolean() && refused["probe"]["class"].string() == name &&
                  refused["webos"].string() == "10.3.2" && refused["reason"].string().size() > 20,
              std::string("refused as ") + name + ": " + refused.dump());
    }
    Json driver = Json::object();
    driver.set("ok", Json::flag(false));
    driver.set("exit", Json::numeric(12));
    driver.set("class", Json::text("gl"));
    check(homeSupportedWithProbe(supported, driver)["reason"].string().find("(check exit 12)") != std::string::npos,
          "the reason names the exit");

    Json unsupported = Json::object();
    unsupported.set("ok", Json::flag(false));
    unsupported.set("reason", Json::text("webOS 4"));
    check(homeSupportedWithProbe(unsupported, driver)["reason"].string() == "webOS 4",
          "a version refusal keeps its own reason, and has no probe");
}

void optionsReadTheConfig() {
    Json config = Json::parse(R"({"nativeClient":{"path":"/x/client","probeTimeoutMs":50,
        "probeCommand":["/bin/true","--probe","--extra"]}})");
    HomeProbeOptions options = homeProbeOptions(config, "/", root);
    check(options.clientPath == "/x/client" && options.timeout.count() == 1000 &&
              options.command.size() == 3 && options.command[2] == "--extra" &&
              options.cachePath == root + "/native-probe.json",
          "the config section is read and the timeout kept sane");
    options = homeProbeOptions(Json::object(), "/nowhere", root);
    check(options.clientPath == std::string("/nowhere/media/developer/apps/usr/palm/applications/") + kHomeClientApp + "/nativetv-tv" &&
              std::string(kHomeClientApp) == "com.tvweb.dashboard" &&
              options.command.empty(),
          "the default client is the app's, and without a jailer it is run directly");
}
} // namespace

int main() {
    char pattern[] = "/tmp/nativetv-probe-gate-XXXXXX";
    if (!mkdtemp(pattern)) {
        std::perror("mkdtemp");
        return 1;
    }
    root = pattern;
    int status = 0;
    try {
        exitCodesAreClassified();
        aMissingClientIsRefused();
        aHungOrCrashedProbeIsRefused();
        noDisplayYetIsTriedAgain();
        aSettledVerdictIsKept();
        knownFollowsTheClientFile();
        aClientSamDoesNotKnowIsRefused();
        aRefusalIsAppliedToSupported();
        optionsReadTheConfig();
        std::puts("home probe gate tests passed");
    } catch (const std::exception &error) {
        std::fprintf(stderr, "home probe gate test failed: %s\n", error.what());
        status = 1;
    }
    const std::string cleanup = "rm -rf '" + root + "'";
    if (std::system(cleanup.c_str()) != 0) status = status ? status : 1;
    return status;
}
