#include "service/adblock.hpp"
#include "service/adblock_store.hpp"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <set>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using nativetv::Json;

namespace {
void check(bool condition, const std::string &message) {
    if (condition)
        return;
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

bool has(const std::vector<std::string> &items, const std::string &value) {
    for (const std::string &item : items)
        if (item == value)
            return true;
    return false;
}

bool sinkholed(const std::string &table, const std::string &host) {
    return table.find("0.0.0.0\t" + host + "\n") != std::string::npos &&
        table.find("::\t" + host + "\n") != std::string::npos;
}

std::string readFile(const std::string &path) {
    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
    if (!input)
        return std::string();
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

void writeFile(const std::string &path, const std::string &contents) {
    std::ofstream output(path.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
    if (!output)
        std::abort();
    output << contents;
    output.close();
    if (!output)
        std::abort();
}

class TempTree {
  public:
    TempTree() {
        char pattern[] = "/tmp/nativetv-adblock-XXXXXX";
        char *created = ::mkdtemp(pattern);
        if (!created)
            std::abort();
        root = created;
        data = root + "/data";
        system = root + "/system";
        etc = system + "/etc";
        preferences = system + "/var/luna/preferences";
        if (::mkdir(data.c_str(), 0700) != 0 ||
            ::mkdir(system.c_str(), 0700) != 0 ||
            ::mkdir(etc.c_str(), 0700) != 0 ||
            ::mkdir((system + "/var").c_str(), 0700) != 0 ||
            ::mkdir((system + "/var/luna").c_str(), 0700) != 0 ||
            ::mkdir(preferences.c_str(), 0700) != 0)
            std::abort();
    }

    ~TempTree() {
        ::unlink((data + "/adblock_hosts").c_str());
        ::unlink((data + "/adblock_enabled").c_str());
        ::unlink((data + "/adblock_custom.json").c_str());
        ::unlink((data + "/local_lookups").c_str());
        ::unlink((etc + "/hosts").c_str());
        ::unlink((preferences + "/webosbrew_block_updates").c_str());
        ::rmdir(preferences.c_str());
        ::rmdir((system + "/var/luna").c_str());
        ::rmdir((system + "/var").c_str());
        ::rmdir(etc.c_str());
        ::rmdir(system.c_str());
        ::rmdir(data.c_str());
        ::rmdir(root.c_str());
    }

    std::string root;
    std::string data;
    std::string system;
    std::string etc;
    std::string preferences;
};

nativetv::service::AdBlockMountOperations fakeMountOperations() {
    const std::shared_ptr<std::vector<std::string>> lowerLayers(
        new std::vector<std::string>());
    nativetv::service::AdBlockMountOperations result;
    result.bind = [lowerLayers](const std::string &source,
                                const std::string &target) {
        lowerLayers->push_back(readFile(target));
        writeFile(target, readFile(source));
        return true;
    };
    result.unmount = [lowerLayers](const std::string &target) {
        if (lowerLayers->empty())
            return false;
        writeFile(target, lowerLayers->back());
        lowerLayers->pop_back();
        return true;
    };
    return result;
}

void checkStore() {
    using namespace nativetv::service;
    {
        TempTree tree;
        const std::string hosts = tree.etc + "/hosts";
        writeFile(hosts, "127.0.0.1 localhost\n192.168.1.2 nas\n");
        AdBlockStore store(tree.data, tree.system, fakeMountOperations());
        Json enabled = store.setMode("ads", true);
        check(enabled["ok"].boolean() && enabled["enabled"].boolean() &&
                  enabled["mode"].string() == "ads" && store.tableMounted(),
              "enabling ad blocking persists the mode and mounts the generated table");
        check(readFile(tree.data + "/adblock_enabled") == "ads" &&
                  readFile(tree.data + "/adblock_hosts").find("192.168.1.2 nas") !=
                      std::string::npos,
              "mode and unrelated host entries are persisted");

        Json full = store.setMode("full", true);
        check(full["ok"].boolean() && full["mode"].string() == "full" &&
                  sinkholed(readFile(tree.data + "/adblock_hosts"), "ngfts.lge.com"),
              "changing tiers rewrites the mounted table without another bind mount");

        Json updateBlock = store.setTvUpdatesBlocked(true, true);
        check(updateBlock["ok"].boolean() && updateBlock["tvUpdatesBlocked"].boolean() &&
                  sinkholed(readFile(tree.data + "/adblock_hosts"), "snu.lge.com"),
              "TV update blocking shares and refreshes the same table");

        Json customOptions = Json::parse(
            R"({"mode":"add","text":"mine.example.com\n","file":"my\u0001list.txt"})");
        Json custom = store.setCustomHosts(customOptions, true);
        check(custom["ok"].boolean() && custom["custom"]["count"].number() == 1 &&
                  custom["custom"]["file"].string() == "mylist.txt" &&
                  sinkholed(readFile(tree.data + "/adblock_hosts"), "mine.example.com"),
              "custom host upload persists sanitized metadata and updates the active table");
        Json disabledCustom = store.setCustomHosts(
            Json::parse(R"({"enabled":false})"), true);
        check(disabledCustom["ok"].boolean() &&
                  !sinkholed(readFile(tree.data + "/adblock_hosts"), "mine.example.com") &&
                  readFile(tree.data + "/adblock_custom.json").find("mine.example.com") !=
                      std::string::npos,
              "disabling a custom list keeps it saved and removes its active entries");

        check(store.setMode("off", true)["ok"].boolean() &&
                  !store.mode().compare("off") && store.tableMounted(),
              "ad-block off leaves the shared table mounted while TV updates remain blocked");
        Json updatesAllowed = store.setTvUpdatesBlocked(false, true);
        check(updatesAllowed["ok"].boolean() && !updatesAllowed["tvUpdatesBlocked"].boolean() &&
                  !store.tableMounted(),
              "allowing TV updates unmounts the table when no blocker remains");
    }
    {
        TempTree tree;
        const std::string hosts = tree.etc + "/hosts";
        writeFile(hosts, "127.0.0.1 localhost\n192.168.1.2 nas\n");
        AdBlockStore store(tree.data, tree.system, fakeMountOperations());
        check(store.setMode("ads", true)["ok"].boolean(),
              "the foreign-host preservation fixture mounts");
        std::ofstream mountedHosts(hosts.c_str(), std::ios::out | std::ios::app);
        mountedHosts << "10.0.0.5 another-device\n";
        mountedHosts.close();
        Json disabled = store.setMode("off", true);
        const std::string table = readFile(tree.data + "/adblock_hosts");
        check(disabled["ok"].boolean() && !disabled["enabled"].boolean() &&
                  store.tableMounted() &&
                  table.find("10.0.0.5 another-device") != std::string::npos &&
                  !sinkholed(table, "ad.lgsmartad.com"),
              "disabling preserves foreign mappings added while the table was mounted");
    }
}
} // namespace

void checkLocalLookups() {
    using namespace nativetv::service;
    // 192.168.2.128/24, 10.0.0.9/16 (covered as the /24 around it), 192.168.5.37/28
    // and a public address, which is left alone.
    const std::vector<LocalInterface> interfaces = {
        {0xc0a80280u, 0xffffff00u}, {0x0a000009u, 0xffff0000u},
        {0xc0a80525u, 0xfffffff0u}, {0x51020304u, 0xffffff00u}};
    const std::vector<std::string> addresses = adBlockLocalLookupAddresses(interfaces);
    size_t onWlan = 0, onSmall = 0;
    for (const std::string &address : addresses) {
        if (address.compare(0, 10, "192.168.2.") == 0) ++onWlan;
        if (address.compare(0, 10, "192.168.5.") == 0) ++onSmall;
    }
    check(onWlan == 253 && !has(addresses, "192.168.2.128") && !has(addresses, "192.168.2.0") &&
              !has(addresses, "192.168.2.255"),
          "a /24 is 1 to 254, less the TV");
    check(has(addresses, "10.0.0.1") && has(addresses, "10.0.0.254") &&
              !has(addresses, "10.0.0.9") && !has(addresses, "10.0.1.1"),
          "a wider network covers the /24 around the TV");
    check(onSmall == 13 && has(addresses, "192.168.5.33") && has(addresses, "192.168.5.46") &&
              !has(addresses, "192.168.5.37") && !has(addresses, "192.168.5.47"),
          "a /28 is its 14 hosts, less the TV");
    check(addresses.size() == 253 + 253 + 13, "public addresses add none");

    check(adBlockIsLocalLookupLine("192.168.2.1\t192.168.2.1") &&
              !adBlockIsLocalLookupLine("192.168.2.5\tnas") &&
              !adBlockIsLocalLookupLine("192.168.2.1 192.168.2.2") &&
              !adBlockIsLocalLookupLine("nas nas") &&
              !adBlockIsLocalLookupLine("999.1.1.1 999.1.1.1") &&
              !adBlockIsLocalLookupLine("192.168.2.1\t192.168.2.1\tnas"),
          "only an address named as itself is a local lookup line");

    const std::string table = adBlockHostsTable("off", "", "", false, false, nullptr, {},
                                                {"192.168.2.1", "192.168.2.2"});
    check(table.find("\n192.168.2.1\t192.168.2.1\n192.168.2.2\t192.168.2.2\n") != std::string::npos,
          "the table names each address as itself");
    check(adBlockHostsTable("off", "", "", false, false, nullptr, {}).find("192.168.2.1") ==
              std::string::npos,
          "nothing without the setting");
    const std::vector<std::string> foreign = adBlockForeignHostsLines(
        table + "192.168.2.5\tnas\n", adBlockManagedNames("", ""));
    check(foreign.size() == 1 && foreign[0] == "192.168.2.5\tnas",
          "the table's own lines are not carried over as another tool's, a real name is");

    {
        TempTree tree;
        const std::string hosts = tree.etc + "/hosts";
        writeFile(hosts, "127.0.0.1 localhost\n");
        const std::shared_ptr<std::vector<LocalInterface>> current(
            new std::vector<LocalInterface>{{0xc0a80280u, 0xffffff00u}});
        AdBlockMountOperations operations = fakeMountOperations();
        operations.interfaces = [current]() { return *current; };
        AdBlockStore store(tree.data, tree.system, operations);
        check(!store.localLookups() && !store.tableMounted(), "off to begin with");
        const Json on = store.setLocalLookups(true, false);
        check(on["ok"].boolean() && on["localLookups"].boolean() && store.localLookups() &&
                  store.tableMounted() && store.mode() == "off",
              "switching it on mounts the table with the ad blocker off");
        std::string mounted = readFile(hosts);
        check(mounted.find("\n192.168.2.1\t192.168.2.1\n") != std::string::npos &&
                  mounted.find("192.168.2.128\t") == std::string::npos &&
                  mounted.find("0.0.0.0") == std::string::npos,
              "the mounted table answers the network but not the TV, and blocks nothing");

        // A real bind mount shows the table file as it is rewritten; the fake copies
        // it once, so later writes are read from the file itself.
        const std::string tableFile = tree.data + "/adblock_hosts";
        check(store.refreshForNetworkChange(false) && readFile(tableFile) == mounted,
              "the same network leaves the table as it is");
        (*current)[0] = LocalInterface{0x0a000009u, 0xffffff00u};
        check(store.refreshForNetworkChange(false), "a refresh succeeds");
        const std::string moved = readFile(tableFile);
        check(moved.find("\n10.0.0.1\t10.0.0.1\n") != std::string::npos &&
                  moved.find("192.168.2.1\t") == std::string::npos,
              "another network replaces the lines, which are not kept as another tool's");

        const Json off = store.setLocalLookups(false, false);
        check(off["ok"].boolean() && !off["localLookups"].boolean() && !store.localLookups() &&
                  !store.tableMounted() && readFile(hosts) == "127.0.0.1 localhost\n",
              "switching it off takes the table down when nothing else needs it");
    }
    {
        // A table that cannot be mounted must not leave the switch on.
        TempTree tree;
        writeFile(tree.etc + "/hosts", "127.0.0.1 localhost\n");
        AdBlockMountOperations operations = fakeMountOperations();
        operations.bind = [](const std::string &, const std::string &) { return false; };
        operations.interfaces = []() {
            return std::vector<LocalInterface>{{0xc0a80280u, 0xffffff00u}};
        };
        AdBlockStore store(tree.data, tree.system, operations);
        const Json refused = store.setLocalLookups(true, false);
        check(!refused["ok"].boolean() && !refused["error"].string().empty() &&
                  !store.localLookups(),
              "a refused mount leaves the setting off, and says why");
    }
    {
        TempTree tree;
        writeFile(tree.etc + "/hosts", "127.0.0.1 localhost\n");
        writeFile(tree.data + "/local_lookups", "");
        AdBlockMountOperations operations = fakeMountOperations();
        operations.interfaces = []() {
            return std::vector<LocalInterface>{{0xc0a80280u, 0xffffff00u}};
        };
        AdBlockStore store(tree.data, tree.system, operations);
        check(store.restoreAtBoot(true) && store.tableMounted() &&
                  readFile(tree.etc + "/hosts").find("192.168.2.1\t192.168.2.1\n") !=
                      std::string::npos,
              "the table comes back at boot for the setting alone");
    }
}

// The names refreshed from upstream Glasshouse's lists, by tier (test/test-privacy.js
// there): each is blocked on both address families, and the Everything tier's stay
// reachable on Ads & telemetry.
void checkRefreshedLists() {
    using namespace nativetv::service;
    const std::vector<std::string> adsNames = {
        "ad.lgappstv.com", "info.lgsmartad.net", "lgad.cjpowercast.com", "lgads.tv",
        "ad-inserter.alphonso.tv", "config-overlay-ads.alphonso.tv",
        "ruc.cdpbeacon.lgtvcommon.com", "eic-rum.elsa.lgecloudapm.com", "lgecloudapm.com",
        "tracking.lgtvsdp.com", "rone-aic.lge.com", "rone-eic.lge.com", "rone-kic.lge.com",
        "rone-saic.lge.com", "rone-tv-aic.lge.com", "rone-tv-cic.lge.com",
        "rone-tv-eic.lge.com", "rone-tv-kic.lge.com", "rone-tv-saic.lge.com",
        // The regional families, bare, with LG's regions and the TV's country.
        "ad.lgsmartad.com", "gb.ad.lgsmartad.com", "ads.lgtvcommon.com", "gb.ads.lgtvcommon.com",
        "eic.videoads.lgtvcommon.com", "gb.elastic.lgwebostv.com", "kic.elastic.lgwebostv.com",
        "cdpbeacon2.lgtvcommon.com", "eu.cdpbeacon2.lgtvcommon.com",
        "api.adts.lgtvcommon.com", "aic.api.adts.lgtvcommon.com", "gb.api.adts.lgtvcommon.com"};
    const std::vector<std::string> adsClockNames = {
        "rdx2.nextlgsdp.com", "gb.rdx2.nextlgsdp.com", "aic.rdx2.nextlgsdp.com"};
    const std::vector<std::string> everythingNames = {
        "eic-ngfts.lge.com", "config.lgtvsdp.com", "thetake.com", "pnv.lgtvcommon.com",
        "wiseresource.lgtvcommon.com", "qcardservice.lgtvcommon.com",
        "ruc.homeprv.lgtvcommon.com", "ruc.nudge.lgtvcommon.com", "ruc.recommend.lgtvcommon.com",
        "ruc.service.lgtvcommon.com", "ruc.wiseconfig.lgtvcommon.com", "ruc.api.lgtviot.com",
        "ruc.commonpush.lgtviot.com", "ruc.push.lgtviot.com", "ruc.sports.lgtviot.com",
        "qt2-kic.lgtviot.com", "ruc-op-lss.lgthinq.com", "it-service.lgthinq.com",
        "lgthinq.com", "lss.lgthinq.com", "noti.lgthinq.com", "prd-api.lgthinq.com",
        "eic.lgchhomeapp.lgtvcommon.com", "app-lgwebos.pluto.tv", "sp.pluto.tv",
        "tags.tiqcdn.com", "lg-channelplus-eu-beacons.xumo.com",
        "lg-channelplus-eu-mds.xumo.com", "bss.lgechannel.com", "lgechannel.com",
        "www.ueiwsp.com", "mtalk.google.com", "device-provisioning.googleapis.com",
        // Bare, with the regions (aic.lgtviot.com) and the TV's country (gb.).
        "lgtviot.com", "gb.lgtviot.com", "eic.api.lgtviot.com", "gb.commonpush.lgtviot.com",
        "kic.sports.lgtviot.com", "gb.buddy.lgtviot.com", "lgeapi.com", "eu.lgeapi.com",
        "gb.app.lgwebostv.com", "app.lgwebostv.com", "gb.lgrecommends.lgappstv.com",
        // Home takes the regions as labels, ThinQ and OCP with a hyphen.
        "cdplauncher.lgtvcommon.com", "aic.cdplauncher.lgtvcommon.com",
        "kic.service.lgtvcommon.com", "eic.wiseconfig.lgtvcommon.com",
        "eic-ocp.lgtviot.com", "ocp.lgtviot.com", "eic-connect-client.lgthinq.com",
        "kic-common.lgthinq.com", "aic-iotservice.lgthinq.com", "eic-op-lss.lgthinq.com"};
    const std::vector<std::string> clockNames = {
        "nextlgsdp.com", "us.nextlgsdp.com", "de.nextlgsdp.com", "af.nextlgsdp.com",
        "eu7.nextlgsdp.com", "ibs.nextlgsdp.com", "gb.ibsstat.nextlgsdp.com",
        "tv.wiselg.com", "gb.tv.wiselg.com", "eic.tv.wiselg.com", "ruc.tv.wiselg.com",
        "ngfts.tv.wiselg.com", "eic-gfts.tv.wiselg.com", "dev-ruc-ngfts.tv.wiselg.com"};

    const std::string adsTable = adBlockHostsTable("ads", "gb", "", false, false, nullptr, {});
    const std::string fullTable = adBlockHostsTable("full", "gb", "", false, false, nullptr, {});
    const std::string graceTable = adBlockHostsTable("full", "gb", "", true, false, nullptr, {});
    const std::string adsGraceTable = adBlockHostsTable("ads", "gb", "", true, false, nullptr, {});
    for (const std::string &name : adsNames) {
        check(sinkholed(adsTable, name) && sinkholed(fullTable, name) &&
                  sinkholed(adsGraceTable, name) && sinkholed(graceTable, name),
              name + " is blocked on both tiers, clock or not");
    }
    for (const std::string &name : everythingNames) {
        check(sinkholed(fullTable, name) && sinkholed(graceTable, name),
              name + " is blocked on the Everything tier, clock or not");
        check(!sinkholed(adsTable, name), name + " stays reachable on Ads & telemetry");
    }
    for (const std::string &name : adsClockNames) {
        check(sinkholed(adsTable, name) && sinkholed(fullTable, name),
              name + " is blocked once the clock is valid");
        check(!sinkholed(adsGraceTable, name) && !sinkholed(graceTable, name),
              name + " waits for the clock");
    }
    for (const std::string &name : clockNames) {
        check(sinkholed(fullTable, name) && !sinkholed(graceTable, name) &&
                  !sinkholed(adsTable, name),
              name + " waits for the clock and is Everything's alone");
    }
    // A country7 variant only for the countries LG documents it for.
    check(has(adBlockPlatform("us", "", false), "us7.nextlgsdp.com") &&
              !has(adBlockPlatform("gb", "", false), "gb7.nextlgsdp.com") &&
              !has(adBlockPlatform("de", "", false), "de7.nextlgsdp.com") &&
              has(adBlockPlatform("", "", false), "au7.nextlgsdp.com") &&
              !has(adBlockPlatform("", "", false), "gb7.nextlgsdp.com"),
          "the country7 name is listed for the countries it is documented for");
    check(has(adBlockPlatform("fr", "", false), "fr.app.lgwebostv.com") &&
              !has(adBlockPlatform("fr", "", false), "gb.app.lgwebostv.com") &&
              !has(adBlockPlatform("fr", "", false), "aic.app.lgwebostv.com"),
          "the Content Store's hosts take the TV's country alone, not LG's regions");
    check(has(adBlockPlatform("", "", false), "gb.app.lgwebostv.com") &&
              has(adBlockPlatform("", "", false), "de.app.lgwebostv.com"),
          "before the TV's country is known the countries LG is seen using stand in");

    // On webOS 11 the Content Store's host is one of the clock servers: it waits with
    // them, where any other store host is blocked at once.
    check(!has(adBlockPlatform("us", "us.tv.wiselg.com", true), "us.tv.wiselg.com") &&
              has(adBlockPlatform("us", "us.tv.wiselg.com", false), "us.tv.wiselg.com") &&
              has(adBlockPlatform("us", "store.lgtvsdp.com", true), "store.lgtvsdp.com"),
          "the store host that is a clock server waits for the clock");

    check(adBlockIsSdpName("tv.wiselg.com") && adBlockIsSdpName("us.tv.wiselg.com") &&
              adBlockIsSdpName("nextlgsdp.com") && adBlockIsSdpName("us.rdx2.nextlgsdp.com") &&
              !adBlockIsSdpName("wiselg.com") && !adBlockIsSdpName("lgtvsdp.com") &&
              !adBlockIsSdpName("notnextlgsdp.com") && !adBlockIsSdpName("tv.wiselg.com.example.org"),
          "the clock servers are told apart by domain, not by a substring");

    // The owner's list names under either domain wait too.
    AdBlockCustomList custom;
    custom.mode = "add";
    custom.names = {"mine.example.com", "eic.tv.wiselg.com"};
    check(!has(adBlockList("ads", "gb", "", true, &custom), "eic.tv.wiselg.com") &&
              has(adBlockList("ads", "gb", "", false, &custom), "eic.tv.wiselg.com") &&
              has(adBlockList("ads", "gb", "", true, &custom), "mine.example.com"),
          "a list naming a tv.wiselg.com host waits for the clock");

    // The unique names, both address families.
    for (const char *mode : {"ads", "full"}) {
        const std::vector<std::string> names = adBlockList(mode, "gb", "", false, nullptr);
        check(std::set<std::string>(names.begin(), names.end()).size() == names.size(),
              std::string(mode) + ": the refreshed lists have no name twice");
    }

    // The TV's country's own clock server is what AirPlay's key check goes through.
    check(adBlockBlocksClockServer({"gb.nextlgsdp.com"}, "gb") &&
              adBlockBlocksClockServer({"gb.tv.wiselg.com"}, "gb") &&
              !adBlockBlocksClockServer({"us.nextlgsdp.com"}, "gb") &&
              !adBlockBlocksClockServer({"gb.rdx2.nextlgsdp.com", "nextlgsdp.com"}, "gb") &&
              adBlockBlocksClockServer({"de.nextlgsdp.com"}, "") &&
              !adBlockBlocksClockServer({"jp.nextlgsdp.com"}, ""),
          "the country's clock server is told from the others, and the fallback countries stand in");
}

int main() {
    using namespace nativetv::service;

    const AdBlockParseResult parsed = parseAdBlockCustomHosts(
        "# comment\n0.0.0.0 a.example.com b.example.com # trailing\r\n"
        "127.0.0.1 localhost\n::1 ip6-localhost\n"
        "192.168.1.5 nas.example.com\nPlain.Example.org.\n"
        ":: v6.example.com\nbad!name.example.com\na.example.com\n");
    check(parsed.names == std::vector<std::string>({
              "a.example.com", "b.example.com", "plain.example.org",
              "v6.example.com"}),
          "custom hosts parsing keeps unique sinkhole names and normalizes case");
    check(parsed.skipped == 2,
          "custom hosts parsing counts redirects and malformed host names");
    {
        const std::vector<std::string> names = {"ad.example.com", "gb.ad.example.com"};
        check(adBlockExport("hosts", names) ==
                  "# Native Tv block list: 2 names\n0.0.0.0 ad.example.com\n"
                  "0.0.0.0 gb.ad.example.com\n",
              "the hosts export is 0.0.0.0 lines under a comment");
        check(adBlockExport("pihole", names) ==
                  "# Native Tv block list: 2 names\nad.example.com\ngb.ad.example.com\n",
              "the Pi-hole export is one name per line");
        check(adBlockExport("adguard", names) ==
                  "! Native Tv block list: 2 names\n||ad.example.com^\n||gb.ad.example.com^\n",
              "the AdGuard export is ||name^ rules under a ! comment");
        check(adBlockExport("csv", names).empty() && adBlockExport("", names).empty(),
              "an unknown export format gives nothing");
        check(adBlockExport("hosts", {}) == "# Native Tv block list: 0 names\n",
              "an empty list exports its header only");
        const std::vector<std::string> full = adBlockList("full", "gb", "", false, nullptr);
        check(parseAdBlockCustomHosts(adBlockExport("hosts", full)).names == full,
              "the hosts export of the built-in list reads back as the same names");
        // What is exported is what the TV blocks at the level.
        const std::vector<std::string> ads = adBlockExportNames("ads", "gb", "", nullptr, false);
        const std::vector<std::string> everything =
            adBlockExportNames("full", "gb", "", nullptr, false);
        check(has(ads, "ad.doubleclick.net") && has(ads, "gb.info.lgsmartad.com") &&
                  !has(ads, "lgtviot.com") && !has(ads, "snu.lge.com"),
              "the ads export holds the ad and tracker names, no platform or update hosts");
        check(has(everything, "ad.doubleclick.net") && has(everything, "lgtviot.com") &&
                  has(everything, "nextlgsdp.com") && !has(everything, "snu.lge.com") &&
                  everything.size() > ads.size(),
              "the full export adds the platform hosts");
        const std::vector<std::string> withUpdates =
            adBlockExportNames("ads", "gb", "", nullptr, true);
        check(has(withUpdates, "snu.lge.com") && has(withUpdates, "nsu.lge.com") &&
                  has(withUpdates, "snu-dev.lge.com") && withUpdates.size() == ads.size() + 6,
              "the TV software update servers are exported while they are blocked");
        check(std::set<std::string>(withUpdates.begin(), withUpdates.end()).size() ==
                  withUpdates.size(),
              "an exported list has no name twice");
        AdBlockCustomList own;
        own.mode = "add";
        own.names = {"extra.example.com", "ad.doubleclick.net"};
        check(has(adBlockExportNames("ads", "gb", "", &own, false), "extra.example.com") &&
                  adBlockExportNames("ads", "gb", "", &own, false).size() == ads.size() + 1,
              "a custom list added to the built-in names is exported once");
        own.mode = "replace";
        check(adBlockExportNames("ads", "gb", "", &own, false) == own.names,
              "a custom list that replaces the built-in names is all that is exported");
        own.enabled = false;
        check(adBlockExportNames("ads", "gb", "", &own, false) == ads,
              "a custom list that is switched off is not exported");
    }
    check(parseAdBlockCustomHosts("notes\n").names.empty(),
          "a single word is not a host name");

    const std::vector<std::string> ads = adBlockAds("gb");
    check(has(ads, "ad.doubleclick.net") && has(ads, "gb.cdpbeacon.lgtvcommon.com") &&
              has(ads, "eu.cdpsvc.lgtvcommon.com"),
          "ads mode includes common advertising and firmware regional hosts");
    check(!has(ads, "us.cdpbeacon.lgtvcommon.com"),
          "a known country does not add fallback country prefixes");

    const std::vector<std::string> fullDuringGrace =
        adBlockOwnList("full", "us", "nativetv.local", true);
    check(has(fullDuringGrace, "lgtvsdp.com") &&
              has(fullDuringGrace, "nativetv.local") &&
              !has(fullDuringGrace, "nextlgsdp.com"),
          "full mode includes platform hosts but leaves clock hosts reachable in grace");
    const std::vector<std::string> fullAfterGrace =
        adBlockOwnList("full", "us", "nativetv.local", false);
    check(has(fullAfterGrace, "nextlgsdp.com"),
          "full mode blocks SDP clock hosts after the grace period");
    const std::vector<std::string> managed =
        adBlockManagedNames("us", "nativetv.local");
    check(has(managed, "ad.doubleclick.net") && has(managed, "nextlgsdp.com") &&
              has(managed, "snu.lge.com") && has(managed, "nativetv.local"),
          "foreign-host filtering can exclude every name managed by either blocker");

    AdBlockCustomList custom;
    custom.mode = "add";
    custom.enabled = true;
    custom.names = {"mine.example.com", "ad.doubleclick.net",
                    "nextlgsdp.com", "deep.nextlgsdp.com"};
    const std::vector<std::string> added =
        adBlockList("ads", "us", "", true, &custom);
    check(has(added, "mine.example.com") && has(added, "ad.doubleclick.net") &&
              !has(added, "nextlgsdp.com") && !has(added, "deep.nextlgsdp.com"),
          "add mode deduplicates built-ins and preserves the SDP grace");
    custom.mode = "replace";
    const std::vector<std::string> replaced =
        adBlockList("full", "us", "", false, &custom);
    check(replaced == custom.names,
          "replace mode blocks only the owner's names");
    custom.enabled = false;
    check(!has(adBlockList("ads", "us", "", false, &custom),
               "mine.example.com"),
          "a disabled custom list remains saved without being applied");
    check(adBlockList("off", "us", "", false, &custom).empty(),
          "off mode applies neither built-in nor custom hosts");

    // A file the table did not write: every sinkhole in it is another tool's (Life's
    // Good Without Spying and LG App Update Blocker write nothing else), kept even
    // where the table blocks the same name, so it outlives a tier being switched off.
    const std::vector<std::string> foreign = adBlockForeignHostsLines(
        "# comment\n0.0.0.0 ad.doubleclick.net\n:: su.lge.com\n"
        "127.0.0.1 cloudfront.xp-assets.aiv-cdn.net\n"
        "::1 cloudfront.xp-assets.aiv-cdn.net\n"
        "127.0.0.1 su.lge.com\n"
        "192.168.1.5 nas\n192.168.1.5 nas\n0.0.0.0\n0.0.0.0 ad.doubleclick.net\n",
        {"ad.doubleclick.net", "su.lge.com"});
    check(foreign == std::vector<std::string>({
              "0.0.0.0 ad.doubleclick.net", ":: su.lge.com",
              "127.0.0.1 cloudfront.xp-assets.aiv-cdn.net",
              "::1 cloudfront.xp-assets.aiv-cdn.net", "192.168.1.5 nas",
              "0.0.0.0"}),
          "another tool's file keeps its sinkholes, drops the managed hosts' redirects "
          "(the Homebrew Channel's update block) and removes duplicates");
    // The table's own file: its sinkholes are its own until the kept lines' header.
    const std::string ownTable =
        "127.0.0.1\tlocalhost\n\n# LG Ad & Telemetry Blackhole (lg-webos-dashboard)\n"
        "0.0.0.0\tad.doubleclick.net\n::\tad.doubleclick.net\n"
        "0.0.0.0\tfoo.example.com\n::\tfoo.example.com\n"
        "\n# Kept from /etc/hosts, where other tools put them\n"
        "0.0.0.0 tracker.example.net\n:: tracker.example.net\n"
        "127.0.0.1 cloudfront.xp-assets.aiv-cdn.net\n";
    check(adBlockForeignHostsLines(ownTable, {"ad.doubleclick.net"}) ==
              std::vector<std::string>({
                  "0.0.0.0 tracker.example.net", ":: tracker.example.net",
                  "127.0.0.1 cloudfront.xp-assets.aiv-cdn.net"}),
          "the table's own sinkholes go, and those under its kept header stay");
    check(adBlockForeignHostsLines(
              "# LG Ad & Telemetry Blackhole (lg-webos-mqtt)\n0.0.0.0\tfoo.example.com\n", {}).empty(),
          "a table written under the earlier name is told apart the same way");
    {
        // What was kept is written back under the header, and is kept again as it is.
        const std::string rewritten = adBlockHostsTable(
            "ads", "gb", "", false, false, nullptr,
            {"0.0.0.0 tracker.example.net", ":: tracker.example.net"});
        check(adBlockForeignHostsLines(rewritten, adBlockManagedNames("gb", "")) ==
                  std::vector<std::string>({"0.0.0.0 tracker.example.net",
                                            ":: tracker.example.net"}),
              "another tool's sinkholes survive the table being written and read again");
        const std::string off = adBlockHostsTable(
            "off", "gb", "", false, false, nullptr, {"0.0.0.0 tracker.example.net"});
        check(!sinkholed(off, "ad.lgsmartad.com") &&
                  off.find("0.0.0.0 tracker.example.net\n") != std::string::npos,
              "with the tier off another tool's sinkhole is still in the table");
    }

    const std::string table = adBlockHostsTable(
        "ads", "gb", "nativetv.local", true, true, &custom, foreign);
    check(table.find("127.0.0.1\tlocalhost.localdomain\tlocalhost\n") == 0,
          "hosts table retains localhost entries");
    check(table.find("# LG Ad & Telemetry Blackhole (lg-webos-dashboard)") !=
              std::string::npos && sinkholed(table, "ad.doubleclick.net"),
          "hosts table carries its marker and dual-stack built-in entries");
    check(sinkholed(table, "snu.lge.com") && sinkholed(table, "su-ssl.lge.com") &&
              sinkholed(table, "nsu.lge.com") && sinkholed(table, "snu-dev.lge.com"),
          "TV update servers are blocked only when their flag is set");
    check(table.find("127.0.0.1 cloudfront.xp-assets.aiv-cdn.net\n") !=
              std::string::npos,
          "hosts table retains foreign mappings");

    const std::string noUpdates = adBlockHostsTable(
        "off", "gb", "", true, false, nullptr, {});
    check(noUpdates.find("snu.lge.com") == std::string::npos &&
              noUpdates.find("ad.doubleclick.net") == std::string::npos,
          "off mode leaves both advertising and TV update hosts reachable");
    // test/test-privacy.js: the built-in tables, on both address families.
    const std::vector<std::string> commonNetworks = {
        "ad.doubleclick.net", "aax.amazon-adsystem.com", "bat.bing.com", "ib.adnxs.com",
        "ads.pubmatic.com", "fastlane.rubiconproject.com", "sb.scorecardresearch.com",
        "api.segment.io", "events.appsflyer.com"};
    const std::vector<std::string> reachable = {
        "youtube.com", "www.youtube.com", "googlevideo.com", "i.ytimg.com", "accounts.google.com",
        "netflix.com", "www.netflix.com", "nflxvideo.net", "primevideo.com", "www.amazon.com",
        "cloudfront.net", "cloudfront.xp-assets.aiv-cdn.net", "akamaized.net", "lgtvsdp.com",
        "gb.lgtvsdp.com", "nextlgsdp.com", "gb.nextlgsdp.com", "wiselg.com", "gb.tv.wiselg.com",
        "lgtviot.com", "ueiwsp.com", "snu.lge.com", "nsu.lge.com", "snu-dev.lge.com", "su-dev.lge.com", "su.lge.com",
        "su-ssl.lge.com"};
    for (const char *mode : {"ads", "full"}) {
        const std::string tier = mode;
        const std::string tierTable = adBlockHostsTable(tier, "", "", false, false, nullptr, {});
        const std::vector<std::string> hosts = adBlockList(tier, "", "", false, nullptr);
        for (const std::string &host : hosts)
            check(sinkholed(tierTable, host), tier + ": " + host + " is missing an IPv4 or IPv6 line");
        for (const std::string &host : commonNetworks)
            check(sinkholed(tierTable, host), tier + ": " + host + " is not blocked");
        check(std::set<std::string>(hosts.begin(), hosts.end()).size() == hosts.size(),
              tier + ": the built-in hosts are unique");
        // Every name is two lines the TV reads on each lookup. Before the TV's country is
        // known the regional names are listed for seven countries, so the larger
        // Everything tier gets more room until it is.
        check(tierTable.size() < (tier == "full" ? 32768u : 24576u),
              tier + ": the hosts table stays small enough for older TVs, country not known");
        check(adBlockHostsTable(tier, "gb", "", false, false, nullptr, {}).size() < 24576,
              tier + ": the hosts table stays under 24 KiB for older TVs, country known");
        check(has(hosts, "cdpsvc.lgtvcommon.com") && has(hosts, "gb.cdpsvc.lgtvcommon.com"),
              tier + ": the customer data host is blocked");
    }
    const std::string adsOnly = adBlockHostsTable("ads", "", "", false, false, nullptr, {});
    check(sinkholed(adBlockHostsTable("full", "", "", false, false, nullptr, {}),
                    "avsxappcaptiveportal.com") &&
              !sinkholed(adsOnly, "avsxappcaptiveportal.com"),
          "Alexa's internet check is blocked on the everything tier only");
    for (const std::string &host : reachable)
        check(!sinkholed(adsOnly, host), host + " must not be sinkholed by the ads tier");
    check(adBlockList("off", "", "", false, nullptr).empty() &&
              !sinkholed(adBlockHostsTable("off", "", "", false, false, nullptr, {}), "ad.lgsmartad.com"),
          "with the ad blocker off the table carries no ad hosts");
    check(adsOnly.find("::1\tlocalhost ip6-localhost ip6-loopback") != std::string::npos,
          "localhost keeps its IPv6 entry");
    checkRefreshedLists();
    checkStore();
    checkLocalLookups();
    std::cout << "native ad-block policy checks passed\n";
    return 0;
}
