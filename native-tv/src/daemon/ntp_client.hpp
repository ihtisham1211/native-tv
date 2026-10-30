#pragma once

#include "core/json.hpp"
#include "daemon/event_loop.hpp"
#include "service/ntp.hpp"
#include <memory>
#include <mutex>
#include <functional>
#include <string>

namespace nativetv {
namespace daemon {
class Logger;
class LunaClient;
class LunaSubscription;

class NtpClient {
  public:
    NtpClient(EventLoop &loop, LunaClient &luna, Logger *logger = nullptr);
    NtpClient(const NtpClient &) = delete;
    NtpClient &operator=(const NtpClient &) = delete;

    bool start(const Json &config);
    bool watchConfig(const std::string &path, const Json &config);
    void setStatusChanged(std::function<void(const Json &)> callback);
    void sync();
    void stop();
    Json status() const;

  private:
    bool startConfigured(const Json &config);
    void stopClient();
    void checkConfigFile();
    void publishStatus() const;
    void syncOnLoop();
    void completeQuery(unsigned long long generation,
                       const service::NtpResult &result);
    void setClock(unsigned long long generation,
                  const service::NtpAnswer &answer);
    void syncFailed(unsigned long long generation, const std::string &message);
    void setError(unsigned long long generation, const std::string &message);
    void schedule(std::chrono::milliseconds delay);

    EventLoop &loop_;
    LunaClient &luna_;
    Logger *logger_;
    mutable std::mutex mutex_;
    std::shared_ptr<LunaSubscription> powerSubscription_;
    service::NtpSettings settings_;
    service::NtpAnswer lastAnswer_;
    EventLoop::TimerId timer_ = 0;
    EventLoop::TimerId configTimer_ = 0;
    unsigned long long generation_ = 0;
    unsigned sets_ = 0;
    unsigned errors_ = 0;
    double lastSetMonoMs_ = 0;
    bool running_ = false;
    bool busy_ = false;
    bool appliedOnce_ = false;
    bool hasLastAnswer_ = false;
    bool hasLastSet_ = false;
    bool powerStateKnown_ = false;
    bool powerWasOn_ = false;
    std::string error_;
    std::string configPath_;
    std::string ntpConfigSnapshot_;
    std::string configStamp_; // fileStamp() of the config last read successfully
    std::function<void(const Json &)> statusChanged_;
};
} // namespace daemon
} // namespace nativetv
