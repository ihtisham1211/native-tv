#include "service/app_icons.hpp"
#include "service/app_kinds.hpp"
#include <cerrno>
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

void makeDirectory(const std::string &path) {
    if (mkdir(path.c_str(), 0700) != 0)
        require(errno == EEXIST, "could not create app icon test directory");
}

void writeFile(const std::string &path, const std::string &contents) {
    std::ofstream output(path.c_str(), std::ios::binary);
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    require(output.good(), "could not write app icon fixture");
}
}

int main() {
    char directory[] = "/tmp/nativetv-app-icons-XXXXXX";
    char *root = mkdtemp(directory);
    require(root != nullptr, "could not create app icon test root");
    const std::string systemRoot(root);
    const std::string apps = systemRoot + "/media/developer/apps/usr/palm/applications";
    const std::string app = apps + "/com.example.app";
    const std::string escapedApp = apps + "/com.example.escape";
    const std::string titledApp = apps + "/com.example.titled";
    const std::string assets = systemRoot + "/assets";
    const std::string pack = assets + "/app-icons/app-icons.pack";
    makeDirectory(systemRoot + "/media");
    makeDirectory(systemRoot + "/media/developer");
    makeDirectory(systemRoot + "/media/developer/apps");
    makeDirectory(systemRoot + "/media/developer/apps/usr");
    makeDirectory(systemRoot + "/media/developer/apps/usr/palm");
    makeDirectory(apps);
    makeDirectory(app);
    makeDirectory(app + "/assets");
    makeDirectory(escapedApp);
    makeDirectory(titledApp);
    makeDirectory(assets);
    makeDirectory(assets + "/app-icons");

    const std::string icon("\x89PNG\r\n\x1a\nicon", 11);
    writeFile(app + "/appinfo.json", "{\"largeIcon\":\"assets/icon.png\"}");
    writeFile(app + "/assets/icon.png", icon);
    const service::AppIconFile found = service::readAppIcon("com.example.app", systemRoot, assets);
    require(found.found, "declared app icon was not found");
    require(found.bytes == icon, "app icon bytes changed");
    require(!found.etag.empty(), "app icon ETag was not created");

    const std::string secret = systemRoot + "/secret.json";
    writeFile(secret, "{\"token\":\"secret\"}");
    require(symlink(secret.c_str(), (escapedApp + "/escape.png").c_str()) == 0,
            "could not create escaping app icon symlink");
    writeFile(escapedApp + "/appinfo.json", "{\"icon\":\"escape.png\"}");
    require(!service::readAppIcon("com.example.escape", systemRoot, assets).found,
            "icon symlink outside its app directory was served");

    writeFile(escapedApp + "/appinfo.json",
              "{\"icon\":\"../../../../secret.json\"}");
    require(!service::readAppIcon("com.example.escape", systemRoot, assets).found,
            "icon path outside its app directory was served");
    require(!service::readAppIcon("../secret", systemRoot, assets).found,
            "invalid app id was accepted");
    require(!service::readAppIcon("com.example.missing", systemRoot, assets).found,
            "missing app icon was reported as found");

    // The themed pack wins by id, even for an app that is not installed, then
    // by title; the app's own icon shows when the pack has neither.
    writeFile(pack, "NTVICONS 1\npacked.id 0 5\nexamplevideoplus 5 4\nbroken.id 9 99\n\nAAAAABBBB");
    const service::AppIconFile byId = service::readAppIcon("packed.id", systemRoot, assets);
    require(byId.found && byId.bytes == "AAAAA" && !byId.etag.empty(), "packed icon by id was not served");
    writeFile(titledApp + "/appinfo.json", "{\"title\":\"Example Video+\",\"icon\":\"own.png\"}");
    writeFile(titledApp + "/own.png", icon);
    const service::AppIconFile byTitle = service::readAppIcon("com.example.titled", systemRoot, assets);
    require(byTitle.found && byTitle.bytes == "BBBB", "packed icon by title was not served");
    require(service::readAppIcon("com.example.app", systemRoot, assets).bytes == icon,
            "an app the pack lacks lost its own icon");
    require(!service::readAppIcon("broken.id", systemRoot, assets).found,
            "pack entry past the end of the file was served");
    // With no assets root the pack is skipped: that is how the route serves an app's own art (own=1).
    require(service::readAppIcon("com.example.titled", systemRoot, "").bytes == icon,
            "an empty assets root did not serve the app's own icon");
    unlink(pack.c_str());
    require(service::readAppIcon("com.example.titled", systemRoot, assets).bytes == icon,
            "missing pack did not fall back to the app's own icon");

    unlink((escapedApp + "/escape.png").c_str());
    unlink((escapedApp + "/appinfo.json").c_str());
    unlink((app + "/assets/icon.png").c_str());
    unlink((app + "/appinfo.json").c_str());
    unlink(secret.c_str());
    unlink((titledApp + "/own.png").c_str());
    unlink((titledApp + "/appinfo.json").c_str());
    rmdir(titledApp.c_str());
    rmdir((assets + "/app-icons").c_str());
    rmdir(assets.c_str());
    rmdir((app + "/assets").c_str());
    rmdir(app.c_str());
    rmdir(escapedApp.c_str());
    rmdir(apps.c_str());
    rmdir((systemRoot + "/media/developer/apps/usr/palm").c_str());
    rmdir((systemRoot + "/media/developer/apps/usr").c_str());
    rmdir((systemRoot + "/media/developer/apps").c_str());
    rmdir((systemRoot + "/media/developer").c_str());
    rmdir((systemRoot + "/media").c_str());
    rmdir(systemRoot.c_str());

    // Kinds: by id, by the title's pack key, by LG's prefixes, else other.
    require(service::appKind("netflix", "Netflix") == "video", "netflix is a video app by id");
    require(service::appKind("com.disney.app", "Disney+") == "video",
            "an unknown id is placed by its title's key");
    require(service::appKind("com.webos.app.camera", "Camera") == "tv",
            "LG's own apps fall under LG & TV");
    require(service::appKind("org.webosbrew.hbchannel", "Homebrew Channel") == "tools",
            "the Homebrew Channel is a tool");
    require(service::appKind("com.example.unknown", "Whatever") == "other",
            "an app nobody named is other");
    require(service::appKinds().size() == 7 && service::appKinds().back().key == std::string("other"),
            "seven kinds, other last");
    require(service::appKindLabel("music") == "Music & audio" && service::appKindLabel("nope").empty(),
            "labels come with the kinds");
    require(service::validAppKind("games") && !service::validAppKind("Games"), "kind keys are exact");
    return 0;
}
