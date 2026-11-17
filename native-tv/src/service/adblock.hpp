#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace nativetv {
namespace service {

struct AdBlockCustomList {
    std::string mode;
    std::vector<std::string> names;
    bool enabled = true;
    std::string file;
    std::string updated;
};

struct AdBlockParseResult {
    std::vector<std::string> names;
    size_t skipped = 0;
};

AdBlockParseResult parseAdBlockCustomHosts(const std::string &text);
// Whether a name is one of the clock servers' (nextlgsdp.com on webOS 9,
// tv.wiselg.com on webOS 11, and anything under either): the TV sets its clock
// from them, so they stay reachable until it is valid. The Ads & telemetry tier
// has one such name, and the owner's list can name any.
bool adBlockIsSdpName(const std::string &name);
// Whether a list of blocked names holds the TV's country's own clock server
// (<country>.nextlgsdp.com or <country>.tv.wiselg.com; any of the fallback
// countries while the TV's is not known).
bool adBlockBlocksClockServer(const std::vector<std::string> &blocked,
                              const std::string &country);
std::vector<std::string> adBlockAds(const std::string &country);
std::vector<std::string> adBlockPlatform(const std::string &country,
                                         const std::string &serverHost,
                                         bool sdpGraceActive);
std::vector<std::string> adBlockManagedNames(const std::string &country,
                                             const std::string &serverHost);
std::vector<std::string> adBlockOwnList(const std::string &mode,
                                        const std::string &country,
                                        const std::string &serverHost,
                                        bool sdpGraceActive);
std::vector<std::string> adBlockList(const std::string &mode,
                                    const std::string &country,
                                    const std::string &serverHost,
                                    bool sdpGraceActive,
                                    const AdBlockCustomList *custom);
// The names the TV blocks at a level ("ads" or "full"), with a custom list as it is
// used and the TV software update servers when those are blocked.
std::vector<std::string> adBlockExportNames(const std::string &mode,
                                            const std::string &country,
                                            const std::string &serverHost,
                                            const AdBlockCustomList *custom,
                                            bool tvUpdatesBlocked);
// A list of names as another blocker reads it: "hosts" (0.0.0.0 lines), "pihole"
// (one name per line) or "adguard" (||name^ rules). Empty for any other format.
std::string adBlockExport(const std::string &format,
                          const std::vector<std::string> &names);
// Whether a hosts line names an address as itself, as the table did for "Answer device
// scans on the TV" in 0.124.0 and earlier, so it is not carried over as another tool's.
bool adBlockIsLegacyLookupLine(const std::string &line);
std::vector<std::string> adBlockForeignHostsLines(
    const std::string &text, const std::vector<std::string> &managedNames);
std::string adBlockHostsTable(const std::string &mode,
                              const std::string &country,
                              const std::string &serverHost,
                              bool sdpGraceActive,
                              bool tvUpdatesBlocked,
                              const AdBlockCustomList *custom,
                              const std::vector<std::string> &foreignLines);

} // namespace service
} // namespace nativetv
