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

// One IPv4 address of the TV and its netmask, in host byte order.
struct LocalInterface {
    unsigned address = 0;
    unsigned netmask = 0;
};

AdBlockParseResult parseAdBlockCustomHosts(const std::string &text);
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
// The addresses LG's device scans reverse-look-up: every host on each private
// IPv4 network the TV is on, less the TV's own addresses.
std::vector<std::string> adBlockLocalLookupAddresses(
    const std::vector<LocalInterface> &interfaces);
// Whether a hosts line names an address as itself, as the table does for the
// local lookups, so it is not carried over as another tool's.
bool adBlockIsLocalLookupLine(const std::string &line);
std::vector<std::string> adBlockForeignHostsLines(
    const std::string &text, const std::vector<std::string> &managedNames);
std::string adBlockHostsTable(const std::string &mode,
                              const std::string &country,
                              const std::string &serverHost,
                              bool sdpGraceActive,
                              bool tvUpdatesBlocked,
                              const AdBlockCustomList *custom,
                              const std::vector<std::string> &foreignLines,
                              const std::vector<std::string> &localLookupAddresses =
                                  std::vector<std::string>());

} // namespace service
} // namespace nativetv
