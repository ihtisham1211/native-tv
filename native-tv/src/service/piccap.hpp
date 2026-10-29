#pragma once

#include "core/json.hpp"
#include <functional>
#include <mutex>
#include <string>

namespace nativetv {
namespace service {

// PicCap is queried only when its Homebrew app is installed. webOS 2 can reset
// some older TVs when its service starts, so known versions below 3 are gated.
bool piccapInstalled(const std::string &systemRoot = "/");
bool piccapSupportedWebos(const std::string &systemRoot = "/");

// The dashboard's PicCap Capture row (piccap.js status): {running} where the app
// is installed and its service answers, an invalid Json (null) otherwise,
// without asking when it is not installed. The answer is kept for 4 s, and a
// service that neither answers nor is missing keeps what it last was.
class PiccapStatus {
  public:
    // ask puts the status reply in reply and its raw text in raw; false on no answer.
    using Ask = std::function<bool(Json &reply, std::string &raw)>;
    PiccapStatus(std::string systemRoot, Ask ask, std::function<long long()> nowMs);

    Json read();

  private:
    std::string systemRoot_;
    Ask ask_;
    std::function<long long()> nowMs_;
    std::mutex mutex_;
    long long checkedAt_ = 0;
    bool available_ = false;
    bool running_ = false;
};

// piccap.js migrateConfig: the old piccap.enabled becomes the entity setting it
// duplicated. False is kept as PicCap unticked in mqtt.entities.disabled, true
// meant nothing, and the key goes either way (the section with it when that
// leaves it empty). The top-level changes to save, or an invalid Json when the
// config has no piccap.enabled. A null change removes its key.
Json piccapConfigMigration(const Json &config);

// Whether a failed status read says PicCap's service is not on this TV.
bool piccapServiceMissing(const Json &reply, const std::string &raw);

} // namespace service
} // namespace nativetv
