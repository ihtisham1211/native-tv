#include "service/app_catalog.hpp"
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using namespace nativetv;

namespace {
const std::string defaultRepo = "https://repo.webosbrew.org/api/apps.json";
const std::string extraRepo = "https://example.org/catalog.json";
const std::string hash(64, 'a');

Json package(const std::string &id, const std::string &version,
             const std::string &ipkUrl, const std::string &title = "") {
    Json manifest = Json::object();
    manifest.set("id", Json::text(id));
    manifest.set("version", Json::text(version));
    manifest.set("ipkUrl", Json::text(ipkUrl));
    Json ipkHash = Json::object();
    ipkHash.set("sha256", Json::text(hash));
    manifest.set("ipkHash", ipkHash);
    manifest.set("ipkSize", Json::numeric(1234));
    Json item = Json::object();
    item.set("id", Json::text(id));
    item.set("title", Json::text(title.empty() ? id : title));
    item.set("manifest", manifest);
    return item;
}

Json catalogPage(const std::vector<Json> &packages, int pages = 1) {
    Json result = Json::object();
    Json list = Json::array();
    for (const Json &item : packages) list.add(item);
    result.set("packages", list);
    Json paging = Json::object();
    paging.set("maxPage", Json::numeric(pages));
    result.set("paging", paging);
    return result;
}

Json findById(const Json &apps, const std::string &id) {
    for (const Json &app : apps.items())
        if (app["id"].string() == id) return app;
    return Json();
}
} // namespace

