// TvAppManager against a stand-in install-app.sh: the answer is the last line
// of output, an install is read from its wording, and the boot refresh covers
// both native client apps (the dashboard tile and the splash).
#include "daemon/child_process.hpp"
#include "daemon/tv_app_manager.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace nativetv;
using namespace nativetv::daemon;

namespace {
int failures = 0;

void check(bool condition, const char *what) {
    std::printf("%s %s\n", condition ? "  ok" : "FAIL", what);
    if (!condition) ++failures;
}
void write(const std::string &path, const std::string &text) { std::ofstream(path) << text; }
std::string read(const std::string &path) {
    std::ifstream in(path);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}
} // namespace

int main() {
    char pattern[] = "/tmp/nativetv-tvapp-XXXXXX";
    const std::string root = mkdtemp(pattern);
    const std::string log = root + "/calls";
    mkdir((root + "/dashboard-app").c_str(), 0755);
    // Records its arguments. The tile is up to date; the splash (named by its
    // folder in $2) is stale, so the script reinstalls it and says so in words.
    write(root + "/dashboard-app/install-app.sh",
          "echo \"$@\" >> " + log + "\n"
          "case \"$1:$2\" in\n"
          "  status:) echo noise; echo '{\"ok\":true,\"supported\":true,\"installed\":true}';;\n"
          "  refresh:) echo '{\"ok\":true,\"installed\":true,\"refreshed\":false}';;\n"
          "  refresh:*) echo 'native startup splash added to the home screen';;\n"
          "  install:) echo 'dashboard app not installed (the dashboard is still served to browsers)';;\n"
          "  *) exit 3;;\n"
          "esac\n");

    ChildProcessRunner children;
    TvAppManager manager(children, root);

    Json status = manager.run("status");
    check(status["supported"].boolean() && status["installed"].boolean(),
          "the last line of output is the answer");
    check(!manager.run("install")["ok"].boolean(), "an install that did not add the tile is not ok");

    // No splash folder: the tile alone is refreshed, and its answer is returned.
    unlink(log.c_str());
    Json tileOnly = manager.refreshInstalled();
    check(tileOnly["ok"].boolean() && !tileOnly["refreshed"].boolean(), "up to date tile, no splash");
    check(read(log) == "refresh\n", "only the tile's refresh ran");

    // With the splash installed beside it, it is refreshed after the tile.
    mkdir((root + "/splash-app").c_str(), 0755);
    write(root + "/splash-app/appinfo.json", "{}");
    unlink(log.c_str());
    Json both = manager.refreshInstalled();
    check(read(log) == "refresh\nrefresh " + root + "/splash-app\n", "the tile, then the splash");
    check(both["ok"].boolean() && both["refreshed"].boolean(),
          "a refreshed splash is reported as refreshed");

    check(manager.run("status", "../elsewhere")["supported"].boolean() == false,
          "a folder outside the assets root is refused");
    Json missing = manager.run("status", "no-such-app");
    check(missing["ok"].boolean() && !missing["supported"].boolean(), "an app with no appinfo.json is unsupported");

    unlink((root + "/dashboard-app/install-app.sh").c_str());
    Json gone = manager.run("status");
    check(gone["ok"].boolean() && !gone["supported"].boolean() && !gone["installed"].boolean(),
          "without the script the app is unsupported");
    check(!manager.refreshInstalled()["refreshed"].boolean(), "and nothing is refreshed");

    for (const char *path : {"/calls", "/splash-app/appinfo.json"}) unlink((root + path).c_str());
    for (const char *path : {"/splash-app", "/dashboard-app", ""}) rmdir((root + path).c_str());
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
