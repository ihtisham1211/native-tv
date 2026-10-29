// What the daemon removes at start that earlier releases installed (the Node server's files included).
#include "daemon/retired_files.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using nativetv::daemon::removeRetiredFiles;

namespace {
int failures = 0;

void check(bool condition, const char *what) {
    std::printf("%s %s\n", condition ? "  ok" : "FAIL", what);
    if (!condition) ++failures;
}
bool exists(const std::string &path) {
    struct stat info;
    return stat(path.c_str(), &info) == 0;
}
void touch(const std::string &path) { std::ofstream(path).put('x'); }
} // namespace

int main() {
    char pattern[] = "/tmp/nativetv-retired-XXXXXX";
    const std::string root = mkdtemp(pattern);
    for (const char *folder : {"/assets", "/assets/i18n"}) mkdir((root + folder).c_str(), 0755);
    touch(root + "/assets/i18n/es.json");
    touch(root + "/assets/qr.js");
    touch(root + "/assets/kept.js");
    check(removeRetiredFiles(root) == 2, "the listed files that exist are counted");
    check(!exists(root + "/assets/i18n"), "the emptied folder goes too");
    check(exists(root + "/assets/kept.js"), "nothing else is touched");
    check(removeRetiredFiles(root) == 0, "a second start finds nothing");
    unlink((root + "/assets/kept.js").c_str());
    rmdir((root + "/assets").c_str());
    rmdir(root.c_str());

    char kitPattern[] = "/tmp/nativetv-retired-kit-XXXXXX";
    const std::string kit = mkdtemp(kitPattern);
    std::string folder = kit;
    for (const char *name : {"assets", "google-tv", "pages", "kit"}) {
        folder += std::string("/") + name;
        mkdir(folder.c_str(), 0755);
    }
    for (const char *name : {"index.html", "kit.css", "kit.js"}) touch(folder + "/" + name);
    check(removeRetiredFiles(kit) == 3, "the retired gallery's files are removed");
    check(!exists(folder), "and its folder");
    check(removeRetiredFiles(kit) == 0, "and a second start finds nothing there");
    for (const char *path : {"/assets/google-tv/pages", "/assets/google-tv", "/assets", ""})
        rmdir((kit + path).c_str());

    // The Node server's files, left by a release before the daemon was the whole server.
    char nodePattern[] = "/tmp/nativetv-retired-node-XXXXXX";
    const std::string node = mkdtemp(nodePattern);
    mkdir((node + "/lib").c_str(), 0755);
    touch(node + "/tvweb.js");
    touch(node + "/lib/ha.js");
    touch(node + "/lib/own.js");
    check(removeRetiredFiles(node) == 2, "tvweb.js and a lib module of the Node server go");
    check(exists(node + "/lib/own.js") && exists(node + "/lib"), "a file the server never had stays, and its folder");
    unlink((node + "/lib/own.js").c_str());
    rmdir((node + "/lib").c_str());
    rmdir(node.c_str());

    // The server's own binaries are never retired.
    char binariesPattern[] = "/tmp/nativetv-retired-binaries-XXXXXX";
    const std::string binaries = mkdtemp(binariesPattern);
    mkdir((binaries + "/native").c_str(), 0755);
    for (const char *name : {"nativetv-daemon", "nativetv-service"}) touch(binaries + "/native/" + name);
    check(removeRetiredFiles(binaries) == 0 &&
              exists(binaries + "/native/nativetv-daemon") && exists(binaries + "/native/nativetv-service"),
          "the daemon and service stay");
    for (const char *name : {"nativetv-daemon", "nativetv-service"}) unlink((binaries + "/native/" + name).c_str());
    rmdir((binaries + "/native").c_str());
    rmdir(binaries.c_str());

    // The dashboard tile's web page, which its native app no longer has; the app's own files stay.
    char tilePattern[] = "/tmp/nativetv-retired-tile-XXXXXX";
    const std::string tile = mkdtemp(tilePattern);
    for (const char *folder : {"/assets", "/assets/dashboard-app"}) mkdir((tile + folder).c_str(), 0755);
    touch(tile + "/assets/dashboard-app/index.html");
    touch(tile + "/assets/dashboard-app/appinfo.json");
    check(removeRetiredFiles(tile) == 1 && !exists(tile + "/assets/dashboard-app/index.html"),
          "the dashboard tile's old page goes");
    check(exists(tile + "/assets/dashboard-app/appinfo.json"), "and the native app's files stay");
    unlink((tile + "/assets/dashboard-app/appinfo.json").c_str());
    for (const char *folder : {"/assets/dashboard-app", "/assets", ""}) rmdir((tile + folder).c_str());

    check(removeRetiredFiles("/nonexistent/nativetv-retired") == 0, "a missing server folder is not an error");
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
