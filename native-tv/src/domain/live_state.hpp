#pragma once

#include "core/json.hpp"
#include <cstdint>
#include <functional>
#include <string>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace nativetv {
namespace state {

struct Event {
    std::string group;
    std::string key;
    Json value;
    std::int64_t sourceTime = 0;
    bool snapshot = false;
    bool tvReported = false;
};

class Cache {
  public:
    using Listener = std::function<void(const Event &)>;

    void onChange(Listener listener);
    bool update(const Event &event);
    Json snapshot() const;

  private:
    struct Entry {
        Json value;
        std::int64_t sourceTime = 0;
    };
    std::unordered_map<std::string, std::unordered_map<std::string, Entry>> values_;
    std::vector<Listener> listeners_;
    mutable std::mutex mutex_;
};

struct MappingOptions {
    // Maps reported short app ids to the TV's HDMI key when they differ.
    Json inputAliases;
    // Maps HDMI keys to the labels supplied by webOS input status.
    Json inputNames;
};

// Map one successful Luna subscription reply to the same primitive state
// values consumed by history, Home Assistant and the TV dashboard.
Json mapSubscription(const std::string &group, const Json &response,
                     const MappingOptions &options = MappingOptions());

} // namespace state
} // namespace nativetv
