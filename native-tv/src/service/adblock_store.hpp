#pragma once

#include "core/json.hpp"
#include "service/adblock.hpp"
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace nativetv {
namespace service {

struct AdBlockMountOperations {
    std::function<bool(const std::string &, const std::string &)> bind;
    std::function<bool(const std::string &)> unmount;
    // The TV's IPv4 addresses, replaced in a test.
    std::function<std::vector<LocalInterface>()> interfaces;
};

class AdBlockStore {
  public:
    AdBlockStore(std::string dataRoot, std::string systemRoot,
                 AdBlockMountOperations mountOperations = AdBlockMountOperations());

    std::string mode() const;
    bool tvUpdatesBlocked() const;
    // Whether LG's device scans are answered on the TV (see adblock.hpp).
    bool localLookups() const;
    bool tableMounted() const;
    bool waitsForSdp() const;
    Json privacyState(bool sdpGraceActive) const;
    bool updateCountry(const std::string &country, bool sdpGraceActive,
                       std::string *error = nullptr) const;
    bool restoreAtBoot(bool sdpGraceActive, std::string *error = nullptr) const;
    bool refreshMountedTable(bool sdpGraceActive, std::string *error = nullptr) const;
    Json setMode(const std::string &mode, bool sdpGraceActive) const;
    Json setTvUpdatesBlocked(bool blocked, bool sdpGraceActive) const;
    Json setLocalLookups(bool on, bool sdpGraceActive) const;
    // Rewrites the mounted table when the TV's network is not the one it was
    // last written for: the TV can move to another network or be given
    // another address.
    bool refreshForNetworkChange(bool sdpGraceActive, std::string *error = nullptr) const;
    Json setCustomHosts(const Json &options, bool sdpGraceActive) const;

    const std::string &dataRoot() const;
    const std::string &systemRoot() const;

  private:
    std::string rootedSystemPath(const std::string &absolutePath) const;
    std::string dataPath(const std::string &filename) const;
    std::string country() const;
    std::string serverHost() const;
    AdBlockCustomList customHosts() const;
    Json customSummary(const AdBlockCustomList *custom,
                       bool sdpGraceActive) const;
    bool writeHostsTable(const std::string &mode, bool sdpGraceActive,
                         const std::vector<std::string> &foreign,
                         std::string *error) const;
    bool applyHostsTable(bool sdpGraceActive, std::string *error) const;
    bool applyCustomHosts(bool sdpGraceActive, std::string *error) const;
    std::vector<std::string> currentForeignHostsLines() const;
    std::vector<std::string> localLookupAddresses() const;

    std::string dataRoot_;
    std::string systemRoot_;
    AdBlockMountOperations mountOperations_;
    mutable std::mutex mutex_;
    // The addresses the table was last written for, as one string.
    mutable std::string localNetworkKey_;
};

} // namespace service
} // namespace nativetv
