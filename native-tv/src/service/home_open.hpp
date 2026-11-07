#pragma once

#include "core/json.hpp"
#include <cstdint>
#include <string>

namespace nativetv {
namespace service {

// What a Home open request does. Only what the catalogue lists can be opened,
// so this is not a way to launch anything at all.
struct HomeOpenPlan {
    bool handled = false; // false: Node still owns the kind; forward the request.
    bool ok = false;      // handled && ok: carry out the action; else report error.
    std::string error;    // localized, for handled && !ok
    std::string action;   // "launch" or "channel"
    std::string appId;    // launch target for app and device tiles
    Json launchParams;    // optional application launch parameters
    std::string channelId; // openChannel target for channel tiles
    std::string row;      // recent-history row stamped when it opens
    std::string rowId;    // recent-history id stamped when it opens
};

// LG's Home escape stays on Node until the lifecycle slice.
bool homeOpenHandledKind(const std::string &kind, const std::string &id);

// Decide what an open request does from the catalogue sources. An app tile
// launches its launch point, a device opens its HDMI input, a channel tunes,
// and YouTube videos and Netflix titles create app-specific launch parameters.
// Those two are the only deep links; a title is never opened in another app.
HomeOpenPlan planHomeOpen(const std::string &kind, const std::string &id,
                          const Json &apps, const Json &devices,
                          const Json &channels);

// Complete the external Wikidata lookup before launching Netflix. The native
// HomeContent service returns the same P1874 link data as the public endpoint.
void resolveHomeNetflixOpen(HomeOpenPlan &plan, const Json &lookup);

} // namespace service
} // namespace nativetv
