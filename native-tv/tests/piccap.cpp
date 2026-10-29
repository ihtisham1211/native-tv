#include "service/piccap.hpp"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace nativetv::service;
using nativetv::Json;
using nativetv::Json;

namespace {
void check(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}

std::string makeRoot() {
    char pattern[] = "/tmp/nativetv-piccap-XXXXXX";
    char *path = ::mkdtemp(pattern);
    if (!path)
        throw std::runtime_error("could not create PicCap test directory");
    return path;
}

void makeDirectory(const std::string &path) {
    size_t offset = 1;
    while (offset <= path.size()) {
        const size_t end = path.find('/', offset);
        const std::string current = path.substr(0, end == std::string::npos
                                                       ? path.size() : end);
        if (!current.empty() && ::mkdir(current.c_str(), 0700) != 0 && errno != EEXIST)
            throw std::runtime_error("could not create " + current);
        if (end == std::string::npos)
            break;
        offset = end + 1;
    }
}

void write(const std::string &root, const std::string &path,
           const std::string &contents) {
    const std::string fullPath = root + path;
    const size_t slash = fullPath.find_last_of('/');
    makeDirectory(fullPath.substr(0, slash));
    std::ofstream file(fullPath.c_str());
    if (!file)
        throw std::runtime_error("could not write " + fullPath);
    file << contents;
}

void removeTree(const std::string &root) {
    const std::string app = root +
        "/media/developer/apps/usr/palm/applications/org.webosbrew.piccap";
    ::rmdir(app.c_str());
    ::rmdir((root + "/media/developer/apps/usr/palm/applications").c_str());
    ::rmdir((root + "/media/developer/apps/usr/palm").c_str());
    ::rmdir((root + "/media/developer/apps/usr").c_str());
    ::rmdir((root + "/media/developer/apps").c_str());
    ::rmdir((root + "/media/developer").c_str());
    ::rmdir((root + "/media").c_str());
    ::rmdir((root + "/etc").c_str());
    ::rmdir(root.c_str());
}

void testInstallationGateAndUnknownVersion() {
    const std::string root = makeRoot();
    try {
        check(piccapSupportedWebos(root),
              "an unread webOS version preserves the legacy allow behavior");
        check(!piccapInstalled(root), "PicCap is absent until its app folder exists");
        makeDirectory(root +
                      "/media/developer/apps/usr/palm/applications/org.webosbrew.piccap");
        check(piccapInstalled(root), "the exact PicCap Homebrew app folder is detected");
    } catch (...) {
        removeTree(root);
        throw;
    }
    removeTree(root);
}

void testOldWebosGates() {
    const std::string issueRoot = makeRoot();
    try {
        write(issueRoot, "/etc/issue", "webOS TV 2.2.3\n");
        check(!piccapSupportedWebos(issueRoot),
              "webOS 2 is refused to avoid the legacy service reset");
    } catch (...) {
        removeTree(issueRoot);
        throw;
    }
    removeTree(issueRoot);

    const std::string starfishRoot = makeRoot();
    try {
        write(starfishRoot, "/etc/starfish-release", "Starfish release 2.2.3\n");
        check(!piccapSupportedWebos(starfishRoot),
              "the starfish release version is used when issue files are absent");
    } catch (...) {
        removeTree(starfishRoot);
        throw;
    }
    removeTree(starfishRoot);
}

void testSupportedWebos() {
    const std::string root = makeRoot();
    try {
        write(root, "/etc/issue.net", "LG webOS TV 3.4.0\n");
        check(piccapSupportedWebos(root), "webOS 3 and newer support PicCap");
    } catch (...) {
        removeTree(root);
        throw;
    }
    removeTree(root);
}

void testStatusReader() {
    const std::string root = makeRoot();
    try {
        long long now = 100000;
        int asked = 0;
        Json answer = Json::parse("{\"returnValue\":true,\"isRunning\":true}");
        bool answers = true;
        std::string raw;
        PiccapStatus status(root, [&](Json &reply, std::string &rawText) {
            ++asked;
            reply = answer;
            rawText = raw;
            return answers;
        }, [&now]() { return now; });

        check(!status.read().valid() && asked == 0,
              "a TV without the app is not asked and has no row");
        makeDirectory(root + "/media/developer/apps/usr/palm/applications/org.webosbrew.piccap");

        Json row = status.read();
        check(row.isObject() && row["running"].boolean() && asked == 1, "a running PicCap shows running");
        now += 3999;
        answer = Json::parse("{\"returnValue\":true,\"isRunning\":false}");
        check(status.read()["running"].boolean() && asked == 1, "the answer is kept for 4 s");
        now += 1;
        row = status.read();
        check(row.isObject() && !row["running"].boolean() && asked == 2, "after 4 s it is asked again");

        now += 4000;
        answers = false;
        check(status.read().isObject() && asked == 3,
              "a lost answer keeps what PicCap last was");
        now += 4000;
        answers = true;
        answer = Json::parse("{\"returnValue\":false,\"errorText\":\"Unknown method \\\"status\\\" for category \\\"/\\\"\"}");
        check(!status.read().valid(), "a service that is not there takes the row away");
        now += 4000;
        answer = Json::parse("{\"returnValue\":false,\"errorText\":\"busy\"}");
        check(!status.read().valid(), "a refusal that is not 'missing' leaves it away");
        now += 4000;
        answer = Json::parse("{\"returnValue\":true,\"isRunning\":true}");
        check(status.read()["running"].boolean(), "and it returns when the service answers");

        check(piccapServiceMissing(Json::object(), "luna-send: No such service"),
              "the raw text counts when the reply has no error text");
        check(!piccapServiceMissing(Json::parse("{\"errorText\":\"timeout\"}"), "no such service"),
              "an error text in the reply decides, not the raw text");

        write(root, "/etc/issue", "webOS TV 2.2.3\n");
        now += 4000;
        asked = 0;
        check(!status.read().valid() && asked == 0, "webOS 2 is never asked");
    } catch (...) {
        removeTree(root);
        throw;
    }
    removeTree(root);
}
// test-piccap-gate.js: an old piccap.enabled becomes the entity setting.
void testOldEnabledKeyMigration() {
    Json off = piccapConfigMigration(Json::parse(
        R"({"piccap":{"enabled":false,"pollIntervalMs":30000},"mqtt":{"entities":{"controls":true,"disabled":["mute"]}}})"));
    check(off.isObject() && off["piccap"]["pollIntervalMs"].number() == 30000 &&
              !off["piccap"].has("enabled") && off["mqtt"]["entities"]["controls"].boolean() &&
              off["mqtt"]["entities"]["disabled"].size() == 2 &&
              off["mqtt"]["entities"]["disabled"][0].string() == "mute" &&
              off["mqtt"]["entities"]["disabled"][1].string() == "piccap",
          "off stays off, as an unticked entity, and the key goes");
    Json on = piccapConfigMigration(Json::parse(R"({"piccap":{"enabled":true}})"));
    check(on.isObject() && !on["piccap"].valid() && !on.has("mqtt"),
          "true meant nothing, so the section just goes");
    Json bare = piccapConfigMigration(Json::parse(R"({"piccap":{"enabled":false}})"));
    check(bare["mqtt"]["entities"]["disabled"].size() == 1 && !bare["piccap"].valid(),
          "no mqtt section yet: one is made, the empty piccap goes");
    Json twice = piccapConfigMigration(Json::parse(
        R"({"piccap":{"enabled":false},"mqtt":{"entities":{"disabled":["piccap"]}}})"));
    check(twice["mqtt"]["entities"]["disabled"].size() == 1, "already unticked: not listed twice");
    check(!piccapConfigMigration(Json::parse(R"({"piccap":{"pollIntervalMs":1000}})")).valid(),
          "nothing to do, nothing written");
    check(!piccapConfigMigration(Json::parse("{}")).valid() && !piccapConfigMigration(Json()).valid(),
          "no section, nothing written");
}
} // namespace

int main() {
    try {
        testInstallationGateAndUnknownVersion();
        testOldWebosGates();
        testSupportedWebos();
        testStatusReader();
        testOldEnabledKeyMigration();
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "PicCap service test failed: %s\n", error.what());
        return 1;
    }
}
