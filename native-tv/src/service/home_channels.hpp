#pragma once

#include "core/json.hpp"

namespace nativetv {
namespace service {

struct HomeChannels {
    Json channels;
    bool tunerAvailable = false;
};

// Projects the webOS API adapter's channel list into the Home API shape.
HomeChannels homeChannels(const Json &reply);

} // namespace service
} // namespace nativetv
