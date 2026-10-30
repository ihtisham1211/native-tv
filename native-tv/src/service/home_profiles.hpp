#pragma once

#include "core/json.hpp"
#include <string>

namespace nativetv {
namespace service {

struct HomeDocumentUpdate {
    Json document;
    Json response;
    bool changed = false;
};

// Pure home.json projection and mutation rules. File ownership and atomic
// persistence stay in the daemon layer.
Json normalizeHomeDocument(const Json &raw);
Json homeLayout(const Json &document);
// `mainName`: what the main profile is called until the owner renames it (the TV's own name).
Json homeProfileList(const Json &document, const std::string &mainName = "Main");
std::string normalizeHomeProfileName(const Json &value);
HomeDocumentUpdate updateHomeLayout(const Json &document, const Json &body);
HomeDocumentUpdate updateHomeProfile(const Json &document, const Json &body,
                                     const std::string &mainName = "Main");
HomeDocumentUpdate updateHomeBooleanSetting(const Json &document,
                                            const std::string &key,
                                            const Json &body);

} // namespace service
} // namespace nativetv
