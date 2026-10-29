// test/test-samrescan.js: sam is made to read its apps again by nudging configd's
// blocked-app list, which is put back exactly as LG had it; without the list
// (webOS 4) the caller is told to restart sam instead (#366). One app is reread
// with a block and a release, the release always sent after a block and tried
// again if refused. Also the visibility checks and the restart around them.
//
// The TV is tests/sam_fake_tv.py behind a real LunaClient; /bin/sh is a stand-in
// in a fake system root, so a restart never reaches the machine running the test.
#include "daemon/child_process.hpp"
#include "daemon/command_policy.hpp"
#include "daemon/event_loop.hpp"
#include "daemon/luna.hpp"
#include "daemon/sam_rescan_manager.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace nativetv;
using namespace nativetv::daemon;

namespace {
const char kNudgeId[] = "io.github.ihtisham1211.nativetv.rescan";
const char kScreensaver[] = "com.webos.app.screensaver";
const char kHome[] = "com.webos.app.home";

void check(bool value, const std::string &message) {
    if (!value) throw std::runtime_error(message);
}

std::string readFile(const std::string &path) {
    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

void writeFile(const std::string &path, const std::string &text, bool executable = false) {
    {
        std::ofstream output(path.c_str(), std::ios::out | std::ios::trunc | std::ios::binary);
        output << text;
    }
    if (executable) chmod(path.c_str(), 0755);
}

void shell(const std::string &command) {
    const int status = system(command.c_str());
    (void)status;
}

ChildRunnerOptions fastStarts() {
    ChildRunnerOptions options;
    options.startupGap = std::chrono::milliseconds(0);
    options.regularGap = std::chrono::milliseconds(0);
    options.startupWindow = std::chrono::milliseconds(0);
    return options;
}

std::vector<std::string> strings(const Json &array) {
    std::vector<std::string> out;
    for (const Json &item : array.items()) out.push_back(item.string());
    return out;
}

typedef std::vector<std::string> Strings;

// One scripted TV, the daemon's modules over it, and the event loop they run on.
struct Rig {
    std::string directory;
    std::string statePath;
    Json state;
    EventLoop loop;
    ChildProcessRunner children;
    std::unique_ptr<LunaClient> luna;
    std::unique_ptr<CommandPolicyExecutor> policy;
    std::unique_ptr<SamRescanManager> sam;
    std::thread loopThread;
    CommandPolicyContext context;

    // overrides is a JSON object whose keys replace the TV's defaults.
    explicit Rig(const std::string &overrides = "{}", const std::string &schema = std::string())
        : loop(4, 16, 32), children(fastStarts()) {
        char pattern[] = "/tmp/nativetv-sam-XXXXXX";
        check(mkdtemp(pattern) != nullptr, "temporary directory");
        directory = pattern;
        statePath = directory + "/tv.json";
        state = Json::parse(
            "{\"list\":[],\"writes\":[],\"failWrites\":0,\"samCalls\":[],\"samPayloads\":[],"
            "\"hasBlock\":true,\"failReleases\":0,\"blockAnswerLost\":false,\"blocked\":{},"
            "\"uris\":[],\"appInfo\":{},\"launchPoints\":[[\"a\",\"b\"]],"
            "\"foreground\":\"com.webos.app.home\",\"launches\":[]}");
        const Json given = Json::parse(overrides);
        for (const std::string &key : given.keys()) state.set(key, given[key]);
        writeFile(statePath, state.dump());

        writeFile(directory + "/luna-send",
                  std::string("#!/bin/sh\nexec \"") + NATIVETV_PYTHON + "\" \"" +
                      NATIVETV_SAM_FAKE_TV + "\" \"$@\"\n", true);
        // The restart and the dependents check run this in place of /bin/sh.
        mkdir((directory + "/bin").c_str(), 0755);
        writeFile(directory + "/bin/sh",
                  "#!/bin/sh\nd=$(dirname \"$0\")/..\nprintf '%s\\n---\\n' \"$2\" >> \"$d/sh.log\"\n"
                  "exit $(cat \"$d/sh.exit\" 2>/dev/null || echo 0)\n", true);
        if (!schema.empty()) {
            shell("mkdir -p '" + directory + "/etc/palm/schemas/sam'");
            writeFile(directory + "/etc/palm/schemas/sam/applicationManager.blockSystemApp.schema", schema);
        }

        setenv("NATIVETV_SAMFAKE_STATE", statePath.c_str(), 1);
        LunaClientOptions options;
        options.executable = directory + "/luna-send";
        options.timeout = std::chrono::milliseconds(8000);
        luna.reset(new LunaClient(loop, children, nullptr, options));
        policy.reset(new CommandPolicyExecutor(*luna));
        sam.reset(new SamRescanManager(*policy, children, directory));
        loopThread = std::thread([this]() { loop.run(); });
    }
    ~Rig() {
        loop.stop();
        loopThread.join();
        shell("rm -rf '" + directory + "'");
    }
    Json tv() { return Json::parse(readFile(statePath)); }
    size_t shellRuns() const {
        const std::string log = readFile(directory + "/sh.log");
        size_t count = 0;
        for (size_t at = log.find("---"); at != std::string::npos; at = log.find("---", at + 3)) ++count;
        return count;
    }
    void shellExits(int code) { writeFile(directory + "/sh.exit", std::to_string(code)); }
};

bool ok(const Json &result) { return result["ok"].boolean() && result["result"].isBoolean() && result["result"].boolean(); }
bool failed(const Json &result) { return result["ok"].boolean() && result["result"].isBoolean() && !result["result"].boolean(); }

Json nudged(const char *list) {
    Json out = Json::parse(list);
    out.add(Json::text(kNudgeId));
    return out;
}

void testNudge() {
    {   // No list (webOS 4): nothing written, and the caller restarts sam.
        Rig rig("{\"list\":null}");
        check(failed(rig.sam->canNudge(rig.context)), "without the list sam cannot be nudged");
        check(failed(rig.sam->nudge(rig.context)), "the nudge says so");
        check(rig.tv()["writes"].size() == 0, "nothing written to configd");
    }
    {   // LG's own entries are put back exactly, after the nudge id was added.
        Rig rig("{\"list\":[\"com.webos.app.roomconnect\",\"netflix\"]}");
        check(ok(rig.sam->canNudge(rig.context)), "the list is there");
        check(ok(rig.sam->nudge(rig.context)), "the nudge goes through");
        const Json tv = rig.tv();
        check(tv["writes"].size() == 2, "one write to nudge, one to restore");
        check(tv["writes"].items()[0].dump() == nudged("[\"com.webos.app.roomconnect\",\"netflix\"]").dump(),
              "the nudge adds an id no app has");
        check(tv["list"].dump() == "[\"com.webos.app.roomconnect\",\"netflix\"]", "LG's list is back exactly");
    }
    {   // A nudge id left by a run cut short is not kept as LG's.
        Rig rig(std::string("{\"list\":[\"netflix\",\"") + kNudgeId + "\"]}");
        check(ok(rig.sam->nudge(rig.context)), "nudged");
        const Json tv = rig.tv();
        check(tv["writes"].items()[0].dump() == nudged("[\"netflix\"]").dump(), "one nudge id, not two");
        check(tv["list"].dump() == "[\"netflix\"]", "a nudge id left behind is dropped, not put back");
    }
    {   // Two at once: the second does not read the list mid-nudge.
        Rig rig("{\"list\":[\"netflix\"]}");
        Json first, second;
        std::thread other([&]() { first = rig.sam->nudge(rig.context); });
        second = rig.sam->nudge(rig.context);
        other.join();
        check(ok(first) && ok(second), "both nudges go through");
        const Json tv = rig.tv();
        check(tv["writes"].size() == 4, "two writes each");
        const std::vector<Json> writes = tv["writes"].items();
        for (size_t index = 0; index < writes.size(); ++index) {
            size_t ids = 0;
            for (const std::string &id : strings(writes[index])) ids += id == kNudgeId;
            check(ids == (index % 2 == 0 ? 1u : 0u), "nudges run one after another, one id at most");
        }
        check(tv["list"].dump() == "[\"netflix\"]", "the list is back");
    }
    {   // A failed restore is tried again.
        Rig rig("{\"list\":[\"netflix\"],\"failWrites\":1}");
        check(ok(rig.sam->nudge(rig.context)), "putting the list back is tried again if configd refuses it");
        check(rig.tv()["writes"].size() == 3 && rig.tv()["list"].dump() == "[\"netflix\"]", "and then it is back");
    }
    {   // A restore configd never takes is reported, with the nudge id still in the list.
        Rig rig("{\"list\":[\"netflix\"],\"failWrites\":9}");
        const Json result = rig.sam->nudge(rig.context);
        check(failed(result) && result["error"].string().find("clears at reboot") != std::string::npos,
              "the failed restore is reported");
        check(rig.tv()["writes"].size() == 4, "the restore is tried three times");
    }
    {   // The controls gate holds for the nudge as for any write.
        Rig rig("{\"list\":[\"netflix\"]}");
        rig.context.allowControl = false;
        check(failed(rig.sam->nudge(rig.context)) && failed(rig.sam->canNudge(rig.context)),
              "controls off refuses");
        check(rig.tv()["uris"].size() == 0, "nothing reached the TV");
    }
}

void testRereadApp() {
    {   // One app: a block then a release, nothing written to configd.
        Rig rig;
        check(ok(rig.sam->rereadApp(kScreensaver, rig.context)), "reread");
        const Json tv = rig.tv();
        check(strings(tv["samCalls"]) == Strings({std::string("blockSystemApp ") + kScreensaver,
                                                  std::string("releaseBlockSystemApp ") + kScreensaver}),
              "a block then a release");
        check(!tv["blocked"][kScreensaver].boolean(), "released");
        check(tv["writes"].size() == 0, "the configd list is not touched");
        check(!tv["samPayloads"].items()[0].has("forceBlockSystemApp"), "no force flag where the schema lacks it");
    }
    {   // Firmware whose schema has the force flag is sent it, as false.
        Rig rig("{}", "{\"properties\":{\"forceBlockSystemApp\":{}}}");
        check(ok(rig.sam->rereadApp(kHome, rig.context)), "reread");
        const Json sent = rig.tv()["samPayloads"].items()[0];
        check(sent.has("forceBlockSystemApp") && !sent["forceBlockSystemApp"].boolean(), "force flag false");
    }
    {   // Only the two apps sam is asked about.
        Rig rig;
        const Json result = rig.sam->rereadApp("netflix", rig.context);
        check(failed(result) && result["error"].string() == "unsupported SAM app id", "other apps refused");
        check(rig.tv()["uris"].size() == 0, "nothing reached the TV");
    }
    {   // sam without the call (webOS 4): the block is refused, one release goes out quietly.
        Rig rig("{\"hasBlock\":false}");
        check(failed(rig.sam->rereadApp(kScreensaver, rig.context)), "the caller falls back");
        check(strings(rig.tv()["samCalls"]) == Strings({std::string("blockSystemApp ") + kScreensaver,
                                                        std::string("releaseBlockSystemApp ") + kScreensaver}),
              "after one release");
    }
    {   // A block sam made whose answer was lost is released all the same.
        Rig rig("{\"blockAnswerLost\":true}");
        check(failed(rig.sam->rereadApp(kScreensaver, rig.context)), "the caller falls back");
        check(!rig.tv()["blocked"][kScreensaver].boolean(), "not left blocked");
    }
    {   // A refused release is tried again, so the app is not left blocked.
        Rig rig("{\"failReleases\":2}");
        check(ok(rig.sam->rereadApp(kScreensaver, rig.context)), "reread in the end");
        const Json tv = rig.tv();
        check(tv["samCalls"].size() == 4, "one block, three releases");
        check(!tv["blocked"][kScreensaver].boolean(), "released in the end");
    }
    {   // A release sam keeps refusing is reported: the app stays blocked.
        Rig rig("{\"failReleases\":9}");
        const Json result = rig.sam->rereadApp(kScreensaver, rig.context);
        check(failed(result) && result["error"].string().find("could not release") != std::string::npos,
              "the release failure is reported");
        check(rig.tv()["samCalls"].size() == 4 && rig.tv()["blocked"][kScreensaver].boolean(), "three releases tried");
    }
    {   // releaseBlockedApp is the same release, for a caller that blocked the app itself.
        Rig rig("{\"failReleases\":2}");
        check(rig.sam->releaseBlockedApp(kHome, rig.context), "released");
        check(!rig.sam->releaseBlockedApp("netflix", rig.context), "only the two apps");
        Rig stuck("{\"failReleases\":9}");
        check(!stuck.sam->releaseBlockedApp(kHome, stuck.context), "a refused release is reported");
    }
}

void testWaitingForSam() {
    {   // The caller waits for sam to report the type; it gives up if sam never does.
        Rig rig("{\"appInfo\":{\"com.webos.app.screensaver\":[{\"type\":\"flutter\"},{\"type\":\"qml\"}]}}");
        check(rig.sam->reportsAppType(kScreensaver, "qml", rig.context, 5), "it waits for sam to have the change");
        check(rig.tv()["uris"].size() == 2, "asked until sam had it");
        Rig never("{\"appInfo\":{\"com.webos.app.screensaver\":[{\"type\":\"flutter\"}]}}");
        check(!never.sam->reportsAppType(kScreensaver, "qml", never.context, 3), "and gives up if it never does");
        check(never.tv()["uris"].size() == 3, "after the attempts it was given");
        check(!never.sam->reportsAppType("netflix", "qml", never.context, 3), "only the two apps");
        check(!never.sam->reportsAppType(kScreensaver, "", never.context, 3), "a type is needed");
    }
    {   // The app list nudged: sam has the change at the second look, and the list is back.
        Rig rig("{\"list\":[\"netflix\"],\"appInfo\":{\"com.example.hidden\":[{\"visible\":true},{\"visible\":false}]}}");
        check(ok(rig.sam->refreshAppVisibility("com.example.hidden", false, rig.context, 5)), "sam caught up");
        check(rig.tv()["list"].dump() == "[\"netflix\"]" && rig.shellRuns() == 0, "no restart needed");
    }
    {   // sam never has it: sam is restarted, and the list is back even so.
        Rig rig("{\"list\":[\"netflix\"],\"appInfo\":{\"com.example.hidden\":[{\"visible\":true}]}}");
        check(ok(rig.sam->refreshAppVisibility("com.example.hidden", false, rig.context, 3)), "restarted instead");
        check(rig.tv()["list"].dump() == "[\"netflix\"]", "the list is back");
        check(rig.shellRuns() == 2, "sam restarted, then its dependents looked at");
    }
    {   // Bad input is refused before the TV is asked.
        Rig rig;
        check(failed(rig.sam->refreshAppVisibility("../etc", true, rig.context)), "a bad app id");
        check(failed(rig.sam->refreshAppVisibility("com.example.app", true, rig.context, 61)), "too many attempts");
        check(rig.tv()["uris"].size() == 0, "nothing reached the TV");
    }
}

void testRefresh() {
    {   // Waits for sam to catch up with every app, then for the launch points to settle.
        Rig rig("{\"list\":[\"netflix\"],\"appInfo\":{\"netflix\":[{\"visible\":true},{\"visible\":true},{\"visible\":false}]}}");
        check(ok(rig.sam->refresh(Json::parse("{\"netflix\":false}"), rig.context)), "refreshed");
        const Json tv = rig.tv();
        check(tv["list"].dump() == "[\"netflix\"]", "the list is back");
        check(rig.shellRuns() == 0, "sam was not restarted");
        size_t looks = 0;
        for (const std::string &uri : strings(tv["uris"])) looks += uri.find("getAppInfo") != std::string::npos;
        check(looks == 3, "asked until sam had it");
    }
    {   // Never catches up: sam is restarted so the caller does not have to.
        Rig rig("{\"list\":[\"netflix\"],\"appInfo\":{\"netflix\":[{\"visible\":true}]}}");
        check(ok(rig.sam->refresh(Json::parse("{\"netflix\":false}"), rig.context)), "restarted instead");
        check(rig.shellRuns() == 2, "sam restarted");
        check(rig.tv()["list"].dump() == "[\"netflix\"]", "the list is back even so");
    }
    {   // No list (webOS 4): straight to the restart.
        Rig rig("{\"list\":null}");
        check(ok(rig.sam->refresh(Json::parse("{}"), rig.context)), "restarted");
        check(rig.shellRuns() == 2 && rig.tv()["writes"].size() == 0, "nothing nudged");
    }
    {   // Bad expectations are refused whole.
        Rig rig("{\"list\":[]}");
        check(failed(rig.sam->refresh(Json::text("x"), rig.context)), "not an object");
        Json many = Json::object();
        for (int index = 0; index < 65; ++index) many.set("app." + std::to_string(index), Json::flag(true));
        const Json tooMany = rig.sam->refresh(many, rig.context);
        check(failed(tooMany) && tooMany["error"].string() == "too many app visibility checks", "more than 64");
        check(rig.tv()["uris"].size() == 0, "nothing reached the TV");
        const Json wrong = rig.sam->refresh(Json::parse("{\"netflix\":\"no\"}"), rig.context);
        check(failed(wrong) && wrong["error"].string() == "expected app visibility must be boolean", "not a boolean");
    }
}

void testRestart() {
    {   // The app on screen comes back after sam restarts; LG's own Home does not need to.
        Rig rig("{\"foreground\":\"netflix\"}");
        check(ok(rig.sam->restart(rig.context)), "restarted");
        check(rig.shellRuns() == 2, "restart script, then the dependents");
        check(strings(rig.tv()["launches"]) == Strings({"netflix"}), "netflix is launched again");
        Rig home("{\"foreground\":\"com.webos.app.home\"}");
        check(ok(home.sam->restart(home.context)), "restarted");
        check(home.tv()["launches"].size() == 0, "Home needs no launch");
    }
    {   // A restart that fails is reported with its reason.
        Rig rig;
        rig.shellExits(1);
        const Json result = rig.sam->restart(rig.context);
        check(failed(result) && !result["error"].string().empty(), "the failure is reported");
    }
    {   // Controls off still refuses: a restart is a write.
        Rig rig;
        rig.context.allowControl = false;
        check(failed(rig.sam->restart(rig.context)), "refused");
        check(rig.shellRuns() == 0, "nothing run");
    }
}
} // namespace

int main() {
    try {
        testNudge();
        testRereadApp();
        testWaitingForSam();
        testRefresh();
        testRestart();
    } catch (const std::exception &error) {
        fprintf(stderr, "sam_rescan_manager: %s\n", error.what());
        return 1;
    }
    return 0;
}
