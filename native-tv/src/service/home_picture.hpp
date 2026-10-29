#pragma once

#include "core/json.hpp"
#include <string>

namespace nativetv {
namespace service {

std::string homePictureRange(const std::string &hdr);
Json homePictureResponse(const std::string &input, const std::string &range,
                         const Json &signal, const std::string &mode,
                         const Json &light, bool oled);
Json homePictureHdmiSignal(const std::string &status, const std::string &packet,
                           bool packetAvailable, const Json &facts);
Json homePictureAppVideo(const Json &reply);

} // namespace service
} // namespace nativetv
