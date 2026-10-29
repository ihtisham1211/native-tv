#pragma once

#include "core/json.hpp"

namespace nativetv {
namespace daemon {

// Native copy of the Home Assistant category and entity metadata exposed by
// legacy lib/ha.js. Discovery payload construction consumes this same
// catalogue so the dashboard and MQTT publication share one native source.
Json haEntityCategories();
Json haEntityCatalogue();

struct HaEntityFilterResult {
    Json published;
    Json withheld;
};

// Apply the hardware, firmware and user-preference rules from ha.js while
// retaining the source order used for discovery and retained-topic cleanup.
HaEntityFilterResult filterHaEntityCatalogue(const Json &catalogue,
                                             const Json &capabilities);

} // namespace daemon
} // namespace nativetv
