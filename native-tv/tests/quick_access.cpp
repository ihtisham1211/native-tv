#include "service/quick_access.hpp"
#include <cstdlib>
#include <iostream>
#include <string>

using namespace nativetv;

namespace {
int failures = 0;

void check(bool condition, const std::string &message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

Json fixtureSettings() {
    Json settings = Json::object();
    for (unsigned key = 1; key <= 9; ++key) {
        Json value = Json::object();
        value.set("id", Json::text(key == 4 ? "youtube" : ""));
        value.set("params", Json::object());
        settings.set("magicNum" + std::to_string(key), value);
    }
    return settings;
}
} // namespace

int main() {
    const Json keys = service::quickAccessSettingKeys();
    check(keys.isArray() && keys.size() == 9 && keys[0].string() == "magicNum1" &&
              keys[8].string() == "magicNum9",
          "read all nine Quick Access settings");

    Json settings = fixtureSettings();
    check(service::quickAccessSettingsSupported(settings),
          "accept the supported {id, params} setting shape");
    Json malformed = settings;
    malformed.set("magicNum7", Json::text("netflix"));
    check(!service::quickAccessSettingsSupported(malformed),
          "leave differently shaped firmware settings untouched");
    Json missingId = settings;
    Json invalid = Json::object();
    invalid.set("params", Json::object());
    missingId.set("magicNum2", invalid);
    check(!service::quickAccessSettingsSupported(missingId),
          "require a string id for each key");

    Json devices = Json::array();
    Json hdmi = Json::object();
    hdmi.set("id", Json::text("HDMI_2"));
    devices.add(hdmi);
    Json apps = Json::array();
    Json netflix = Json::object();
    netflix.set("id", Json::text("netflix"));
    netflix.set("visible", Json::flag(true));
    apps.add(netflix);
    Json hidden = Json::object();
    hidden.set("id", Json::text("com.webos.app.self-diagnosis"));
    hidden.set("visible", Json::flag(false));
    apps.add(hidden);
    Json systemInput = Json::object();
    systemInput.set("id", Json::text("com.webos.app.hdmi3"));
    apps.add(systemInput);
    Json container = Json::object();
    container.set("id", Json::text("com.webos.app.container"));
    apps.add(container);
    Json liveTv = Json::object();
    liveTv.set("id", Json::text("com.webos.app.livetv"));
    apps.add(liveTv);

    check(service::quickAccessTargetAllowed("netflix", devices, apps),
          "allow an installed visible app");
    check(service::quickAccessTargetAllowed("com.webos.app.hdmi2", devices, apps),
          "allow an HDMI input reported by EIM");
    check(service::quickAccessTargetAllowed("com.webos.app.livetv", devices, apps),
          "allow Live TV");
    check(!service::quickAccessTargetAllowed("com.webos.app.self-diagnosis", devices, apps),
          "reject hidden apps");
    check(!service::quickAccessTargetAllowed("com.webos.app.hdmi3", devices, apps),
          "reject an HDMI app absent from the input service");
    check(!service::quickAccessTargetAllowed("com.webos.app.container", devices, apps),
          "reject the app container");
    check(!service::quickAccessTargetAllowed("missing.app", devices, apps),
          "reject apps that are not installed");

    const Json write = service::quickAccessWriteRequest(4, "netflix");
    check(write["category"].string() == "option" &&
              write["settings"]["magicNum4"]["id"].string() == "netflix" &&
              write["settings"]["magicNum4"]["params"].isObject(),
          "write the selected id using LG's settings schema");
    check(write["settings"].keys().size() == 1,
          "write only the selected key");

    if (failures) {
        std::cerr << failures << " Quick Access assertions failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "native Quick Access service tests passed\n";
    return EXIT_SUCCESS;
}
