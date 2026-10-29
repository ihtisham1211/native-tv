#pragma once

#include <cstddef>
#include <string>

namespace nativetv {
namespace service {

struct SavedPageTarget {
    std::string url;
    std::string host;
};

// Matches the legacy saved-page URL allow-list and its default https scheme.
bool savedPageTarget(const std::string &address, SavedPageTarget &target);
bool savedPageAddressEmpty(const std::string &address);
// Collapses JavaScript whitespace and trims the title, preserving the 60 UTF-16
// code-unit limit used by the phone dashboard.
std::string normalizeSavedPageTitle(const std::string &title);
size_t savedPageTitleLength(const std::string &title);
bool savedPageBookmark(const std::string &appId, const std::string &launchPointType,
                       const std::string &browserId);

} // namespace service
} // namespace nativetv
