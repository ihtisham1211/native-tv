#pragma once

#include "core/json.hpp"
#include <string>
#include <vector>

namespace nativetv {
namespace daemon {
struct ServerConfig {
    int port = 8080;
    std::string host = "::";
    std::string token;
    bool allowControl = true;
    bool allowPower = true;
    bool sideload = false;
    Json document = Json::object();
};

struct ConfigLoadOptions {
    ConfigLoadOptions();

    std::string primaryPath;
    std::string fallbackPath;
    std::string overridePath;
};

struct ConfigLoadResult {
    ServerConfig config;
    // File used for reads and writes. Defaults to the primary path when no
    // valid file exists, matching the legacy server's first-save behavior.
    std::string path;
    bool loaded = false;
    std::vector<std::string> warnings;
};

class ConfigLoader {
  public:
    static ConfigLoadResult load(const ConfigLoadOptions &options = ConfigLoadOptions());
};

// Writes the existing config document atomically while preserving unknown
// keys. Setup uses this for the TV-only network access switch.
class ConfigStore {
  public:
    explicit ConfigStore(std::string path);
    bool setNetworkOpen(bool open, std::string *error = nullptr) const;
    bool setMqttSetup(const Json &mqtt, const Json &device,
                      std::string *error = nullptr) const;
    bool writeDashboardSettings(const Json &patch,
                                std::string *error = nullptr) const;
    bool setNetworkDebuggerAllowed(bool allowed,
                                   std::string *error = nullptr) const;
    // Sets (or, with a null value, removes) top-level keys under the shared
    // config lock, keeping everything else in the document.
    bool setTopLevel(const Json &changes, std::string *error = nullptr) const;
    bool patchObjectSection(const std::string &section, const Json &patch,
                            Json *updatedSection = nullptr,
                            std::string *error = nullptr) const;
    bool patchBackupSettings(const Json &patch,
                             std::string *error = nullptr,
                             Json *changedFields = nullptr) const;

  private:
    std::string path_;
};
} // namespace daemon
} // namespace nativetv
