#pragma once

#include <string>

namespace nativetv {
namespace daemon {

// Topic names shared by MQTT state, discovery and commands.
class MqttTopics {
  public:
    explicit MqttTopics(std::string prefix = "lgtv");

    const std::string &prefix() const;
    std::string status() const;
    std::string telemetry() const;
    std::string commandPrefix() const;
    std::string command(const std::string &name) const;
    std::string state(const std::string &path) const;
    std::string eventsButton() const;

  private:
    std::string prefix_;
};

} // namespace daemon
} // namespace nativetv
