#include "daemon/last_app.hpp"
#include "daemon/json_file.hpp"

namespace nativetv {
namespace daemon {
namespace {
const size_t MAX_LAST_APP_FIELD_BYTES = 512;

bool validField(const Json &snapshot, const std::string &field, std::string *error) {
    if (!snapshot.has(field)) {
        if (error)
            *error = "last app snapshot is missing " + field;
        return false;
    }
    const Json value = snapshot[field];
    if (!value.valid()) // JSON null is represented by an invalid Json value.
        return true;
    if (!value.isString()) {
        if (error)
            *error = "last app " + field + " must be a string or null";
        return false;
    }
    const std::string text = value.string();
    if (text.empty() || text.size() > MAX_LAST_APP_FIELD_BYTES) {
        if (error)
            *error = "last app " + field + " is empty or too long";
        return false;
    }
    return true;
}
} // namespace

bool validateLastAppSnapshot(const Json &snapshot, std::string *error) {
    if (!snapshot.isObject()) {
        if (error)
            *error = "last app snapshot must be an object";
        return false;
    }
    if (!validField(snapshot, "app", error) ||
        !validField(snapshot, "app_id", error))
        return false;
    const Json app = snapshot["app"];
    const Json appId = snapshot["app_id"];
    if (!app.valid() && !appId.valid()) {
        if (error)
            *error = "last app snapshot must contain an app or app id";
        return false;
    }

    return true;
}

bool recordLastApp(const std::string &path, const Json &snapshot,
                   std::string *error) {
    if (!validateLastAppSnapshot(snapshot, error))
        return false;
    Json normalized = Json::object();
    const Json app = snapshot["app"];
    const Json appId = snapshot["app_id"];
    normalized.set("app", app);
    normalized.set("app_id", appId);
    return JsonFile(path, 2048).save(normalized, error);
}

} // namespace daemon
} // namespace nativetv
