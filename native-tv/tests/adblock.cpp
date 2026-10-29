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

    const std::vector<std::string> foreign = adBlockForeignHostsLines(
        "# comment\n0.0.0.0 ad.doubleclick.net\n:: su.lge.com\n"
        "127.0.0.1 cloudfront.xp-assets.aiv-cdn.net\n"
        "::1 cloudfront.xp-assets.aiv-cdn.net\n"
        "192.168.1.5 nas\n192.168.1.5 nas\n0.0.0.0\n",
        {"ad.doubleclick.net", "su.lge.com"});
    check(foreign == std::vector<std::string>({
              "127.0.0.1 cloudfront.xp-assets.aiv-cdn.net",
              "::1 cloudfront.xp-assets.aiv-cdn.net", "192.168.1.5 nas",
              "0.0.0.0"}),
          "foreign host preservation excludes managed and sinkhole lines and removes duplicates");

    const std::string table = adBlockHostsTable(
        "ads", "gb", "nativetv.local", true, true, &custom, foreign);
    check(table.find("127.0.0.1\tlocalhost.localdomain\tlocalhost\n") == 0,
          "hosts table retains localhost entries");
    check(table.find("# LG Ad & Telemetry Blackhole (lg-webos-dashboard)") !=
              std::string::npos && sinkholed(table, "ad.doubleclick.net"),
          "hosts table carries its marker and dual-stack built-in entries");
    check(sinkholed(table, "snu.lge.com") && sinkholed(table, "su-ssl.lge.com"),
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
        "lgtviot.com", "ueiwsp.com", "snu.lge.com", "su-dev.lge.com", "su.lge.com", "su-ssl.lge.com"};
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
        check(tierTable.size() < 24576, tier + ": the hosts table stays under 24 KiB for older TVs");
        check(has(hosts, "cdpsvc.lgtvcommon.com") && has(hosts, "gb.cdpsvc.lgtvcommon.com"),
              tier + ": the customer data host is blocked");
    }
    const std::string adsOnly = adBlockHostsTable("ads", "", "", false, false, nullptr, {});
    for (const std::string &host : reachable)
        check(!sinkholed(adsOnly, host), host + " must not be sinkholed by the ads tier");
    check(adBlockList("off", "", "", false, nullptr).empty() &&
              !sinkholed(adBlockHostsTable("off", "", "", false, false, nullptr, {}), "ad.lgsmartad.com"),
          "with the ad blocker off the table carries no ad hosts");
    check(adsOnly.find("::1\tlocalhost ip6-localhost ip6-loopback") != std::string::npos,
          "localhost keeps its IPv6 entry");
    checkStore();
    std::cout << "native ad-block policy checks passed\n";
    return 0;
}
