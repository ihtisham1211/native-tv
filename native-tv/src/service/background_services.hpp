#pragma once

#include "core/json.hpp"
#include <map>
#include <string>
#include <vector>

namespace nativetv {
namespace service {

struct BackgroundServiceProbe {
    std::string id;
    std::string unit;
    std::string upstart;
};

struct BackgroundServiceInfo {
    std::string id;
    std::string bin;
    std::string exe;
    std::string unit;
    std::string upstart;
    std::string jail;
};

bool backgroundServicesSupported(const std::string &systemRoot);
std::string backgroundSystemctlExecutable(const std::string &systemRoot);
std::string backgroundInitctlExecutable(const std::string &systemRoot);
std::vector<BackgroundServiceProbe> backgroundServiceProbes(
    const std::string &systemRoot);
bool backgroundServiceInfo(const std::string &id, const std::string &systemRoot,
                           BackgroundServiceInfo &info);
bool backgroundHiddenServiceInfo(const std::string &id,
                                const std::string &systemRoot,
                                BackgroundServiceInfo &info);
bool backgroundHiddenServiceDefinition(const std::string &id,
                                      BackgroundServiceInfo &info);
// Probes a hidden catalog service for owner features without exposing it in
// the user-facing background-service catalogue or toggle route.
bool backgroundHiddenServiceAvailable(const std::string &id,
                                      const std::string &systemRoot);
std::vector<std::string> backgroundServicePrograms(
    const std::string &id, const std::string &systemRoot);
std::vector<int> backgroundServiceProcessIds(const std::string &id,
                                             const std::string &procRoot);
std::string backgroundServiceStubPath(const std::string &dataRoot, bool javascript);
bool backgroundServiceStubbed(const std::string &procRoot, const std::string &path);
bool readDisabledBackgroundServices(const std::string &dataRoot,
                                    std::vector<std::string> &disabled,
                                    std::string *error = nullptr);
bool setBackgroundServiceDisabled(const std::string &dataRoot, const std::string &id,
                                  bool disabled,
                                  std::vector<std::string> &disabledServices,
                                  std::string *error = nullptr);
bool syncBackgroundServiceBootScript(const std::string &systemRoot,
                                    const std::string &dataRoot,
                                    const std::vector<std::string> &disabledServices,
                                    std::string *error = nullptr);
Json backgroundServicesStatus(const std::string &systemRoot,
                              const std::string &procRoot,
                              const std::string &dataRoot,
                              const std::map<std::string, bool> &managerRunning);

} // namespace service
} // namespace nativetv
