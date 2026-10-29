#include "daemon/mqtt_availability.hpp"
#include <stdexcept>
#include <utility>

namespace nativetv {
namespace daemon {

MqttAvailabilityPublisher::MqttAvailabilityPublisher(MqttTopics topics,
                                                     Publish publish,
                                                     SetWill setWill)
    : topics_(std::move(topics)), publish_(std::move(publish)),
      setWill_(std::move(setWill)) {
    if (!publish_ || !setWill_)
        throw std::invalid_argument("MQTT availability needs publish and will handlers");
}

void MqttAvailabilityPublisher::setSystemOn(bool systemOn) {
    const bool nextTvOff = !systemOn;
    if (tvOff_ == nextTvOff)
        return;
    tvOff_ = nextTvOff;
    publishCurrent();
    setCurrentWill();
}

void MqttAvailabilityPublisher::onConnected() {
    publishCurrent();
    setCurrentWill();
}

bool MqttAvailabilityPublisher::tvOff() const { return tvOff_; }

void MqttAvailabilityPublisher::publishCurrent() {
    (void)publish_(topics_.status(), tvOff_ ? "off" : "online", true);
}

void MqttAvailabilityPublisher::setCurrentWill() {
    (void)setWill_(tvOff_ ? "asleep" : "offline");
}

} // namespace daemon
} // namespace nativetv
