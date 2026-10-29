#include "daemon/app_elevation_store.hpp"
#include "daemon/json_file.hpp"

namespace nativetv {
namespace daemon {
namespace {
const size_t MAX_APP_ID_BYTES = 128;
const size_t MAX_SERVICES = 256;

bool validName(const std::string &value) {
    if (value.empty() || value.size() > MAX_APP_ID_BYTES ||
        !((value[0] >= 'a' && value[0] <= 'z') ||
          (value[0] >= 'A' && value[0] <= 'Z') ||
          (value[0] >= '0' && value[0] <= '9')))
        return false;
    for (unsigned char character : value)
        if (!((character >= 'a' && character <= 'z') ||
              (character >= 'A' && character <= 'Z') ||
              (character >= '0' && character <= '9') || character == '.' ||
              character == '_' || character == '+' || character == '-'))
            return false;
    return true;
}

bool validServices(const Json &services, std::string *error) {
    if (!services.isArray() || services.size() > MAX_SERVICES) {
        if (error) *error = "elevated services must be an array of at most 256 names";
        return false;
    }
    for (const Json &service : services.items()) {
        if (!service.isString() || !validName(service.string())) {
            if (error) *error = "elevated service name is invalid";
            return false;
        }
    }
    return true;
}

Json loadMap(const std::string &path, std::string *error) {
    const JsonFileResult loaded = JsonFile(path, 128 * 1024).load();
    if (!loaded.error.empty()) {
        if (error) *error = loaded.error;
        return Json();
    }
    if (!loaded.found) return Json::object();
    if (!loaded.value.isObject()) {
        if (error) *error = "elevated service state must be an object";
        return Json();
    }
    return loaded.value;
}
} // namespace

bool readElevatedServices(const std::string &path, const std::string &appId,
                          Json &services, std::string *error) {
    if (!validName(appId)) {
        if (error) *error = "app id is invalid";
        return false;
    }
    Json map = loadMap(path, error);
    if (!map.valid()) return false;
    const Json stored = map[appId];
    services = Json::array();
    if (!stored.isArray()) return true;
    for (const Json &service : stored.items()) {
        if (service.isString() && validName(service.string()))
            services.add(service);
    }
    return true;
}

bool recordElevatedServices(const std::string &path, const std::string &appId,
                            const Json &services, std::string *error) {
    if (!validName(appId)) {
        if (error) *error = "app id is invalid";
        return false;
    }
    if (!validServices(services, error)) return false;
    Json map = loadMap(path, error);
    if (!map.valid()) return false;
    map.set(appId, services);
    return JsonFile(path, 128 * 1024).save(map, error);
}

} // namespace daemon
} // namespace nativetv
