#include "daemon/apps_state.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace nativetv;
using namespace nativetv::daemon;

namespace {
void require(bool condition, const char *message) {
    if (condition) return;
    std::fprintf(stderr, "%s\n", message);
    std::exit(1);
}

std::string readFile(const std::string &path) {
    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}
}

int main() {
    char folder[] = "/tmp/nativetv-app-state-XXXXXX";
    require(mkdtemp(folder) != nullptr, "could not create apps state test directory");
    const std::string root(folder);
    const std::string hidden = root + "/hidden_apps";
    const std::string enabled = root + "/tile_hiding_enabled";
    const Json state = Json::parse(
        "{\"hiddenApps\":[\"z.app\",\"a-app\"],\"tileHidingEnabled\":true}");
    std::string error;
    require(persistAppTileState(hidden, enabled, state, &error), error.c_str());
    require(readFile(hidden) == "a-app\nz.app\n", "hidden app list was not sorted or preserved");
    require(readFile(enabled) == "1\n", "enabled flag format changed");
    struct stat info = {};
    require(stat(hidden.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600,
            "hidden app list permissions are not 0600");
    require(stat(enabled.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600,
            "tile flag permissions are not 0600");

    const Json off = Json::parse(
        "{\"hiddenApps\":[],\"tileHidingEnabled\":false}");
    require(persistAppTileState(hidden, enabled, off, &error), error.c_str());
    require(readFile(hidden).empty(), "empty hidden list should stay empty");
    require(readFile(enabled) == "0\n", "disabled flag format changed");

    const Json invalid = Json::parse(
        "{\"hiddenApps\":[\"../escape\"],\"tileHidingEnabled\":true}");
    require(!persistAppTileState(hidden, enabled, invalid, &error),
            "path-like app id was accepted");
    require(readFile(hidden).empty(), "invalid app id changed the hidden list");
    require(readFile(enabled) == "0\n", "invalid app id changed the flag");
    const Json duplicate = Json::parse(
        "{\"hiddenApps\":[\"app.id\",\"app.id\"],\"tileHidingEnabled\":true}");
    require(!persistAppTileState(hidden, enabled, duplicate, &error),
            "duplicate app ids were accepted");
    require(readFile(enabled) == "0\n", "duplicate app ids changed the flag");

    // test-tile-hiding-gate.js: an upgrade keeps tile hiding where it finds it
    // running, without overwriting a choice, and held back on webOS 10 and later.
    require(!tileHidingCarriesOver(Json::parse("{\"port\":8080}"), false, ""),
            "a clean install must not be migrated");
    require(tileHidingCarriesOver(Json::parse("{\"port\":8080}"), true, ""),
            "an install with tile hiding running must carry it over");
    require(tileHidingCarriesOver(Json::parse("{\"port\":8080}"), true, "qml"),
            "QML stock saver is not held back");
    require(!tileHidingCarriesOver(Json::parse("{\"allowTileHiding\":false}"), true, ""),
            "an explicit false was overwritten");
    require(!tileHidingCarriesOver(Json::parse("{\"allowTileHiding\":true}"), true, ""),
            "an explicit true is already set");
    require(!tileHidingCarriesOver(Json::parse("{\"port\":8080}"), true, "flutter"),
            "webOS 10 and later must stay held back");
    require(tileHidingCarriesOver(Json::parse("{\"allowOnWebos10\":true}"), true, "flutter"),
            "the old override name was not honoured");
    require(tileHidingCarriesOver(Json::parse("{\"allowExperimentalScreensavers\":true}"), true, "flutter"),
            "the experimental screen savers did not lift the hold");
    require(!tileHidingCarriesOver(Json(), true, ""), "a missing config was migrated");

    const std::string config = root + "/config.json";
    {
        std::ofstream file(config.c_str());
        file << "{\"port\":8080,\"custom\":{\"keep\":1}}";
    }
    require(!migrateTileHidingConfig(config, Json::parse(readFile(config)), root, &error),
            "migrated with the flag off");
    {
        std::ofstream file(enabled.c_str());
        file << "1\n";
    }
    require(migrateTileHidingConfig(config, Json::parse(readFile(config)), root, &error), error.c_str());
    const Json migrated = Json::parse(readFile(config));
    require(migrated["allowTileHiding"].boolean() && migrated["port"].number() == 8080 &&
                migrated["custom"]["keep"].number() == 1,
            "the migration lost a key or did not save allowTileHiding");
    require(!migrateTileHidingConfig(config, migrated, root, &error), "migrated twice");
    {
        std::ofstream file((root + "/screensaver-stock-type").c_str());
        file << "flutter\n";
        std::ofstream plain(config.c_str());
        plain << "{\"port\":8080}";
    }
    require(!migrateTileHidingConfig(config, Json::parse(readFile(config)), root, &error),
            "migrated on webOS 10 without the override");

    // allowOnWebos10 became allowExperimentalScreensavers, keeping an explicit new value.
    {
        std::ofstream file(config.c_str());
        file << "{\"port\":8080,\"allowOnWebos10\":true,\"custom\":{\"keep\":1}}";
    }
    require(migrateLegacyConfig(config, Json::parse(readFile(config)), &error), error.c_str());
    Json renamed = Json::parse(readFile(config));
    require(renamed["allowExperimentalScreensavers"].boolean() && !renamed.has("allowOnWebos10") &&
                renamed["custom"]["keep"].number() == 1 && renamed["port"].number() == 8080,
            "the rename lost a key or did not move the value");
    require(!migrateLegacyConfig(config, renamed, &error), "renamed twice");
    {
        std::ofstream file(config.c_str());
        file << "{\"allowOnWebos10\":true,\"allowExperimentalScreensavers\":false}";
    }
    require(migrateLegacyConfig(config, Json::parse(readFile(config)), &error), error.c_str());
    renamed = Json::parse(readFile(config));
    require(renamed.has("allowExperimentalScreensavers") && !renamed["allowExperimentalScreensavers"].boolean() &&
                !renamed.has("allowOnWebos10"),
            "an explicit new value was overwritten by the old one");

    // test-launcher-refresh.js: an install or uninstall has sam read the apps again only when
    // the home ribbon would otherwise miss the change.
    {
        const std::string launcher = root + "/launcher";
        mkdir(launcher.c_str(), 0700);
        const auto put = [&launcher](const char *name, const char *text) {
            std::ofstream file((launcher + "/" + name).c_str());
            file << text;
        };
        const auto clear = [&launcher](const char *name) { unlink((launcher + "/" + name).c_str()); };
        const Json allowed = Json::parse("{\"allowTileHiding\":true}");
        put("tile_hiding_enabled", "0\n");
        require(!launcherNeedsRefresh(launcher, allowed, false),
                "no refresh while nothing is hidden and sam has not been restarted");
        put("tile_hiding_enabled", "1\n");
        put("hidden_apps", "com.webos.app.music\n");
        put(".from-homebrew-channel", "");
        require(!launcherNeedsRefresh(launcher, allowed, false),
                "no refresh with tiles hidden on a Homebrew Channel install, whose boot leaves sam alone");
        clear(".from-homebrew-channel");
        require(launcherNeedsRefresh(launcher, allowed, false),
                "a refresh with tiles hidden, since the boot hook restarted sam");
        put("tile_hiding_enabled", "0\n");
        clear("hidden_apps");
        require(!launcherNeedsRefresh(launcher, allowed, false), "and none once nothing is hidden");
        require(launcherNeedsRefresh(launcher, allowed, true),
                "but every later change once sam has been restarted this run");
        require(!launcherNeedsRefresh(launcher, Json::parse("{\"allowTileHiding\":false}"), true),
                "never where tile hiding is not allowed");
        put("tile_hiding_enabled", "1\n");
        require(launcherNeedsRefresh(launcher, Json::object(), true),
                "with no setting, allowed follows whether tile hiding is on");
        put("tile_hiding_enabled", "0\n");
        require(!launcherNeedsRefresh(launcher, Json::object(), true), "and off means not allowed");
        clear("tile_hiding_enabled");
        rmdir(launcher.c_str());
    }

    unlink(config.c_str());
    unlink((root + "/screensaver-stock-type").c_str());
    unlink(hidden.c_str());
    unlink(enabled.c_str());
    rmdir(root.c_str());
    return 0;
}
