#pragma once

#include "daemon/event_loop.hpp"
#include "domain/live_state.hpp"
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <string>

namespace nativetv {
namespace daemon {
class Logger;
class LunaClient;
class LunaSubscription;

// Owns the native equivalents of state.js's reference-counted Luna groups.
// Lifecycle methods are called on the EventLoop thread; cache reads are safe
// from HTTP workers.
class StateSubscriptions {
  public:
    using Invalidate = std::function<void(const std::string &)>;
    using StatusChange = std::function<void(const std::string &, bool)>;

    StateSubscriptions(EventLoop &loop, LunaClient &luna,
                       state::MappingOptions mapping = state::MappingOptions(),
                       Logger *logger = nullptr, Invalidate invalidate = Invalidate(),
                       StatusChange statusChange = StatusChange());
    ~StateSubscriptions();
    StateSubscriptions(const StateSubscriptions &) = delete;
    StateSubscriptions &operator=(const StateSubscriptions &) = delete;

    bool start();
    bool stop();
    bool startGroup(const std::string &name);
    bool stopGroup(const std::string &name);
    bool isLive(const std::string &name) const;
    Json snapshot() const;
    state::Cache &cache();
    const state::Cache &cache() const;

  private:
    struct Group {
        std::string name;
        std::string uri;
        Json payload;
        std::shared_ptr<LunaSubscription> subscription;
        std::atomic<unsigned> users{0};
        std::atomic<bool> snapshotPending{true};
        std::atomic<bool> live{false};
        unsigned long long generation = 0;
    };

    void defineGroups();
    std::shared_ptr<LunaSubscription> makeSubscription(Group &group,
                                                        unsigned long long generation);
    void receive(Group &group, unsigned long long generation, const Json &response);
    void refused(Group &group, const Json &response);
    void acquire(Group &group);
    void release(Group &group);
    void setLive(Group &group, bool live);

    EventLoop &loop_;
    LunaClient &luna_;
    state::MappingOptions mapping_;
    Logger *logger_;
    Invalidate invalidate_;
    StatusChange statusChange_;
    std::string mediaSeenPath_;
    state::Cache cache_;
    std::map<std::string, std::unique_ptr<Group>> groups_;
};
} // namespace daemon
} // namespace nativetv
