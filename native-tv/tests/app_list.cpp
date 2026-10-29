#include "service/app_list.hpp"
#include "service/app_ids.hpp"
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace nativetv;

namespace {
void makeDirectory(const std::string &path) {
    std::string current;
    for (size_t index = 0; index < path.size(); ++index) {
        current.push_back(path[index]);
        if (path[index] != '/' || current.size() <= 1) continue;
        if (mkdir(current.c_str(), 0700) != 0 && errno != EEXIST)
            assert(false && "could not create test directory");
    }
    if (mkdir(path.c_str(), 0700) != 0 && errno != EEXIST)
        assert(false && "could not create test directory");
}

void writeFile(const std::string &path, const std::string &contents) {
    const size_t slash = path.find_last_of('/');
    if (slash != std::string::npos) makeDirectory(path.substr(0, slash));
    std::ofstream output(path.c_str(), std::ios::binary);
    assert(output.good());
    output << contents;
    assert(output.good());
}

Json findById(const Json &items, const std::string &id) {
    for (const Json &item : items.items())
        if (item["id"].string() == id) return item;
    return Json();
}
} // namespace

int main() {
    char temporary[] = "/tmp/nativetv-app-list-XXXXXX";
    char *root = mkdtemp(temporary);
    assert(root);
    const std::string systemRoot = root;
    const std::string dataRoot = systemRoot + "/data";
    writeFile(dataRoot + "/saved_page_titles.json", "{\"page-1\":\"renamed\"}");
    writeFile(systemRoot + "/media/system/apps/usr/palm/applications/org.example.hidden/appinfo.json",
              "{\"id\":\"org.example.hidden\",\"title\":\"Hidden Tile\"}");
    writeFile(systemRoot + "/media/developer/apps/usr/palm/applications/org.example.youtube/a.bin",
              "1234567");
    writeFile(systemRoot + "/media/developer/apps/usr/palm/applications/org.example.youtube/skip-link",
              "ignored");
    const std::string target = systemRoot + "/media/developer/apps/usr/palm/applications/org.example.youtube/a.bin";
    const std::string link = systemRoot + "/media/developer/apps/usr/palm/applications/org.example.youtube/skip-link";
    unlink(link.c_str());
    assert(symlink(target.c_str(), link.c_str()) == 0);

    const Json appsReply = Json::parse(
        "{\"apps\":["
        "{\"id\":\"org.example.youtube\",\"title\":\"YouTube\",\"version\":\"2.1\","
        "\"vendor\":\"Example\",\"folderPath\":\"/media/developer/apps/usr/palm/applications/org.example.youtube\","
        "\"removable\":true,\"appsize\":0},"
        "{\"id\":\"com.webos.app.gallery\",\"title\":\"Gallery\",\"systemApp\":true,"
        "\"removable\":false,\"folderPath\":\"/usr/palm/applications/com.webos.app.gallery\"},"
        "{\"id\":\"org.example.store\",\"title\":\"Store Copy\",\"folderPath\":\"/media/cryptofs/apps/org.example.store\",\"removable\":false},"
        "{\"id\":\"com.tvweb.dashboard\",\"title\":\"Native Tv\",\"removable\":true,"
        "\"folderPath\":\"/media/developer/apps/com.tvweb.dashboard\"},"
        "{\"id\":\"org.example:encoded\",\"title\":\"Encoded\",\"removable\":true,"
        "\"folderPath\":\"/media/developer/apps/org.example:encoded\"}]}");
    const Json launchPointsReply = Json::parse(
        "{\"launchPoints\":["
        "{\"id\":\"com.webos.app.gallery\",\"title\":\"Gallery Home\",\"systemApp\":true},"
        "{\"id\":\"com.webos.app.browser\",\"lptype\":\"bookmark\",\"launchPointId\":\"page-1\","
        "\"title\":\"Movie Site\",\"params\":{\"target\":\"https://example.org/watch\"}},"
        "{\"id\":\"com.webos.app.browser\",\"lptype\":\"bookmark\",\"launchPointId\":\"page-2\","
        "\"title\":\"News\",\"params\":{\"url\":\"https://example.org/news\"}}]}");

    service::AppListState state;
    state.systemRoot = systemRoot;
    state.dataRoot = dataRoot;
    state.hiddenIds.insert("org.example.hidden");
    state.hiddenIds.insert("org.example.youtube");
    state.tileHidingEnabled = true;
    const Json result = service::appListResponse(appsReply, launchPointsReply, state, true);
    assert(result["ok"].boolean());
    assert(result["writable"].boolean());
    assert(result["tileHidingEnabled"].boolean());
    assert(result["hiddenCount"].number() == 2);
    assert(result["installed"].size() == 3);
    assert(result["systemTiles"].size() == 2);
    assert(result["savedPages"].size() == 2);

    Json youtube = findById(result["installed"], "org.example.youtube");
    assert(youtube.valid());
    assert(youtube["size"].number() == 7);
    assert(youtube["removable"].boolean());
    assert(findById(result["installed"], "com.tvweb.dashboard").valid() == false);
    Json store = findById(result["installed"], "org.example.store");
    assert(store.valid() && store["removable"].boolean());
    Json encoded = findById(result["installed"], "org.example:encoded");
    assert(encoded["iconUrl"].string() == "/api/apps/icon?id=org.example%3Aencoded");

    Json gallery = findById(result["systemTiles"], "com.webos.app.gallery");
    assert(gallery.valid());
    assert(gallery["title"].string() == "Gallery Home");
    assert(!gallery["hidden"].boolean());
    Json hidden = findById(result["systemTiles"], "org.example.hidden");
    assert(hidden.valid() && hidden["hidden"].boolean());
    assert(hidden["title"].string() == "Hidden Tile");

    Json firstPage = result["savedPages"][0];
    assert(firstPage["launchPointId"].string() == "page-1");
    assert(firstPage["address"].string() == "https://example.org/watch");
    assert(firstPage["renamed"].boolean());
    Json secondPage = result["savedPages"][1];
    assert(secondPage["address"].string() == "https://example.org/news");
    assert(!secondPage["renamed"].boolean());

    assert(service::appIdIsProtected("com.webos.app.input.hdmi2"));
    assert(service::appIdIsProtected("com.tvweb.splash"));
    assert(service::appIdIsProtected("hdmi2"));
    assert(!service::appIdIsValid("../escape"));

    // test/test-apps.js isProtected: the core TV services, and nothing else.
    for (const char *id : {"com.tvweb.dashboard", "com.webos.app.browser", "org.webosbrew.hbchannel",
                           "com.webos.app.livetv", "com.palm.app.settings", "com.webos.app.settings",
                           "com.webos.app.container", "container", "com.webos.app.inputcommon",
                           "inputcommon", "com.webos.app.home", "com.webos.app.firstuse", "firstuse",
                           "com.webos.app.eula", "eula", "com.webos.app.webapphost", "webapphost",
                           "com.webos.app.hdmi1", "com.webos.app.hdmi2", "com.webos.app.hdmi3",
                           "com.webos.app.hdmi4", "airplay", "com.tvweb.home", "com.tvweb.native"})
        assert(service::appIdIsProtected(id));
    for (const char *id : {"com.webos.app.igallery", "com.webos.app.music", "com.webos.app.photovideo",
                           "com.webos.app.tvuserguide", "com.webos.app.sportsteamsettings",
                           "com.webos.app.homeconnect", "com.webos.app.lifeonscreen",
                           "com.webos.app.camera", "youtube.leanback.v4", "netflix", "spotify-beehive"})
        assert(!service::appIdIsProtected(id));
    // No id is the safe side of the question: nothing to hide or uninstall.
    assert(service::appIdIsProtected(""));

    unlink((dataRoot + "/saved_page_titles.json").c_str());
    unlink((systemRoot + "/media/system/apps/usr/palm/applications/org.example.hidden/appinfo.json").c_str());
    unlink(target.c_str());
    unlink(link.c_str());
    std::cout << "native app list projection assertions passed\n";
    return 0;
}
