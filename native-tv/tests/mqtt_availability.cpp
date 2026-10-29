#include "daemon/mqtt_availability.hpp"
#include <cassert>
#include <string>
#include <tuple>
#include <vector>

int main() {
    using namespace nativetv::daemon;
    std::vector<std::tuple<std::string, std::string, bool>> published;
    std::vector<std::string> wills;
    MqttAvailabilityPublisher availability(
        MqttTopics("fixture"),
        [&published](const std::string &topic, const std::string &payload,
                     bool retained) {
            published.emplace_back(topic, payload, retained);
            return true;
        },
        [&wills](const std::string &payload) {
            wills.push_back(payload);
            return true;
        });

    availability.onConnected();
    assert(published.size() == 1);
    assert(std::get<0>(published.back()) == "fixture/status");
    assert(std::get<1>(published.back()) == "online");
    assert(std::get<2>(published.back()));
    assert(wills.size() == 1 && wills.back() == "offline");
    assert(!availability.tvOff());

    availability.setSystemOn(false);
    assert(published.size() == 2);
    assert(std::get<1>(published.back()) == "off");
    assert(std::get<2>(published.back()));
    assert(wills.size() == 2 && wills.back() == "asleep");
    assert(availability.tvOff());

    availability.onConnected();
    assert(published.size() == 3 && std::get<1>(published.back()) == "off");
    assert(wills.size() == 3 && wills.back() == "asleep");
    availability.setSystemOn(true);
    assert(published.size() == 4 && std::get<1>(published.back()) == "online");
    assert(wills.size() == 4 && wills.back() == "offline");
    assert(!availability.tvOff());

    return 0;
}
