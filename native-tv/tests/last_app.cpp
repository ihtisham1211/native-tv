#include "daemon/last_app.hpp"
#include "daemon/json_file.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace nativetv;

namespace {
void require(bool condition, const char *message) {
    if (condition)
        return;
    std::fprintf(stderr, "%s\n", message);
    std::exit(1);
}

std::string readFile(const std::string &path) {
    std::ifstream input(path.c_str(), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}
}

int main() {
    char directory[] = "/tmp/nativetv-last-app-XXXXXX";
    char *root = mkdtemp(directory);
    require(root != nullptr, "could not create last app test directory");
    const std::string path = std::string(root) + "/last_app.json";

    std::string error;
    Json snapshot = Json::parse("{\"app\":\"Netflix\",\"app_id\":\"netflix\"}");
    require(daemon::recordLastApp(path, snapshot, &error), error.c_str());
    JsonFileResult saved = JsonFile(path).load();
    require(saved.found, "last app store did not save a JSON file");
    require(saved.value["app"].string() == "Netflix", "saved app title changed");
    require(saved.value["app_id"].string() == "netflix", "saved app id changed");
    struct stat info;
    require(stat(path.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600,
            "last app file permissions are not 0600");

    const std::string original = readFile(path);
    require(!daemon::recordLastApp(
                path, Json::parse("{\"app\":\"YouTube\"}"), &error),
            "missing app id was accepted");
    require(readFile(path) == original, "missing app id changed the saved file");
    require(!daemon::recordLastApp(
                path, Json::parse("{\"app\":17,\"app_id\":\"netflix\"}"), &error),
            "numeric app title was accepted");
    require(readFile(path) == original, "invalid app title changed the saved file");
    require(!daemon::recordLastApp(
                path, Json::parse("{\"app\":null,\"app_id\":null}"), &error),
            "empty last app snapshot was accepted");
    require(readFile(path) == original, "empty snapshot changed the saved file");
    require(!daemon::recordLastApp(
                path, Json::parse("{\"app\":\"\",\"app_id\":\"netflix\"}"), &error),
            "empty app title was accepted");
    require(readFile(path) == original, "empty app title changed the saved file");

    require(daemon::recordLastApp(
                path, Json::parse("{\"app\":null,\"app_id\":\"youtube\"}"), &error),
            error.c_str());
    saved = JsonFile(path).load();
    require(saved.found, "nullable app title was not persisted");
    require(!saved.value["app"].valid(), "nullable app title changed");
    require(saved.value["app_id"].string() == "youtube", "nullable snapshot app id changed");

    unlink((path + ".last-good").c_str());
    unlink(path.c_str());
    rmdir(root);
    return 0;
}
