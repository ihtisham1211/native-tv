#pragma once

#include "daemon/mqtt_topics.hpp"
#include <functional>
#include <string>

namespace nativetv {
namespace daemon {

// Keeps Home Assistant availability aligned with the TV's power state while
// preserving a separate will for broker/network loss during standby.
class MqttAvailabilityPublisher {
  public:
    using Publish = std::function<bool(const std::string &, const std::string &, bool)>;
    using SetWill = std::function<bool(const std::string &)>;

    MqttAvailabilityPublisher(MqttTopics topics, Publish publish, SetWill setWill);

    void setSystemOn(bool systemOn);
    void onConnected();
    bool tvOff() const;

  private:
    void publishCurrent();
    void setCurrentWill();

    MqttTopics topics_;
    Publish publish_;
    SetWill setWill_;
    bool tvOff_ = false;
};

} // namespace daemon
} // namespace nativetv
