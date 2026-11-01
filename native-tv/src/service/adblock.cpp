#include "service/adblock.hpp"
#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>

namespace nativetv {
namespace service {
namespace {
const char *const kMarker =
    "# LG Ad & Telemetry Blackhole (lg-webos-dashboard)";

const std::vector<std::string> kAdHosts = {
    "ad.lgsmartad.com", "ibis.lgappstv.com", "ibs.lgappstv.com",
    "lgsmartad.com", "rdx.lgtvcommon.com", "aic.lgtvcommon.com",
    "smartad.lge.com", "smartclip.com", "smartclip-services.com",
    "yumenetworks.com", "alphonso.tv", "prov-lg.alphonso.tv",
    "prov.alphonso.tv", "eulacheck.alphonso.tv", "bl-server.alphonso.tv",
    "clockskew.alphonso.tv", "acrdb.alphonso.tv", "ads.alphonso.tv",
    "api.alphonso.tv", "tvads.alphonso.tv", "sync.alphonso.tv",
    "tr.alphonso.tv", "tn.alphonso.tv", "insights.alphonso.tv",
    "bwlkup.alphonso.tv", "eu-acr86.alphonso.tv",
    "ad.doubleclick.net", "googleads.g.doubleclick.net",
    "stats.g.doubleclick.net", "securepubads.g.doubleclick.net",
    "pubads.g.doubleclick.net", "cm.g.doubleclick.net",
    "adclick.g.doubleclick.net", "fls.doubleclick.net",
    "static.doubleclick.net", "adservice.google.com",
    "www.googleadservices.com", "pagead2.googlesyndication.com",
    "pagead.googlesyndication.com", "tpc.googlesyndication.com",
    "www.google-analytics.com", "ssl.google-analytics.com",
    "region1.google-analytics.com", "analytics.google.com",
    "app-measurement.com", "aax.amazon-adsystem.com",
    "aax-us-east.amazon-adsystem.com", "aax-eu.amazon-adsystem.com",
    "c.amazon-adsystem.com", "mads.amazon-adsystem.com",
    "s.amazon-adsystem.com", "device-metrics-us.amazon.com",
    "device-metrics-us-2.amazon.com", "bat.bing.com", "ads.msn.com",
    "adnxs.com", "ib.adnxs.com", "secure.adnxs.com", "acdn.adnxs.com",
    "prebid.adnxs.com", "ads.pubmatic.com", "image6.pubmatic.com",
    "image8.pubmatic.com", "hbopenbid.pubmatic.com",
    "fastlane.rubiconproject.com", "pixel.rubiconproject.com",
    "token.rubiconproject.com", "eus.rubiconproject.com", "us-u.openx.net",
    "us-ads.openx.net", "rtb.openx.net", "sync.adap.tv",
    "ads.adaptv.advertising.com", "adsrvr.org", "ad.adsrvr.org",
    "insight.adsrvr.org", "match.adsrvr.org", "d.turn.com", "r.turn.com",
    "ads.stickyadstv.com", "sb.scorecardresearch.com",
    "b.scorecardresearch.com", "pixel.quantserve.com",
    "secure.quantserve.com", "edge.quantserve.com", "rules.quantcount.com",
    "pixel.adsafeprotected.com", "static.adsafeprotected.com",
    "js.adsrvr.org", "px.moatads.com", "z.moatads.com",
    "js-agent.newrelic.com", "bam.nr-data.net", "api.segment.io",
    "cdn.segment.com", "cdn.segment.io", "api2.amplitude.com",
    "api.amplitude.com", "api-js.mixpanel.com", "api.mixpanel.com",
    "cdn.mxpnl.com", "events.appsflyer.com", "t.appsflyer.com",
    "app.adjust.com", "sessions.bugsnag.com"
};

const std::vector<std::string> kRegionalNames = {
    "info.lgsmartad.com", "rdx2.lgtvsdp.com", "ibs.lgappstv.com",
    "ibsstat.lgappstv.com", "cdpbeacon.lgtvcommon.com",
    "cdpsvc.lgtvcommon.com", "wau.lgtvcommon.com"
};
const std::vector<std::string> kRegionPrefixes = {"aic", "eic", "kic", "eu"};
const std::vector<std::string> kFallbackCountries = {
    "us", "gb", "au", "br", "ca", "de", "fr"
};
const std::vector<std::string> kPlatformHosts = {
    "lgtvsdp.com", "us.lgtvsdp.com", "gb.lgtvsdp.com", "eu.lgtvsdp.com",
    "ngfts.lge.com", "aic-ngfts.lge.com", "cdplauncher.lgtvcommon.com",
    "lgchhomeapp.lgtvcommon.com", "lgtviot.com", "api.lgtviot.com",
    "push.lgtviot.com", "ocp.lgtviot.com", "common.lgthinq.com",
    "connect-client.lgthinq.com", "ueiwsp.com",
    // Amazon's Alexa SDK (libAVSCommon) fetches /generate_204 from it on a
    // timer to tell whether the TV is online, while Alexa runs; blocked,
    // Alexa reads the TV as offline.
    "avsxappcaptiveportal.com"
};
const std::vector<std::string> kSdpHosts = {
    "nextlgsdp.com", "us.nextlgsdp.com", "gb.nextlgsdp.com",
    "eu.nextlgsdp.com"
};
const std::vector<std::string> kUpdateHosts = {
    "snu.lge.com", "su-dev.lge.com", "su.lge.com", "su-ssl.lge.com"
};
const std::vector<std::string> kCustomSinkAddresses = {
    "0.0.0.0", "127.0.0.1", "::", "::1"
};
const std::vector<std::string> kCustomExcludedNames = {
    "localhost", "localhost.localdomain", "local", "broadcasthost",
    "ip6-localhost", "ip6-loopback", "ip6-localnet", "ip6-mcastprefix",
    "ip6-allnodes", "ip6-allrouters", "ip6-allhosts", "0.0.0.0"
};

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string trim(const std::string &value) {
    size_t start = 0;
    while (start < value.size() &&
           std::isspace(static_cast<unsigned char>(value[start])))
        ++start;
    size_t end = value.size();
    while (end > start &&
           std::isspace(static_cast<unsigned char>(value[end - 1])))
        --end;
    return value.substr(start, end - start);
}

bool contains(const std::vector<std::string> &values, const std::string &value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

void appendUnique(std::vector<std::string> &values, const std::string &value) {
    if (!contains(values, value))
        values.push_back(value);
}

bool validCountry(const std::string &country) {
    return country.size() == 2 &&
        std::isalpha(static_cast<unsigned char>(country[0])) &&
        std::isalpha(static_cast<unsigned char>(country[1]));
}

bool validDomainName(const std::string &name) {
    if (name.empty() || name.size() > 253 || name[0] == '.' || name[name.size() - 1] == '.')
        return false;
    size_t start = 0;
    while (start < name.size()) {
        const size_t end = name.find('.', start);
        const size_t labelEnd = end == std::string::npos ? name.size() : end;
        if (labelEnd == start)
            return false;
        const auto validEdge = [](unsigned char c) {
            return std::isalnum(c) || c == '_';
        };
        if (!validEdge(static_cast<unsigned char>(name[start])) ||
            !validEdge(static_cast<unsigned char>(name[labelEnd - 1])))
            return false;
        for (size_t i = start + 1; i + 1 < labelEnd; ++i) {
            const unsigned char c = static_cast<unsigned char>(name[i]);
            if (!(std::isalnum(c) || c == '_' || c == '-'))
                return false;
        }
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    if (name.find('.') == std::string::npos)
        return false;
    const size_t lastDot = name.rfind('.');
    if (lastDot + 1 < name.size() &&
        std::all_of(name.begin() + static_cast<std::ptrdiff_t>(lastDot + 1),
                    name.end(), [](unsigned char c) { return std::isdigit(c); }))
        return false;
    return true;
}

bool isSdpName(const std::string &name) {
    const std::string suffix = "nextlgsdp.com";
    return name == suffix ||
        (name.size() > suffix.size() &&
         name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0 &&
         name[name.size() - suffix.size() - 1] == '.');
}

// A dotted IPv4 address, or false.
bool parseIp4(const std::string &text, unsigned &address) {
    unsigned long long result = 0;
    size_t parts = 0;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t dot = text.find('.', start);
        const std::string part = text.substr(
            start, dot == std::string::npos ? std::string::npos : dot - start);
        if (part.empty() || part.size() > 3 ||
            !std::all_of(part.begin(), part.end(), [](unsigned char c) {
                return std::isdigit(c) != 0;
            }))
            return false;
        const unsigned long value = std::stoul(part);
        if (value > 255)
            return false;
        result = result * 256 + value;
        ++parts;
        if (dot == std::string::npos)
            break;
        start = dot + 1;
    }
    if (parts != 4)
        return false;
    address = static_cast<unsigned>(result);
    return true;
}

std::string ip4Text(unsigned address) {
    return std::to_string(address >> 24) + "." +
           std::to_string((address >> 16) & 255) + "." +
           std::to_string((address >> 8) & 255) + "." +
           std::to_string(address & 255);
}

// 10/8, 172.16/12 and 192.168/16.
bool isPrivate4(unsigned address) {
    return (address >= 0x0a000000u && address <= 0x0affffffu) ||
           (address >= 0xac100000u && address <= 0xac1fffffu) ||
           (address >= 0xc0a80000u && address <= 0xc0a8ffffu);
}

void sinkhole(std::ostringstream &output, const std::string &host) {
    output << "0.0.0.0\t" << host << "\n::\t" << host << '\n';
}
} // namespace

AdBlockParseResult parseAdBlockCustomHosts(const std::string &text) {
    AdBlockParseResult result;
    std::vector<std::string> seen;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t end = text.find('\n', start);
        std::string line = text.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        const size_t comment = line.find('#');
        if (comment != std::string::npos)
            line.erase(comment);
        line = trim(line);
        if (!line.empty()) {
            std::istringstream words(line);
            std::vector<std::string> parts;
            std::string part;
            while (words >> part)
                parts.push_back(part);
            if (!parts.empty()) {
                const std::string address = parts[0];
                const bool ipv4Address = std::all_of(
                    address.begin(), address.end(), [](unsigned char c) {
                        return std::isdigit(c) || c == '.';
                    });
                const bool ipv6Address = address.find(':') != std::string::npos;
                if (ipv4Address || ipv6Address) {
                    if (!contains(kCustomSinkAddresses, address)) {
                        ++result.skipped;
                        parts.clear();
                    } else {
                        parts.erase(parts.begin());
                    }
                } else if (parts.size() > 1) {
                    ++result.skipped;
                    parts.clear();
                }
                for (std::string name : parts) {
                    name = lower(name);
                    if (!name.empty() && name[name.size() - 1] == '.')
                        name.erase(name.size() - 1);
                    if (contains(kCustomExcludedNames, name) || contains(seen, name))
                        continue;
                    if (!validDomainName(name)) {
                        ++result.skipped;
                        continue;
                    }
                    seen.push_back(name);
                    result.names.push_back(name);
                }
            }
        }
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return result;
}

std::vector<std::string> adBlockAds(const std::string &countryValue) {
    std::vector<std::string> result = kAdHosts;
    const std::string country = lower(countryValue);
    std::vector<std::string> prefixes = kRegionPrefixes;
    if (validCountry(country))
        appendUnique(prefixes, country);
    else
        for (const std::string &fallback : kFallbackCountries)
            appendUnique(prefixes, fallback);
    for (const std::string &base : kRegionalNames) {
        appendUnique(result, base);
        for (const std::string &prefix : prefixes)
            appendUnique(result, prefix + "." + base);
    }
    return result;
}

std::vector<std::string> adBlockPlatform(const std::string &country,
                                         const std::string &serverHost,
                                         bool sdpGraceActive) {
    (void)country;
    std::vector<std::string> result = kPlatformHosts;
    if (!serverHost.empty())
        appendUnique(result, serverHost);
    if (!sdpGraceActive)
        for (const std::string &host : kSdpHosts)
            appendUnique(result, host);
    return result;
}

std::vector<std::string> adBlockManagedNames(const std::string &country,
                                             const std::string &serverHost) {
    std::vector<std::string> result = adBlockAds(country);
    for (const std::string &host : adBlockPlatform(country, serverHost, false))
        appendUnique(result, host);
    for (const std::string &host : kUpdateHosts)
        appendUnique(result, host);
    return result;
}

std::vector<std::string> adBlockOwnList(const std::string &mode,
                                        const std::string &country,
                                        const std::string &serverHost,
                                        bool sdpGraceActive) {
    if (mode == "off")
        return {};
    std::vector<std::string> result = adBlockAds(country);
    if (mode == "full") {
        const std::vector<std::string> platform =
            adBlockPlatform(country, serverHost, sdpGraceActive);
        for (const std::string &host : platform)
            appendUnique(result, host);
    }
    return result;
}

std::vector<std::string> adBlockList(const std::string &mode,
                                    const std::string &country,
                                    const std::string &serverHost,
                                    bool sdpGraceActive,
                                    const AdBlockCustomList *custom) {
    std::vector<std::string> own =
        adBlockOwnList(mode, country, serverHost, sdpGraceActive);
    if (mode == "off" || !custom || !custom->enabled ||
        (custom->mode != "add" && custom->mode != "replace"))
        return own;
    std::vector<std::string> customNames;
    for (const std::string &name : custom->names)
        if (!sdpGraceActive || !isSdpName(name))
            appendUnique(customNames, name);
    if (custom->mode == "replace")
        return customNames;
    for (const std::string &name : customNames)
        appendUnique(own, name);
    return own;
}

std::vector<std::string> adBlockLocalLookupAddresses(
    const std::vector<LocalInterface> &interfaces) {
    // Each network is the TV's subnet where that is a /24 or smaller, otherwise
    // the /24 around the TV, which is what the scans cover. The TV's own
    // addresses are left out: the daemon looks those up to name the TV.
    std::set<unsigned> own;
    std::vector<std::pair<unsigned, unsigned>> networks;
    for (const LocalInterface &entry : interfaces) {
        if (!isPrivate4(entry.address))
            continue;
        own.insert(entry.address);
        unsigned long long size = 4294967296ULL - entry.netmask;
        if (!(size > 0 && size <= 256))
            size = 256;
        networks.push_back({entry.address - entry.address % static_cast<unsigned>(size),
                            static_cast<unsigned>(size)});
    }
    std::vector<std::string> result;
    std::set<unsigned> seen;
    for (const auto &network : networks)
        for (unsigned host = 1; host + 1 < network.second; ++host) {
            const unsigned address = network.first + host;
            if (own.count(address) == 0 && seen.insert(address).second)
                result.push_back(ip4Text(address));
        }
    return result;
}

bool adBlockIsLocalLookupLine(const std::string &line) {
    std::istringstream words(line);
    std::string first, second, extra;
    unsigned address = 0;
    return (words >> first >> second) && !(words >> extra) && first == second &&
           parseIp4(first, address);
}

std::vector<std::string> adBlockForeignHostsLines(
    const std::string &text, const std::vector<std::string> &managedNames) {
    std::vector<std::string> kept;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t end = text.find('\n', start);
        const std::string line = trim(text.substr(
            start, end == std::string::npos ? std::string::npos : end - start));
        if (!line.empty() && line[0] != '#') {
            std::istringstream words(line);
            std::vector<std::string> parts;
            std::string part;
            while (words >> part)
                parts.push_back(part);
            const bool sinkholeLine =
                (line.size() > 7 && line.compare(0, 7, "0.0.0.0") == 0 &&
                 std::isspace(static_cast<unsigned char>(line[7]))) ||
                (line.size() > 2 && line.compare(0, 2, "::") == 0 &&
                 std::isspace(static_cast<unsigned char>(line[2])));
            if (!sinkholeLine && line.find("localhost") == std::string::npos &&
                line.find("ip6-") == std::string::npos &&
                !adBlockIsLocalLookupLine(line)) {
                bool isManaged = false;
                for (size_t i = 1; i < parts.size() && !isManaged; ++i)
                    isManaged = contains(managedNames, lower(parts[i]));
                if (!isManaged)
                    appendUnique(kept, line);
            }
        }
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return kept;
}

std::string adBlockHostsTable(const std::string &mode,
                              const std::string &country,
                              const std::string &serverHost,
                              bool sdpGraceActive,
                              bool tvUpdatesBlocked,
                              const AdBlockCustomList *custom,
                              const std::vector<std::string> &foreignLines,
                              const std::vector<std::string> &localLookupAddresses) {
    std::ostringstream output;
    output << "127.0.0.1\tlocalhost.localdomain\tlocalhost\n"
           << "::1\tlocalhost ip6-localhost ip6-loopback\n"
           << "fe00::0\tip6-localnet\n"
           << "ff00::0\tip6-mcastprefix\n"
           << "ff02::1\tip6-allnodes\n"
           << "ff02::2\tip6-allrouters\n\n"
           << kMarker << '\n';
    for (const std::string &host : adBlockList(
             mode, country, serverHost, sdpGraceActive, custom))
        sinkhole(output, host);
    if (tvUpdatesBlocked) {
        output << "\n# TV software updates, blocked by the flag the Homebrew Channel also uses\n";
        for (const std::string &host : kUpdateHosts)
            sinkhole(output, host);
    }
    if (!localLookupAddresses.empty()) {
        output << "\n# The local network, so LG's device scans are answered on the TV\n";
        for (const std::string &address : localLookupAddresses)
            output << address << '\t' << address << '\n';
    }
    if (!foreignLines.empty()) {
        output << "\n# Kept from /etc/hosts, where other tools put them\n";
        for (const std::string &line : foreignLines)
            output << line << '\n';
    }
    output << '\n';
    return output.str();
}

} // namespace service
} // namespace nativetv
