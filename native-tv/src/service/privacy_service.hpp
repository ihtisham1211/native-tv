#pragma once

#include <string>
#include <vector>

namespace nativetv {
namespace service {

// True only when the named LS2 service is declared on-demand and its program
// is not currently the first argument of any process under procRoot.
bool privacyServiceWouldStart(const std::string &systemRoot,
                              const std::string &procRoot,
                              const std::string &serviceName);

// The first argument of each process, matching `ps -eo args`'s command word.
std::vector<std::string> privacyProcessFirstArguments(const std::string &procRoot);

} // namespace service
} // namespace nativetv
