// What the legacy server's syncLauncherPort and syncLauncherAppinfo did (its test-dashboard-port suite).
#include "core/json.hpp"
#include "daemon/launcher_sync.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

// Fixture folders are made and removed with the shell; a failure shows in the checks that read them.
static void runShell(const std::string &command) {
    if (std::system(command.c_str()) != 0) {
    }
}


using nativetv::Json;
using nativetv::daemon::syncLauncherAppinfo;
using nativetv::daemon::syncLauncherPort;

namespace {
int failures = 0;

void check(bool condition, const char *what) {
    std::printf("%s %s\n", condition ? "  ok" : "FAIL", what);
    if (!condition) ++failures;
}
std::string read(const std::string &path) {
    std::ifstream in(path.c_str(), std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}
void write(const std::string &path, const std::string &text) {
    std::ofstream out(path.c_str(), std::ios::binary | std::ios::trunc);
    out << text;
}
bool has(const std::string &text, const std::string &part) { return text.find(part) != std::string::npos; }
using Ids = std::vector<std::string>;
} // namespace

int main() {
    char pattern[] = "/tmp/nativetv-launcher-XXXXXX";
    const std::string root = mkdtemp(pattern);
    const Ids ids = {"com.tvweb.dashboard", "com.tvweb.home",
                     "io.github.ihtisham1211.nativetv"};
    const std::string page =
        "<script>\n  var PORT = 8080;                // tvweb:port\n"
        "  var BASE = 'http://localhost:' + PORT;\n</script>\n";
    for (const std::string &id : ids) {
        mkdir((root + "/" + id).c_str(), 0755);
        write(root + "/" + id + "/index.html", page);
    }
    const auto index = [&](size_t which) { return root + "/" + ids[which] + "/index.html"; };

    check(syncLauncherPort(8080, root).empty(), "already right: nothing written");
    check(syncLauncherPort(8081, root) == ids, "every installed page follows a changed port");
    bool all = true;
    for (size_t i = 0; i < ids.size(); ++i) {
        const std::string html = read(index(i));
        all = all && has(html, "var PORT = 8081;") && has(html, "// tvweb:port") &&
              has(html, "'http://localhost:' + PORT");
    }
    check(all, "the port line changes, the marker and the rest stay");
    check(syncLauncherPort(8081, root).empty(), "a second run changes nothing");
    check(syncLauncherPort(8080, root) == ids, "back to the default port");
    check(syncLauncherPort(70000, root).empty() && syncLauncherPort(0, root).empty(),
          "a port outside 1-65535 is ignored");
    check(has(read(index(0)), "var PORT = 8080;"), "and leaves the pages alone");

    unlink(index(2).c_str());
    check(syncLauncherPort(8082, root) == Ids(ids.begin(), ids.begin() + 2),
          "an app that is not installed is skipped");
    check(syncLauncherPort(8082, root + "/missing").empty(), "no apps directory at all");

    write(index(0), "<p>an older page with no marker</p>");
    check(syncLauncherPort(8083, root) == Ids{ids[1]}, "a page without the marker is left alone");
    check(read(index(0)) == "<p>an older page with no marker</p>", "and is not rewritten");

    write(index(0), "var PORT = 8080;\n// tvweb:port\nvar PORT = 9;  // elsewhere\n");
    check(syncLauncherPort(9000, root).size() == 2 && has(read(index(0)), "var PORT = 9000;\n// tvweb:port") &&
              has(read(index(0)), "var PORT = 9;  // elsewhere"),
          "only the marked line changes, whitespace between the semicolon and marker may be a newline");

    // appinfo.json asks for a 1920x1080 canvas so a 4K panel scales it.
    for (const std::string &id : ids)
        write(root + "/" + id + "/appinfo.json", "{\"id\":\"" + id + "\",\"title\":\"Native Tv\"}");
    check(syncLauncherAppinfo(root) == ids, "appinfo.json without a resolution is updated");
    bool kept = true;
    for (const std::string &id : ids) {
        const Json info = Json::parse(read(root + "/" + id + "/appinfo.json"));
        kept = kept && info["resolution"].string() == "1920x1080" &&
               info["title"].string() == "Native Tv" && info["id"].string() == id;
    }
    check(kept, "resolution is 1920x1080 and the other fields are kept");
    check(syncLauncherAppinfo(root).empty(), "a second run changes nothing");
    check(syncLauncherAppinfo(root + "/nonexistent").empty(), "no apps directory at all");
    write(root + "/" + ids[0] + "/appinfo.json", "{invalid json");
    check(syncLauncherAppinfo(root).empty() && read(root + "/" + ids[0] + "/appinfo.json") == "{invalid json",
          "an unreadable appinfo.json is left alone");
    write(root + "/" + ids[0] + "/appinfo.json", "{\"resolution\":\"1280x720\"}");
    check(syncLauncherAppinfo(root) == Ids{ids[0]} &&
              Json::parse(read(root + "/" + ids[0] + "/appinfo.json"))["resolution"].string() == "1920x1080",
          "a different resolution is corrected");
    write(root + "/" + ids[1] + "/appinfo.json", "{\"id\":\"x\"}");
    (void)syncLauncherPort(8081, root);
    check(Json::parse(read(root + "/" + ids[1] + "/appinfo.json"))["resolution"].string() == "1920x1080",
          "syncLauncherPort runs the appinfo step first");

    // The pages that ship carry the marker at the default port, and nothing else hard-codes it.
    // The dashboard tile is the native client and has no page; it reads the port from the server's own file.
    const struct {
        size_t id;
        const char *page;
    } shipped[] = {{1, "native-tv/assets/home-app/index.html"}, {2, "hbc/app/index.html"}};
    for (const auto &app : shipped) {
        const std::string html = read(std::string(NATIVETV_REPO_ROOT) + "/" + app.page);
        write(index(app.id), html);
        bool followed = false;
        for (const std::string &moved : syncLauncherPort(8123, root)) followed = followed || moved == ids[app.id];
        check(has(html, "var PORT = 8080;") && !has(html, "localhost:8080") && followed &&
                  has(read(index(app.id)), "var PORT = 8123;"),
              app.page);
    }
    const Json tile = Json::parse(read(std::string(NATIVETV_REPO_ROOT) + "/native-tv/assets/dashboard-app/appinfo.json"));
    check(tile["id"].string() == ids[0] && tile["type"].string() == "native" && tile["resolution"].string() == "1920x1080" &&
              access((std::string(NATIVETV_REPO_ROOT) + "/native-tv/assets/dashboard-app/index.html").c_str(), F_OK) != 0,
          "the dashboard tile ships as a native app with no page");

    const std::string cleanup = "rm -rf " + root;
    runShell(cleanup);
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    return 0;
}
