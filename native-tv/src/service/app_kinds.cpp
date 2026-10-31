#include "service/app_kinds.hpp"
#include "service/app_icons.hpp"
#include <cstring>

namespace nativetv {
namespace service {
namespace {
const std::vector<AppKind> kKinds = {{"video", "Movies & TV"}, {"music", "Music & audio"},
                                     {"games", "Games"},       {"home", "Smart home"},
                                     {"tv", "LG & TV"},        {"tools", "Tools"},
                                     {"other", "Other"}};

struct Entry {
    const char *key; // an app id, or a title as titleKey() writes it
    const char *kind;
};
// The streaming brands as design/app-icons.json names them, LG's own apps, and the Homebrew
// Channel's apps by their ids.
const Entry kTable[] = {
    // Movies & TV
    {"netflix", "video"}, {"youtube.leanback.v4", "video"}, {"youtube", "video"},
    {"youtubetv", "video"}, {"youtubekids", "video"}, {"amazon", "video"},
    {"primevideo", "video"}, {"amazonprimevideo", "video"}, {"disneyplus", "video"},
    {"hulu", "video"}, {"com.wbd.stream", "video"}, {"max", "video"}, {"hbomax", "video"},
    {"com.peacock.tv", "video"}, {"peacock", "video"}, {"paramountplus", "video"},
    {"appletv", "video"}, {"appletvplus", "video"}, {"twitch", "video"},
    {"crunchyroll", "video"}, {"plex", "video"}, {"jellyfin", "video"}, {"emby", "video"},
    {"kodi", "video"}, {"stremio", "video"}, {"tiktok", "video"},
    {"com.webos.app.lgchannels", "video"}, {"com.webos.app.livetv", "video"},
    {"com.webos.app.mediadiscovery", "video"}, {"com.webos.app.lifeonscreen", "video"},
    {"com.webos.app.newandhot", "video"}, {"com.animemania.app", "video"},
    {"org.jellyfin.webos", "video"}, {"org.xbmc.kodi", "video"}, {"com.breezyfin.app", "video"},
    {"org.moonfin.webos", "video"}, {"org.litefin.app", "video"}, {"com.ocenfin.app", "video"},
    {"com.beb.plxnative", "video"}, {"com.crunchyroll.webos", "video"},
    {"com.crunchyroll.stream.app", "video"}, {"com.biliwebos.app", "video"},
    {"com.cheerchen.decotv", "video"}, {"org.webosbrew.cccbib", "video"},
    // Music & audio
    {"youtubemusic", "music"}, {"applemusic", "music"}, {"spotify", "music"},
    {"tidal", "music"}, {"deezer", "music"}, {"soundcloud", "music"},
    {"com.webos.app.totalmusic", "music"},
    // Games
    {"steam", "games"}, {"xbox", "games"}, {"geforcenow", "games"}, {"amazonluna", "games"},
    {"com.webos.app.gameoptimizer", "games"}, {"com.limelight.webos", "games"},
    {"org.mariotaku.ihsplay", "games"}, {"com.halfbrick.fruitninja", "games"},
    {"org.chocolate-doom.demo", "games"}, {"com.gabrielecirulli.app2048.webos", "games"},
    {"com.github.k4zmu2a.space-cadet-pinball", "games"}, {"com.leondrolio.x.openttd", "games"},
    // Smart home
    {"homeassistant", "home"}, {"googlehome", "home"}, {"amazon.alexa.view", "home"},
    {"amazonalexa", "home"}, {"alexa", "home"}, {"com.webos.app.homeconnect", "home"},
    {"nextcloud", "home"}, {"immich", "home"}, {"com.immich.webos", "home"},
    {"com.seeky91.immichtv", "home"},
    // Tools
    {"com.webos.app.browser", "tools"}, {"org.webosbrew.hbchannel", "tools"},
    {"com.tvweb.dashboard", "tools"}, {"facebook", "tools"}, {"instagram", "tools"},
    {"reddit", "tools"}, {"x", "tools"}, {"twitter", "tools"},
    {"org.webosbrew.custom-screensaver", "tools"}, {"org.webosbrew.piccap", "tools"},
    {"org.webosbrew.hyperhdr.loader", "tools"}, {"org.webosbrew.hyperion.ng.loader", "tools"},
    {"org.webosbrew.inputhook", "tools"}, {"ooo.lew.lifesgoodwithoutspying", "tools"},
    {"org.webosbrew.safeupdate", "tools"}, {"com.open.hidden.menu", "tools"},
    {"io.github.beatcracker.decoderbench", "tools"}, {"com.amazoff.patcher", "tools"},
};

const char *lookup(const std::string &key) {
    if (key.empty())
        return nullptr;
    for (const Entry &entry : kTable)
        if (key == entry.key)
            return entry.kind;
    return nullptr;
}
bool startsWith(const std::string &value, const char *prefix) {
    return value.compare(0, std::strlen(prefix), prefix) == 0;
}
} // namespace

const std::vector<AppKind> &appKinds() {
    return kKinds;
}
bool validAppKind(const std::string &key) {
    for (const AppKind &kind : kKinds)
        if (key == kind.key)
            return true;
    return false;
}
std::string appKindLabel(const std::string &key) {
    for (const AppKind &kind : kKinds)
        if (key == kind.key)
            return kind.label;
    return "";
}
std::string appKind(const std::string &appId, const std::string &title) {
    if (const char *kind = lookup(appId))
        return kind;
    if (const char *kind = lookup(titleKey(title)))
        return kind;
    // What LG ships that the table does not name: settings, guides, the inputs.
    if (startsWith(appId, "com.webos.") || startsWith(appId, "com.palm."))
        return "tv";
    return "other";
}

} // namespace service
} // namespace nativetv
