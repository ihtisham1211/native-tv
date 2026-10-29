#include "daemon/mqtt_topics.hpp"
#include <utility>

namespace nativetv {
namespace daemon {

MqttTopics::MqttTopics(std::string prefix)
    : prefix_(prefix.empty() ? "lgtv" : std::move(prefix)) {}

const std::string &MqttTopics::prefix() const { return prefix_; }
std::string MqttTopics::status() const { return prefix_ + "/status"; }
std::string MqttTopics::telemetry() const { return prefix_ + "/telemetry"; }
std::string MqttTopics::commandPrefix() const { return prefix_ + "/command/"; }
std::string MqttTopics::command(const std::string &name) const {
    return commandPrefix() + name;
}
std::string MqttTopics::state(const std::string &path) const {
    return prefix_ + "/state/" + path;
}
std::string MqttTopics::eventsButton() const { return prefix_ + "/events/button"; }

} // namespace daemon
} // namespace nativetv
