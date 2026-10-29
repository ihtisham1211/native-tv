#pragma once

#include "core/json.hpp"
#include <cstdint>

namespace nativetv {
namespace service {

// Returns an on-air programme in the Home API shape, or an invalid Json when
// the channel has no current programme.
Json homeCurrentProgramme(const Json &reply, int64_t nowMs);

} // namespace service
} // namespace nativetv
