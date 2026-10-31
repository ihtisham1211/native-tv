#pragma once

#include <string>
#include <vector>

namespace nativetv {
namespace service {

struct AppKind {
    const char *key;
    const char *label;
};

// The kinds the Apps page can group by, in their default order; "other" is last and takes
// every app the table does not name.
const std::vector<AppKind> &appKinds();
bool validAppKind(const std::string &key);
std::string appKindLabel(const std::string &key); // "" for a key that is not a kind

// The kind of an installed app: by its id, then by its title keyed as the icon pack keys
// titles (tools/gen_app_icons.py), then LG's own id prefixes; "other" when nothing matches.
std::string appKind(const std::string &appId, const std::string &title);

} // namespace service
} // namespace nativetv
