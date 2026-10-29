#pragma once

#include "core/json.hpp"
#include <string>

namespace nativetv {
namespace service {

// Builds the setting-key to agreement-id mapping from the TV's
// eulaInfoNetwork.json file.
Json privacyConsentGroups(const Json &licenseMapping);

// Builds the legacy dashboard's consent projection from the TV's raw consent
// flags, firmware agreement mapping and current agreement documents.
Json privacyConsentReadProjection(const Json &flags, const Json &licenseMapping,
                                  const Json &documents);

// Builds the short privacy summary shown above the detailed consent rows.
Json privacySimpleSummary(const Json &projection);

struct PrivacyConsentPlan {
    bool valid = false;
    std::string error;
    bool changed = false;
    unsigned alsoChanged = 0;
    Json flags;
    Json documents;
};

bool privacyConsentSettable(const std::string &key, const Json &groups);
PrivacyConsentPlan planPrivacyConsent(const std::string &key, bool enabled,
                                     const Json &flags, const Json &documents,
                                     const Json &groups);

} // namespace service
} // namespace nativetv