int main() {
    assert(service::AppCatalog::defaultRepository() == defaultRepo);
    assert(service::AppCatalog::normalizeRepositoryCandidate(
               "  https://github.com/Owner/Project.git/?ref=tv  ") ==
           "https://raw.githubusercontent.com/Owner/Project/HEAD/repo.json");
    assert(service::AppCatalog::normalizeRepositoryCandidate(
               "https://github.com/Owner/.git") ==
           "https://raw.githubusercontent.com/Owner/.git/HEAD/repo.json");
    assert(service::AppCatalog::repositoryInputError("") ==
           "the address must start with http:// or https://");
    assert(service::AppCatalog::repositoryInputError("http://example.org/apps.json") ==
           "the address must start with https://");
    assert(service::AppCatalog::repositoryInputError(
               "https://example.org/bad path.json") ==
           "the address contains spaces or control characters");
    Json customRepositories = service::AppCatalog::customRepositories(Json::parse(
        "[\"https://repo.webosbrew.org/api/apps.json\",\"https://example.org/catalog.json\","
        "\"https://example.org/catalog.json\",2]"));
    assert(customRepositories.size() == 1);
    assert(customRepositories[0].string() == extraRepo);

    Json noHash = package("com.example.nohash", "1.0", "missing-hash.ipk");
    Json noHashManifest = noHash["manifest"];
    noHashManifest.set("ipkHash", Json::object());
    noHash.set("manifest", noHashManifest);
    Json first = catalogPage({
        package("org.webosbrew.hbchannel", "1.0", "channel.ipk", "Homebrew"),
        package("org.webosbrew.hbchannel.beta", "1.0", "beta.ipk"),
        package("com.example.player", "2.0.0", "../ipks/player.ipk", "Player"),
        package("bad/id", "1.0", "bad.ipk"),
        noHash
    }, 2);
    Json second = catalogPage({
        package("com.example.player", "2.0.0", "../ipks/player.ipk", "Duplicate"),
        package("com.example.multi", "1.2", "multi.ipk")
    });
    Json custom = catalogPage({package("com.example.store", "1.5", "store.ipk")});
    std::vector<std::string> requests;
    bool failExtra = false;
    service::AppCatalog catalog("4.2.0", [&](const std::string &url,
                                               const std::string &agent,
                                               Json &document,
                                               std::string &error) {
        requests.push_back(url);
        assert(agent == "tvweb/4.2.0");
        if (url == extraRepo && failExtra) {
            error = "offline";
            return false;
        }
        if (url == defaultRepo) document = first;
        else if (url == "https://repo.webosbrew.org/api/apps/2.json") document = second;
        else if (url == extraRepo) document = custom;
        else {
            error = "unexpected request";
            return false;
        }
        error.clear();
        return true;
    });

    Json configured = Json::array();
    configured.add(Json::text(defaultRepo));
    configured.add(Json::text(extraRepo));
    configured.add(Json::text(extraRepo));
    Json installed = Json::parse(
        "{\"apps\":["
        "{\"id\":\"com.example.player\",\"version\":\"1.0\","
        "\"folderPath\":\"/media/developer/apps/player\"},"
        "{\"id\":\"com.example.store\",\"version\":\"1.4\","
        "\"folderPath\":\"/media/cryptofs/apps/store\"}]}" );

    Json result = catalog.get(configured, installed, false, 100000);
    assert(result["ok"].boolean());
    assert(result["fetchedAt"].number() == 100000);
    assert(requests.size() == 3);
    assert(requests[0] == defaultRepo);
    assert(requests[1] == "https://repo.webosbrew.org/api/apps/2.json");
    assert(requests[2] == extraRepo);
    assert(result["apps"].size() == 4);
    assert(result["others"].size() == 1);
    assert(result["repos"].size() == 1);
    assert(result["repos"][0]["label"].string() == "example.org");
    assert(result["repos"][0]["count"].number() == 1);

    Json channel = findById(result["apps"], "org.webosbrew.hbchannel");
    assert(channel["state"].string() == "hbc");
    assert(channel["vetted"].boolean());
    assert(findById(result["apps"], "org.webosbrew.hbchannel.beta")["state"].string() == "hbc");
    Json player = findById(result["apps"], "com.example.player");
    assert(player["state"].string() == "update");
    assert(player["installedVersion"].string() == "1.0");
    assert(player["ipkUrl"].string() == "https://repo.webosbrew.org/ipks/player.ipk");
    assert(player["size"].number() == 1234);
    assert(findById(result["apps"], "com.example.multi").valid());
    Json store = findById(result["others"], "com.example.store");
    assert(store["state"].string() == "store");
    assert(!store["vetted"].boolean());

    Json installedNewer = Json::parse(
        "{\"apps\":[{\"id\":\"com.example.player\",\"version\":\"3.0\","
        "\"folderPath\":\"/media/developer/apps/player\"}]}" );
    Json cached = catalog.get(configured, installedNewer, false, 105000);
    assert(requests.size() == 3);
    assert(findById(cached["apps"], "com.example.player")["state"].string() == "installed");
    assert(findById(cached["apps"], "com.example.player")["installedVersion"].string() == "3.0");
    catalog.get(configured, installed, true, 105000);
    assert(requests.size() == 3);
    catalog.get(configured, installed, true, 110001);
    assert(requests.size() == 6);

    failExtra = true;
    Json stale = catalog.get(configured, installed, false, 3710002);
    assert(requests.size() == 9);
    assert(stale["ok"].boolean());
    assert(stale.has("error"));
    assert(stale["repos"][0].has("error"));
    assert(findById(stale["others"], "com.example.store").valid());
    catalog.get(configured, installed, false, 3715000);
    assert(requests.size() == 9);

    Json selected = catalog.findPackage(configured, installed, "com.example.player", "",
                                        3715001);
    assert(selected["ok"].boolean());
    assert(selected["pkg"]["id"].string() == "com.example.player");
    Json selectedCustom = catalog.findPackage(configured, installed, "com.example.store",
                                              extraRepo, 3715001);
    assert(selectedCustom["ok"].boolean());
    assert(selectedCustom["pkg"]["repo"].string() == extraRepo);
    Json missing = catalog.findPackage(configured, installed, "com.example.missing", "",
                                       3715001);
    assert(!missing["ok"].boolean());
    assert(missing["error"].string() == "not in the catalog: com.example.missing");
    assert(requests.size() == 9);

    failExtra = false;
    Json checkedRepo = catalog.checkRepository(extraRepo);
    assert(checkedRepo["ok"].boolean());
    assert(requests.size() == 10);
    failExtra = true;
    Json failedRepo = catalog.checkRepository(extraRepo);
    assert(!failedRepo["ok"].boolean());
    assert(failedRepo["error"].string() == "offline");
    Json refusedRepo = catalog.checkRepository("http://example.org/apps.json");
    assert(!refusedRepo["ok"].boolean());
    assert(refusedRepo["error"].string().find("https://") != std::string::npos);
    assert(requests.size() == 11);

    const Json invalidHttp = service::AppCatalog::parsePackage(
        package("com.example.bad", "1.0", "http://example.org/app.ipk"),
        defaultRepo, defaultRepo);
    assert(!invalidHttp.valid());
    assert(service::AppCatalog::resolveUrl("https://cdn.example/a/manifest.json",
                                           "../ipk/app.ipk") ==
           "https://cdn.example/ipk/app.ipk");
    assert(service::AppCatalog::resolveUrl("https://cdn.example/a/b.json",
                                           "/ipk/app.ipk") ==
           "https://cdn.example/ipk/app.ipk");
    Json fetched;
    std::string fetchError;
    assert(!service::AppCatalog::fetchJson("http://example.org/apps.json", "tvweb/4.2.0",
                                           fetched, fetchError));
    assert(fetchError.find("https://") != std::string::npos);

    std::cout << "native app catalog assertions passed\n";
    return 0;
}
