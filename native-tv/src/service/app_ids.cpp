#include "service/app_ids.hpp"
#include "domain/names.hpp"

namespace nativetv {
namespace service {

bool appIdIsValid(const std::string &appId) {
    if (appId.empty()) return false;
    for (unsigned char character : appId)
        if (!((character >= 'a' && character <= 'z') ||
              (character >= 'A' && character <= 'Z') ||
              (character >= '0' && character <= '9') || character == '_' ||
              character == '-' || character == '.'))
            return false;
    return true;
}

bool appPackageIdIsSafe(const std::string &appId) {
    if (appId.empty() || appId.size() > 128) return false;
    const unsigned char first = static_cast<unsigned char>(appId[0]);
    if (!((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') ||
          (first >= '0' && first <= '9')))
        return false;
    for (unsigned char character : appId)
        if (!((character >= 'a' && character <= 'z') ||
              (character >= 'A' && character <= 'Z') ||
              (character >= '0' && character <= '9') || character == '.' ||
              character == '_' || character == '+' || character == '-'))
            return false;
    return true;
}

bool appIdIsProtected(const std::string &appId) {
    // No id is the safe side of the question, as in the legacy server.
    if (appId.empty()) return true;
    static const char *protectedIds[] = {
        "com.tvweb.dashboard", "com.tvweb.home", "com.tvweb.native",
        "com.tvweb.splash",
        "org.webosbrew.hbchannel", "com.webos.app.livetv",
        "com.palm.app.settings", "com.webos.app.settings",
        "com.webos.app.container", "container", "com.webos.app.inputcommon",
        "inputcommon", "com.webos.app.home", "com.webos.app.firstuse",
        "firstuse", "com.webos.app.eula", "eula",
        "com.webos.app.webapphost", "webapphost", "airplay",
        "com.webos.app.browser"
    };
    for (const char *id : protectedIds)
        if (appId == id) return true;
    return names::isHdmiKey(names::shortAppId(appId)) ||
           appId.compare(0, 19, "com.webos.app.input") == 0;
}

} // namespace service
} // namespace nativetv
