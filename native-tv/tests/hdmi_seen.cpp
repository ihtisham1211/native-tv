#include "daemon/hdmi_seen.hpp"
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace nativetv;
using namespace nativetv::daemon;

namespace {
void check(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}

std::string readText(const std::string &path) {
    std::ifstream input(path.c_str(), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

std::string temporaryDirectory() {
    char pattern[] = "/tmp/nativetv-hdmi-seen-XXXXXX";
    char *created = mkdtemp(pattern);
    if (!created)
        throw std::runtime_error("could not create HDMI marker test directory");
    return created;
}

void testSortedMergeAndFormat(const std::string &root) {
    const std::string path = root + "/hdmi_seen";
    Json diagnostics = Json::object();
    diagnostics.set("port", Json::numeric(2));
    diagnostics.set("phy_mode", Json::text("FRL 48 Gbps"));
    diagnostics.set("chroma", Json());
    diagnostics.set("allm", Json::flag(false));
    diagnostics.set("vrr", Json::flag(true));
    std::string error;
    check(recordHdmiSeen(path, diagnostics, &error),
          "native diagnostics write failed: " + error);
    check(readText(path) == "allm\nphy_mode\nvrr\n",
          "marker fields should be sorted, newline-separated, and exclude port/null fields");
    const Json loaded = readHdmiSeen(path, &error);
    check(loaded["allm"].boolean() && loaded["phy_mode"].boolean() &&
              loaded["vrr"].boolean() && !loaded["chroma"].valid(),
          "marker reader should expose only persisted fields as capabilities");

    struct stat info;
    check(stat(path.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600,
          "native HDMI marker must be private");

    const std::string stable = readText(path);
    check(recordHdmiSeen(path, diagnostics, &error),
          "repeated diagnostics should be accepted: " + error);
    check(readText(path) == stable,
          "repeated diagnostics must not rewrite an unchanged marker");

    diagnostics.set("hdcp", Json::text("HDCP 2.3"));
    check(recordHdmiSeen(path, diagnostics, &error),
          "adding a diagnostic field failed: " + error);
    check(readText(path) == "allm\nhdcp\nphy_mode\nvrr\n",
          "new fields should merge into the sorted legacy marker");

    check(!recordHdmiSeen(path, Json::array(), &error),
          "non-object diagnostics should be rejected");
}
} // namespace

int main() {
    std::string root;
    try {
        root = temporaryDirectory();
        testSortedMergeAndFormat(root);
    } catch (const std::exception &error) {
        if (!root.empty()) {
            std::remove((root + "/hdmi_seen").c_str());
            rmdir(root.c_str());
        }
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::remove((root + "/hdmi_seen").c_str());
    rmdir(root.c_str());
    std::cout << "native HDMI seen tests passed\n";
    return 0;
}
