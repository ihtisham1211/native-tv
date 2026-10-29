// test/test-homescreen.js and test-homescreen-webos10.js, for the parts the
// native HomeScreenManager owns: LG's Home screen parts are hidden by writing a
// copy of its own layout without them (webOS 9) or an overlay holding a changed
// home.xml, once the experimental switch is on (webOS 10); putting LG's Home
// screen back removes only what was added here. The launch points of the input
// tiles are the front door's, covered with the Home screen routes.
//
// Everything runs in a fake root. The overlay mount is a stand-in script, and
// the "home app" signalled on a restart is a sleeping child of this test.
#include "daemon/child_process.hpp"
#include "daemon/home_screen_manager.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <regex>
#include <signal.h>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using namespace nativetv;
using namespace nativetv::daemon;

namespace {
const char kMark[] = "<!-- nativetv -->";
const char kTakeoverMark[] = "<!-- nativetv-home-starting -->";

// LG's built-in layout, as a C2 (webOS 9.2.2) has it.
const char kLgLayout9[] =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<home version=\"2.0\">\n"
    "    <layout type=\"ShelfView\" vMargin=\"30\">\n"
    "        <firstPage>\n"
    "             <item type=\"Hero\" itemX=\"10\" itemY=\"24\" uiWidth=\"1896\" uiHeight=\"450\"  itemWidth=\"1896\" itemHeight=\"488\" focus=\"true\"/>\n"
    "             <item type=\"Qcard\" itemX=\"24\" itemY=\"770\" itemWidth=\"1872\" itemHeight=\"120\" focus=\"true\" option=\"simple\"/>\n"
    "             <item type=\"AppList\" itemX=\"24\" itemY=\"850\" itemWidth=\"1872\" itemHeight=\"173\" focus=\"true\" option=\"webOS24\"/>\n"
    "             <item type=\"AdShelf\" itemX=\"430\" itemY=\"220\" itemWidth=\"1920\" itemHeight=\"350\"/>\n"
    "        </firstPage>\n"
    "    </layout>\n"
    "</home>\n";

// home.xml as a C2 on 10.3.1 and a B4 on 10.3.2 have it (#612).
const char kLgLayout10[] =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<home version=\"2.0\">\n"
    "\t<layout windowType=\"overlay\" pageType=\"none\" pageCount=\"1\" defaultPage=\"0\">\n"
    "\t\t<page pageBodyType=\"container\">\n"
    "\t\t\t<item id=\"margin\" itemWidth=\"3840\" itemHeight=\"45\" focusType=\"none\"/>\n"
    "\t\t\t<item id=\"container\" hasChildren=\"true\" itemWidth=\"3840\" itemHeight=\"900\" focusType=\"scope\">\n"
    "\t\t\t\t<item id=\"globalline\" itemWidth=\"300\" itemHeight=\"900\" focusType=\"scope\" autoFocus=\"false\"/>\n"
    "\t\t\t\t<item id=\"herobanner\" itemWidth=\"3492\" itemHeight=\"900\" focusType=\"scope\" autoFocus=\"false\"/>\n"
    "\t\t\t</item>\n"
    "\t\t\t<item id=\"margin\" itemWidth=\"3840\" itemHeight=\"74\" focusType=\"none\"/>\n"
    "\t\t\t<item id=\"qcardList\" itemWidth=\"3840\" itemHeight=\"180\" focusType=\"scope\" autoFocus=\"false\"/>\n"
    "\t\t\t<item id=\"margin\" itemWidth=\"3840\" itemHeight=\"72\" focusType=\"none\"/>\n"
    "\t\t\t<item id=\"appList\" itemWidth=\"3840\" itemHeight=\"248\" focusType=\"scope\" autoFocus=\"true\"/>\n"
    "\t\t\t<item id=\"margin\" itemWidth=\"3840\" itemHeight=\"48\" focusType=\"none\"/>\n"
    "\t\t\t<item id=\"recommendedShelf\" itemWidth=\"3798\" itemHeight=\"531\" focusType=\"scope\" autoFocus=\"false\"/>\n"
    "\t\t\t<item id=\"quickGuide\" itemX=\"0\" itemY=\"0\" itemWidth=\"0\" itemHeight=\"0\" focusType=\"none\"/>\n"
    "\t\t</page>\n"
    "\t</layout>\n"
    "</home>";

void check(bool value, const std::string &message) {
    if (!value) throw std::runtime_error(message);
}

void shell(const std::string &command) {
    const int status = system(command.c_str());
    (void)status;
}

std::string readFile(const std::string &path) {
    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

bool exists(const std::string &path) {
    struct stat info;
    return lstat(path.c_str(), &info) == 0;
}

void writeFile(const std::string &path, const std::string &text, bool executable = false) {
    shell("mkdir -p '" + path.substr(0, path.find_last_of('/')) + "'");
    {
        std::ofstream output(path.c_str(), std::ios::out | std::ios::trunc | std::ios::binary);
        output << text;
    }
    if (executable) chmod(path.c_str(), 0755);
}

bool has(const std::string &text, const std::string &needle) { return text.find(needle) != std::string::npos; }

ChildRunnerOptions fastStarts() {
    ChildRunnerOptions options;
    options.startupGap = std::chrono::milliseconds(0);
    options.regularGap = std::chrono::milliseconds(0);
    options.startupWindow = std::chrono::milliseconds(0);
    return options;
}

// A fake TV root with one Home screen manager over it.
struct Rig {
    std::string root;
    std::string data;
    std::string proc;
    ChildProcessRunner children;
    std::unique_ptr<HomeScreenManager> manager;

    Rig(const std::string &version, bool allowExperimental = false) : children(fastStarts()) {
        char pattern[] = "/tmp/nativetv-homescreen-XXXXXX";
        check(mkdtemp(pattern) != nullptr, "temporary directory");
        root = pattern;
        data = root + "/data";
        proc = root + "/proc-self";
        shell("mkdir -p '" + data + "' '" + proc + "'");
        writeFile(root + "/etc/issue", "webOS " + version + "\n");
        manager.reset(new HomeScreenManager(children, root, data, proc, allowExperimental));
    }
    ~Rig() { shell("rm -rf '" + root + "'"); }

    std::string home() const { return root + "/usr/palm/applications/com.webos.app.home"; }
    // webOS 9
    std::string layoutDir() const { return root + "/mnt/lg/cmn_data/manifest"; }
    std::string layoutFile() const { return layoutDir() + "/home_layoutShelfView.xml"; }
    std::string layoutAlias() const { return layoutDir() + "/home_layout.xml"; }
    void seed9() {
        writeFile(home() + "/qml/UserInterfaceLayer/home_layoutShelfView.xml", kLgLayout9);
        writeFile(home() + "/qml/UserInterfaceLayer/Containers/MainView_M.qml",
                  "property var xmlResources: ({ path: \"/mnt/lg/cmn_data/manifest/\", defaultPath: \"../\" })\n");
    }
    // webOS 10
    std::string assets() const { return home() + "/data/flutter_assets/assets"; }
    void seed10(const std::string &layout = kLgLayout10) {
        writeFile(assets() + "/home.xml", layout);
        writeFile(root + "/proc/mounts", "");
        writeFile(root + "/proc/filesystems", "nodev\ttmpfs\nnodev\toverlay\n");
        // A mount of the overlay marks the folder mounted and keeps the home.xml
        // it was given; an unmount clears it.
        writeFile(root + "/bin/mount",
                  "#!/bin/sh\nr=$(dirname \"$0\")/..\nfor a; do last=$a; done\n"
                  "[ -f \"$r/mount.fail\" ] && { echo 'mount: denied' >&2; exit 1; }\n"
                  "printf '%s\\n' \"$*\" >> \"$r/mount.log\"\n"
                  "upper=$(echo \"$5\" | sed 's/.*upperdir=\\([^,]*\\).*/\\1/')\n"
                  "cp \"$upper/home.xml\" \"$r/mounted-home.xml\"\n"
                  "printf 'overlay %s overlay rw 0 0\\n' \"$last\" > \"$r/proc/mounts\"\n", true);
        writeFile(root + "/bin/umount",
                  "#!/bin/sh\nr=$(dirname \"$0\")/..\necho umount >> \"$r/umount.log\"\n: > \"$r/proc/mounts\"\n", true);
    }
    size_t lines(const std::string &name) const {
        const std::string text = readFile(root + "/" + name);
        size_t count = 0;
        for (char character : text) count += character == '\n';
        return count;
    }
    void unmountedByRestart() { writeFile(root + "/proc/mounts", ""); }
};

// The rows of a webOS 10 layout stacked from their heights.
struct Row { std::string id; unsigned height; };
std::vector<Row> rows(const std::string &xml) {
    std::vector<Row> list;
    int depth = 0;
    size_t start = 0;
    while (start <= xml.size()) {
        size_t end = xml.find('\n', start);
        if (end == std::string::npos) end = xml.size();
        const std::string line = xml.substr(start, end - start);
        start = end + 1;
        if (has(line, "</item>")) { --depth; continue; }
        std::smatch match;
        if (std::regex_search(line, match, std::regex("<item id=\"(\\w+)\"[^>]*itemHeight=\"(\\d+)\"")) && depth == 0)
            list.push_back({match[1].str(), static_cast<unsigned>(std::stoul(match[2].str()))});
        if (has(line, "hasChildren=\"true\"")) ++depth;
    }
    return list;
}
unsigned total(const std::vector<Row> &list) {
    unsigned sum = 0;
    for (const Row &row : list) sum += row.height;
    return sum;
}
unsigned belowApps(const std::vector<Row> &list) {
    unsigned sum = 0;
    bool after = false;
    for (const Row &row : list) {
        if (after) sum += row.height;
        if (row.id == "appList") after = true;
    }
    return sum;
}
std::string attribute(const std::string &xml, const std::string &id, const std::string &name) {
    std::smatch match;
    const std::regex pattern("<item id=\"" + id + "\"[^>]*" + name + "=\"(\\d+)\"");
    return std::regex_search(xml, match, pattern) ? match[1].str() : std::string();
}

std::string partsOf(const Json &state) {
    std::string out;
    for (const Json &part : state["parts"].items())
        out += (out.empty() ? "" : " ") + part["id"].string() + ":" + (part["shown"].boolean() ? "true" : "false");
    return out;
}

void testWebos9() {
    {   // Every part shown on webOS 9, and no section on webOS 4.
        Rig rig("9.2.2");
        rig.seed9();
        const Json state = rig.manager->state();
        check(state["supported"].boolean() && state["allowed"].boolean(), "webOS 9 offers the section");
        check(partsOf(state) == "banner:true quickCards:true apps:true deviceFunctions:true", "every part shows");
        check(rig.manager->isWebos9() && rig.manager->supportsTakeover(false), "webOS 9 supports the takeover");
        Rig old("4.4.3");
        old.seed9();
        check(!old.manager->state()["supported"].boolean() && old.manager->state()["parts"].size() == 0,
              "webOS 4 has no Home screen section");
        check(!old.manager->isWebos9() && !old.manager->supportsTakeover(true), "nor a takeover");
    }
    {   // Hiding a part writes LG's layout without it, marked, under both names.
        Rig rig("9.2.2");
        rig.seed9();
        const Json result = rig.manager->setPart("quickCards", false);
        check(result["ok"].boolean() && result["changed"].boolean() && result["restart"].boolean(), "hidden");
        const std::string xml = readFile(rig.layoutFile());
        check(has(xml, kMark), "marked as ours");
        check(!has(xml, "type=\"Qcard\"") && has(xml, "type=\"Hero\"") && has(xml, "type=\"AppList\""),
              "only the one part is gone");
        check(readFile(rig.layoutAlias()) == xml, "both names written");
        check(partsOf(rig.manager->state()) == "banner:true quickCards:false apps:true deviceFunctions:true",
              "the state follows the layout");
        check(readFile(rig.home() + "/qml/UserInterfaceLayer/home_layoutShelfView.xml") == kLgLayout9,
              "LG's own layout is not touched");

        // Showing it again that makes all four shown removes the copy.
        const Json again = rig.manager->setPart("quickCards", true);
        check(again["ok"].boolean() && !exists(rig.layoutFile()) && !exists(rig.layoutDir()),
              "all four shown puts LG's layout back");
        check(!rig.manager->setPart("nonsense", false)["ok"].boolean(), "no such part");
    }
    {   // The last part showing cannot be hidden.
        Rig rig("9.2.2");
        rig.seed9();
        check(rig.manager->setPart("banner", false)["ok"].boolean() &&
                  rig.manager->setPart("deviceFunctions", false)["ok"].boolean() &&
                  rig.manager->setPart("quickCards", false)["ok"].boolean(), "three hidden");
        const Json last = rig.manager->setPart("apps", false);
        check(!last["ok"].boolean() && !last["error"].string().empty(), "the last part stays");
        check(has(readFile(rig.layoutFile()), "type=\"AppList\""), "the layout still shows the apps");
    }
    {   // While nativeTv Home has LG's Home's place, its layout is not changed.
        Rig rig("9.2.2");
        rig.seed9();
        check(rig.manager->setPart("quickCards", false)["ok"].boolean(), "hidden");
        const std::string starting = std::string("<home>") + kTakeoverMark + "</home>";
        writeFile(rig.layoutFile(), starting);
        const Json held = rig.manager->setPart("banner", true);
        check(!held["ok"].boolean() && has(held["error"].string(), "nativeTv Home"), "refused");
        check(readFile(rig.layoutFile()) == starting, "its layout is left in place");
    }
    {   // Putting LG's Home screen back removes only the layout written here.
        Rig rig("9.2.2");
        rig.seed9();
        check(rig.manager->setPart("quickCards", false)["ok"].boolean(), "hidden");
        const Json back = rig.manager->restore();
        check(back["ok"].boolean() && back["changed"].boolean(), "restored");
        check(!exists(rig.layoutFile()) && !exists(rig.layoutAlias()) && !exists(rig.layoutDir()),
              "layout files and the empty folder are gone");
        check(!rig.manager->restore()["changed"].boolean(), "nothing to put back the second time");
    }
    {   // A layout of LG's own in that folder is left alone, and the section is not offered.
        Rig rig("9.2.2");
        rig.seed9();
        writeFile(rig.layoutFile(), kLgLayout9);
        check(!rig.manager->state()["supported"].boolean(), "not offered");
        const Json back = rig.manager->restore();
        check(back["ok"].boolean() && !back["changed"].boolean() && readFile(rig.layoutFile()) == kLgLayout9,
              "LG's file untouched");
    }
    {   // A layout LG changed shape in is not recognised.
        Rig rig("9.2.2");
        writeFile(rig.home() + "/qml/UserInterfaceLayer/home_layoutShelfView.xml",
                  "<home><layout><firstPage><item type=\"Hero\"/></firstPage></layout></home>\n");
        writeFile(rig.home() + "/qml/UserInterfaceLayer/Containers/MainView_M.qml", "/mnt/lg/cmn_data/manifest/");
        check(!rig.manager->state()["supported"].boolean(), "a layout without the four parts is left alone");
        check(!rig.manager->setPart("banner", false)["ok"].boolean(), "and not changed");
    }
}

void testStartLayout() {
    {   // The empty starting layout is kept over LG's, which comes back exactly.
        Rig rig("9.2.2");
        rig.seed9();
        const std::string lgs = "<home>LG's own home_layout.xml</home>";
        writeFile(rig.layoutAlias(), lgs);
        check(rig.manager->startLayout(true)["blank"].boolean(), "blank");
        check(has(readFile(rig.layoutFile()), kTakeoverMark) && has(readFile(rig.layoutAlias()), kTakeoverMark),
              "both names hold the empty layout");
        check(readFile(rig.data + "/home-takeover-lg-layout/home_layout.xml") == lgs, "LG's file kept");
        check(exists(rig.data + "/home-takeover-lg-layout/home_layoutShelfView.xml.none"), "an absent one noted");
        check(rig.manager->startLayout(true)["ok"].boolean() &&
                  readFile(rig.data + "/home-takeover-lg-layout/home_layout.xml") == lgs,
              "starting twice keeps the first copy");
        const Json back = rig.manager->startLayout(false);
        check(back["ok"].boolean() && !back["blank"].boolean(), "restored");
        check(readFile(rig.layoutAlias()) == lgs && !exists(rig.layoutFile()), "LG's files are back as they were");
        check(!exists(rig.data + "/home-takeover-lg-layout"), "the copies are gone");
        check(rig.manager->startLayout(false)["ok"].boolean(), "restoring with nothing kept is fine");
    }
    {   // The takeover's layout stands in the way of the parts.
        Rig rig("9.2.2");
        rig.seed9();
        check(rig.manager->startLayout(true)["ok"].boolean(), "blank");
        check(has(rig.manager->setPart("quickCards", false)["error"].string(), "nativeTv Home"), "refused");
    }
}

void testInputs() {
    Rig rig("9.2.2");
    rig.seed9();
    check(rig.manager->setAddedInputs(Json::parse("{\"hdmi2\":\"100\"}"))["ok"].boolean(), "recorded");
    check(rig.manager->state()["addedInputs"]["hdmi2"].string() == "100", "the state names the launch point");
    check(!rig.manager->setAddedInputs(Json::parse("{\"hdmi9\":\"100\"}"))["ok"].boolean(), "only HDMI 1 to 4");
    check(!rig.manager->setAddedInputs(Json::parse("{\"hdmi1\":5}"))["ok"].boolean(), "a launch point is text");
    check(!rig.manager->setAddedInputs(Json::parse("{\"hdmi1\":\"\"}"))["ok"].boolean(), "and not empty");
    check(!rig.manager->setAddedInputs(Json::parse("[]"))["ok"].boolean(), "the state is an object");
    check(rig.manager->state()["addedInputs"]["hdmi2"].string() == "100", "a refusal leaves the record");
    check(rig.manager->setAddedInputs(Json::object())["ok"].boolean() && !exists(rig.data + "/home-inputs.json"),
          "no tiles added, no file");
}

pid_t sleeper() {
    const pid_t pid = fork();
    check(pid >= 0, "fork");
    if (pid == 0) {
        execl("/bin/sleep", "sleep", "30", static_cast<char *>(nullptr));
        _exit(127);
    }
    return pid;
}

void testStopHome() {
    // The home app as a C2 on 10.3.1 runs it, an older name, and apps that only look alike.
    Rig rig("10.3.1");
    const pid_t home = sleeper(), older = sleeper(), saver = sleeper(), shellLookalike = sleeper();
    const auto cmdline = [](const std::vector<std::string> &arguments) {
        std::string joined;
        for (const std::string &argument : arguments) joined += argument + '\0';
        return joined;
    };
    const struct { pid_t pid; std::string cmdline; } entries[] = {
        {home, cmdline({"/usr/bin/flutter-client", "-i", "com.webos.app.home"})},
        {older, cmdline({"com.webos.app.home"})},
        {saver, cmdline({"/usr/bin/flutter-client", "-i", "com.webos.app.screensaver"})},
        {shellLookalike, cmdline({"/bin/sh", "-c", "echo -i com.webos.app.home"})}};
    for (const auto &entry : entries) {
        const std::string directory = rig.proc + "/" + std::to_string(entry.pid);
        shell("mkdir -p '" + directory + "'");
        writeFile(directory + "/cmdline", entry.cmdline);
    }
    shell("mkdir -p '" + rig.proc + "/self'");
    writeFile(rig.proc + "/self/cmdline", "");
    // Checkpoints the home app would restore its old state from.
    const char *const checkpoints[] = {"/tmp/checkpoint/com.webos.app.home", "/tmp/checkpoint/hash_check_com.webos.app.home",
                                       "/mnt/lg/cmn_data/checkpoint/com.webos.app.home"};
    for (const char *checkpoint : checkpoints) writeFile(rig.root + checkpoint + "/state", "x");
    writeFile(rig.root + "/tmp/checkpoint/com.webos.app.other/state", "x");

    bool stopped = false;
    std::string error;
    const bool done = rig.manager->stopHome(stopped, &error);
    int status = 0;
    const pid_t reaped = waitpid(home, &status, 0);
    const bool homeSignalled = reaped == home && WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM;
    status = 0;
    const bool olderSignalled = waitpid(older, &status, 0) == older && WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM;
    const bool saverAlive = waitpid(saver, &status, WNOHANG) == 0;
    const bool lookalikeAlive = waitpid(shellLookalike, &status, WNOHANG) == 0;
    kill(saver, SIGKILL);
    kill(shellLookalike, SIGKILL);
    waitpid(saver, &status, 0);
    waitpid(shellLookalike, &status, 0);
    check(done && stopped, "the home app was stopped: " + error);
    check(homeSignalled && olderSignalled, "the home app under either name got SIGTERM");
    check(saverAlive && lookalikeAlive, "no other Flutter app or shell was signalled");
    for (const char *checkpoint : checkpoints) check(!exists(rig.root + checkpoint), "the checkpoint is dropped");
    check(exists(rig.root + "/tmp/checkpoint/com.webos.app.other/state"), "another app's checkpoint stays");

    // The processes are gone: forget them, as a pid could be reused.
    shell("rm -rf '" + rig.proc + "'/*");
    bool again = true;
    check(rig.manager->stopHome(again, nullptr) && !again, "nothing running is not an error");
    check(rig.manager->restartHome(nullptr), "restartHome is the same stop");
}

Json hide(Rig &rig, const std::vector<std::string> &parts) {
    Json result;
    for (const std::string &part : parts) result = rig.manager->setPart(part, false);
    return result;
}

void testWebos10() {
    {   // Offered on webOS 10 only, where the layout is the one recognised.
        Rig nine("9.2.2");
        nine.seed10();
        check(!nine.manager->state()["supported"].boolean(), "webOS 9 has its own layout");
        Rig eleven("11.2.0");
        eleven.seed10();
        check(!eleven.manager->state()["supported"].boolean(), "webOS 11 is not offered yet");
        check(!eleven.manager->supportsTakeover(false) && eleven.manager->supportsTakeover(true),
              "the takeover on webOS 11 needs the experimental switch");
        Rig ten("10.3.1");
        ten.seed10();
        const Json state = ten.manager->state();
        check(state["supported"].boolean() && state["experimental"].boolean() && !state["allowed"].boolean(),
              "webOS 10 offers it, experimental, not allowed yet");
        check(partsOf(state) == "sideIcons:true quickCards:true recommended:true", "three parts");
        Rig odd("10.3.1");
        odd.seed10("<home><item id=\"container\" hasChildren=\"true\"/></home>");
        check(!odd.manager->state()["supported"].boolean(), "an unrecognised home.xml is left alone");
        Rig noOverlay("10.3.1");
        noOverlay.seed10();
        writeFile(noOverlay.root + "/proc/filesystems", "nodev\ttmpfs\n");
        check(!noOverlay.manager->state()["supported"].boolean(), "no overlay filesystem, no section");
    }
    {   // Nothing changes until the experimental switch is on.
        Rig rig("10.3.1");
        rig.seed10();
        const Json refused = rig.manager->setPart("quickCards", false);
        check(!refused["ok"].boolean() && has(refused["error"].string(), "experimental"), "refused");
        check(rig.lines("mount.log") == 0 && !exists(rig.data + "/home-webos10.json"), "nothing mounted or saved");
    }
    {   // Hiding a part mounts LG's layout without it over the folder.
        Rig rig("10.3.1", true);
        rig.seed10();
        const Json result = rig.manager->setPart("quickCards", false);
        check(result["ok"].boolean() && result["restart"].boolean(), "hidden");
        check(has(readFile(rig.root + "/mount.log"), "lowerdir=" + rig.assets()), "LG's folder underneath");
        const std::string mounted = readFile(rig.root + "/mounted-home.xml");
        check(!has(mounted, "qcardList") && has(mounted, "recommendedShelf"), "only the quick cards are out");
        check(readFile(rig.data + "/home-webos10-lg.xml") == kLgLayout10, "LG's copy kept");
        check(readFile(rig.assets() + "/home.xml") == kLgLayout10, "LG's folder itself is not written");
        check(partsOf(rig.manager->state()) == "sideIcons:true quickCards:false recommended:true", "the state follows");

        // The side buttons are shrunk, not removed, after the banner, and LG's copy
        // stands in for the folder now that the overlay covers it.
        check(rig.manager->setPart("sideIcons", false)["ok"].boolean(), "side icons hidden");
        const std::string both = readFile(rig.root + "/mounted-home.xml");
        const std::regex shrunk("id=\"globalline\"[^>]*itemWidth=\"0\"[^>]*itemHeight=\"0\"");
        check(std::regex_search(both, shrunk), "shrunk to nothing");
        check(both.find("herobanner") < both.find("globalline"), "after the banner");
        check(!has(both, "qcardList"), "the quick cards are still out, read from LG's copy while mounted");
        check(rig.lines("umount.log") == 1, "the first overlay was unmounted before the second");

        // A restart of the TV: the mount is gone, and the server's start puts it back.
        rig.unmountedByRestart();
        const size_t mounts = rig.lines("mount.log");
        const Json reapplied = rig.manager->reapply();
        check(reapplied["ok"].boolean() && reapplied["changed"].boolean() && rig.lines("mount.log") == mounts + 1,
              "mounted again");
        const Json twice = rig.manager->reapply();
        check(twice["ok"].boolean() && !twice["changed"].boolean() && rig.lines("mount.log") == mounts + 1,
              "not twice");

        // Putting LG's Home screen back unmounts it and forgets the choices.
        const Json back = rig.manager->restore();
        check(back["ok"].boolean() && back["changed"].boolean(), "restored");
        check(readFile(rig.root + "/proc/mounts").empty() && !exists(rig.data + "/home-webos10.json") &&
                  !exists(rig.root + "/tmp/nativetv-home"),
              "unmounted, forgotten and the overlay files removed");
        check(partsOf(rig.manager->state()) == "sideIcons:true quickCards:true recommended:true", "every part again");
        check(!rig.manager->restore()["changed"].boolean(), "nothing to put back the second time");
        check(!rig.manager->reapply()["changed"].boolean(), "and nothing to reapply");
    }
    {   // The mount refused is reported, and what was chosen is kept for the next start.
        Rig rig("10.3.1", true);
        rig.seed10();
        writeFile(rig.root + "/mount.fail", "");
        const Json failed = rig.manager->setPart("recommended", false);
        check(!failed["ok"].boolean() && has(failed["error"].string(), "denied"), "reported with the reason");
    }
    {   // The switch taken back puts LG's layout back.
        Rig rig("10.3.1", true);
        rig.seed10();
        check(rig.manager->setPart("recommended", false)["ok"].boolean(), "hidden");
        const Json off = rig.manager->setAllowed(false);
        check(off["ok"].boolean() && off["changed"].boolean() && readFile(rig.root + "/proc/mounts").empty(),
              "unmounted when the switch goes off");
        check(!rig.manager->setAllowed(true)["changed"].boolean(), "switching on changes nothing by itself");
    }
    {   // The banner takes the height of hidden rows, so the apps row stays at the bottom.
        const std::vector<Row> lg = rows(kLgLayout10);
        const std::vector<std::vector<std::string>> sets = {
            {"quickCards"}, {"recommended"}, {"quickCards", "recommended"},
            {"sideIcons", "quickCards", "recommended"}};
        for (const std::vector<std::string> &hidden : sets) {
            Rig rig("10.3.1", true);
            rig.seed10();
            check(hide(rig, hidden)["ok"].boolean(), "hidden");
            const std::string xml = readFile(rig.root + "/mounted-home.xml");
            const std::vector<Row> stacked = rows(xml);
            const bool recommended = std::find(hidden.begin(), hidden.end(), "recommended") != hidden.end();
            const bool side = std::find(hidden.begin(), hidden.end(), "sideIcons") != hidden.end();
            check(total(stacked) == total(lg), "same total height");
            check(belowApps(stacked) == belowApps(lg) - (recommended ? 531u : 0u), "the apps row stays at the bottom");
            const std::string box = attribute(xml, "container", "itemHeight");
            check(attribute(xml, "herobanner", "itemHeight") == box, "the banner fills the taller container");
            check(attribute(xml, "globalline", "itemHeight") == (side ? "0" : box),
                  "the side buttons grow with it unless hidden");
        }
        Rig rig("10.3.1", true);
        rig.seed10();
        check(hide(rig, {"quickCards", "recommended"})["ok"].boolean(), "both hidden");
        const std::string both = readFile(rig.root + "/mounted-home.xml");
        check(attribute(both, "container", "itemHeight") == "1685", "900 + 74 + 180 + 531");
        std::string ids;
        for (const Row &row : rows(both)) ids += row.id + " ";
        check(ids == "margin container margin appList margin quickGuide ", "the rows left");
    }
}
} // namespace

int main() {
    try {
        testWebos9();
        testStartLayout();
        testInputs();
        testStopHome();
        testWebos10();
    } catch (const std::exception &error) {
        fprintf(stderr, "home_screen_manager: %s\n", error.what());
        return 1;
    }
    return 0;
}
