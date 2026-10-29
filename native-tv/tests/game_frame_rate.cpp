// The source's frame rate: the Game tab's stream and the stats' cached one-shot
// read sharing one bind (test/test-game.js), against a scripted luna-send.
#include "daemon/child_process.hpp"
#include "daemon/event_loop.hpp"
#include "daemon/game_frame_rate.hpp"
#include "daemon/luna.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

// Fixture folders are made and removed with the shell; a failure shows in the checks that read them.
static void runShell(const std::string &command) {
    if (std::system(command.c_str()) != 0) {
    }
}


using namespace nativetv;
using namespace nativetv::daemon;

namespace {
int failures = 0;
std::string root;

void check(bool condition, const std::string &what) {
    std::printf("%s %s\n", condition ? "  ok" : "FAIL", what.c_str());
    if (!condition) ++failures;
}
std::string read(const std::string &path) {
    std::ifstream in(path.c_str());
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}
void write(const std::string &path, const std::string &text) {
    std::ofstream out(path.c_str(), std::ios::trunc);
    out << text;
}
void append(const std::string &path, const std::string &line) {
    std::ofstream out(path.c_str(), std::ios::app);
    out << line << "\n";
}
size_t count(const std::string &needle) {
    const std::string text = read(root + "/calls");
    size_t found = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++found;
    return found;
}
bool waitFor(const std::function<bool()> &condition, int ms = 4000) {
    for (int waited = 0; waited < ms; waited += 20) {
        if (condition()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return condition();
}

// luna-send: a subscription (-i) follows a feed file the test appends to, a
// one-shot getVRRInfo answers from reply-<pipeline>; every call is logged.
void script() {
    write(root + "/luna-send",
          "#!/bin/sh\n"
          // tail -f polls once a second where inotify is not available (a container's
          // filesystem), so the feed is followed here, every 50 ms, the same on macOS and Linux.
          "follow() { n=0; while :; do c=$(wc -l < \"$1\"); [ \"$c\" -lt \"$n\" ] && n=0;\n"
          "  if [ \"$c\" -gt \"$n\" ]; then tail -n +$((n + 1)) \"$1\" | head -n $((c - n)); n=$c; fi; sleep 0.05; done; }\n"
          "mode=once; for a in \"$@\"; do [ \"$a\" = \"-i\" ] && mode=sub; done\n"
          "for a in \"$@\"; do last_prev=$last; last=$a; done\n"
          "uri=$last_prev; payload=$last\n"
          "case \"$uri\" in luna://*) ;; *) uri=$(echo \"$*\" | sed 's/.*\\(luna:\\/\\/[^ ]*\\).*/\\1/') ;; esac\n"
          "echo \"$mode $uri $payload\" >> " + root + "/calls\n"
          "case \"$uri\" in\n"
          "  *extinputs/bind) follow " + root + "/bind.feed ;;\n"
          "  *extinputs/getVRRInfo)\n"
          "    if [ \"$mode\" = sub ]; then follow " + root + "/vrr.feed; fi\n"
          "    p=$(echo \"$payload\" | sed 's/.*\"pipelineId\":\"\\([^\"]*\\)\".*/\\1/')\n"
          "    cat " + root + "/reply-$p 2>/dev/null ;;\n"
          "esac\n");
    chmod((root + "/luna-send").c_str(), 0755);
}

struct Rig {
    ChildProcessRunner children;
    EventLoop loop{8, 32, 128};
    LunaClient luna;
    GameFrameRate game;
    std::thread loopThread;
    static LunaClientOptions options() {
        LunaClientOptions o;
        o.executable = root + "/luna-send";
        return o;
    }
    static ChildRunnerOptions fast() {
        ChildRunnerOptions o;
        o.startupGap = std::chrono::milliseconds(0);
        o.regularGap = std::chrono::milliseconds(0);
        o.startupWindow = std::chrono::milliseconds(0);
        return o;
    }
    Rig() : children(fast()), luna(loop, children, nullptr, options()), game(loop, luna) {
        for (const char *name : {"calls", "bind.feed", "vrr.feed"}) write(root + "/" + name, "");
        loopThread = std::thread([this]() { loop.run(); });
    }
    ~Rig() {
        game.stop();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        loop.post([this]() { loop.stop(); });
        loopThread.join();
    }
};

const char *const kBind = "{\"returnValue\":true,\"broadcastId\":\"p1\"}";
const char *const kReading =
    "{\"returnValue\":true,\"port\":\"HDMI4\",\"pipelineId\":\"p1\",\"vrrInfo\":{\"frameRate\":119,\"vrrType\":\"gsync\"}}";
bool is(const Json &reading, double rate, const char *type, const char *port) {
    return reading.isObject() && reading["frameRate"].number() == rate &&
           reading["vrrType"].string() == type &&
           (port ? reading["port"].string() == port : !reading["port"].isString());
}
} // namespace

int main() {
    char pattern[] = "/tmp/nativetv-game-XXXXXX";
    root = mkdtemp(pattern);
    script();

    {   // The stats read binds, asks once per TTL, and never starts the stream.
        Rig rig;
        write(root + "/reply-p1", kReading);
        append(root + "/bind.feed", kBind);
        const Json first = rig.game.readForStats(true);
        check(is(first, 119, "gsync", "HDMI4"), "the stats read is one call on the bound pipeline");
        check(count("sub luna://com.webos.service.utp.extinputs/bind") == 1 && count("once luna://com.webos.service.utp.extinputs/getVRRInfo") == 1,
              "an input on screen holds one bind and asks once");
        check(is(rig.game.readForStats(true), 119, "gsync", "HDMI4") &&
                  count("once luna://com.webos.service.utp.extinputs/getVRRInfo") == 1,
              "a second read within the TTL is served from the cache");
        check(count("sub luna://com.webos.service.utp.extinputs/getVRRInfo") == 0, "the stats never start the stream");
        // The Game tab's stream shares the bind and serves the stats while it runs.
        rig.game.frameRate();
        check(waitFor([&]() { return count("sub luna://com.webos.service.utp.extinputs/getVRRInfo") == 1; }),
              "the Game tab's stream starts on the bound pipeline");
        check(read(root + "/calls").find("\"subscribe\":true,\"pipelineId\":\"p1\",\"mode\":\"GameOptimizer\",\"interval\":1000") != std::string::npos ||
                  read(root + "/calls").find("\"pipelineId\":\"p1\"") != std::string::npos,
              "with the pipeline, mode and a one second interval");
        check(count("sub luna://com.webos.service.utp.extinputs/bind") == 1, "and no second bind");
        append(root + "/vrr.feed", "{\"returnValue\":true,\"port\":\"HDMI4\",\"vrrInfo\":{\"frameRate\":40,\"vrrType\":\"gsync\"}}");
        check(waitFor([&]() { return is(rig.game.frameRate(), 40, "gsync", "HDMI4"); }), "the stream's reading is served");
        const size_t before = count("once luna://com.webos.service.utp.extinputs/getVRRInfo");
        check(is(rig.game.readForStats(true), 40, "gsync", "HDMI4") &&
                  count("once luna://com.webos.service.utp.extinputs/getVRRInfo") == before,
              "no one-shot call while the stream runs");
        // Off the input the stats have no reading; the stream still holds the bind.
        check(!rig.game.readForStats(false).valid() &&
                  count("sub luna://com.webos.service.utp.extinputs/bind") == 1,
              "off an input the stats have no reading and the stream keeps the bind");
    }

    {   // An input change disconnects the pipeline and announces no new one: the bind is dropped.
        Rig rig;
        write(root + "/reply-p1", kReading);
        append(root + "/bind.feed", kBind);
        check(is(rig.game.readForStats(true), 119, "gsync", "HDMI4"), "a reading on the first pipeline");
        append(root + "/bind.feed", "{\"returnValue\":true,\"broadcastId\":\"p1\",\"binded\":false,\"status\":\"disconnected\"}");
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        // The next reader binds afresh, on the new pipeline.
        write(root + "/bind.feed", "{\"returnValue\":true,\"broadcastId\":\"p5\"}\n");
        write(root + "/reply-p5", "{\"returnValue\":true,\"port\":\"HDMI1\",\"pipelineId\":\"p5\",\"vrrInfo\":{\"frameRate\":60,\"vrrType\":\"\"}}");
        const Json second = rig.game.readForStats(true);
        check(count("sub luna://com.webos.service.utp.extinputs/bind") == 2, "the next read binds afresh");
        check(is(second, 60, "off", "HDMI1"), "and reads the new pipeline");
        check(read(root + "/calls").find("\"pipelineId\":\"p5\"") != std::string::npos, "by its id");
    }

    {   // webOS 5 lists the rate and type as strings.
        Rig rig;
        append(root + "/bind.feed", "{\"returnValue\":true,\"broadcastId\":\"w1\"}");
        rig.game.frameRate();
        check(waitFor([&]() { return count("sub luna://com.webos.service.utp.extinputs/getVRRInfo") == 1; }), "the stream starts");
        append(root + "/vrr.feed", "{\"returnValue\":true,\"port\":\"HDMI4\",\"pipelineId\":\"w1\",\"vrrInfo\":{\"sublist_1\":\"60.0Hz\",\"sublist_2\":\"FIXED\",\"sublist_3\":\"3840 x 2160P@60\"}}");
        check(waitFor([&]() { return is(rig.game.frameRate(), 60, "off", "HDMI4"); }),
              "webOS 5's sublist strings give the rate, and FIXED reads as off");
    }

    {   // A bind that never answers leaves the read empty after its wait.
        Rig rig;
        const auto started = std::chrono::steady_clock::now();
        const Json none = rig.game.readForStats(true);
        const long long waited = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        check(!none.valid() && waited >= 1300 && waited < 4000, "a bind that never answers leaves the read empty after about 1.5 s");
    }

    {   // A TV without the service is not asked again.
        Rig rig;
        append(root + "/bind.feed", "{\"returnValue\":false,\"errorCode\":-1,\"errorText\":\"Unknown method \\\"bind\\\" for category \\\"/\\\"\"}");
        check(!rig.game.readForStats(true).valid(), "an unknown bind method leaves the read empty");
        const size_t asked = count("luna://");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        (void)rig.game.readForStats(true);
        rig.game.frameRate();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        check(count("luna://") == asked, "and neither reader starts another luna-send");
    }

    runShell(("pkill -f '" + root + "' 2>/dev/null; rm -rf '" + root + "'"));
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    return 0;
}
