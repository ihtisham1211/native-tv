#include "daemon/retired_files.hpp"
#include <ftw.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>

namespace nativetv {
namespace daemon {
namespace {

// A file is listed here when it leaves the repository.
const char *const kFiles[] = {
    "assets/dashboard.html", "assets/home.html", "assets/home-tv.html", "assets/setup.html",
    "assets/setup-phone.html", "assets/qr.js", "assets/i18n.js", "assets/i18n/es.json",
    "assets/fonts/Manrope.ttf", "assets/fonts/Manrope-Regular.ttf",
    "assets/fonts/Manrope-SemiBold.ttf", "assets/fonts/Outfit.ttf",
    "assets/fonts/Outfit-Light.ttf", "assets/fonts/Outfit-Regular.ttf",
    "assets/fonts/OFL-Manrope.txt", "assets/fonts/OFL-Outfit.txt",
    "assets/fonts/Figtree-Light.ttf", "assets/fonts/Figtree-Regular.ttf",
    "assets/fonts/Figtree-Medium.ttf", "assets/fonts/Figtree-SemiBold.ttf",
    "assets/fonts/LICENSE-Figtree-OFL.txt",
    "assets/google-tv/base.css", "assets/google-tv/tokens.css", "assets/google-tv/text.js",
    "assets/google-tv/kit.html", "assets/google-tv/components/button.css",
    "assets/google-tv/components/card.css", "assets/google-tv/components/chip.css",
    "assets/google-tv/components/controls.css", "assets/google-tv/components/list.css",
    "assets/google-tv/components/overlay.css", "assets/google-tv/components/progress.css",
    "assets/google-tv/components/tabs.css", "assets/google-tv/pages/dashboard.html",
    "assets/google-tv/pages/home.html", "assets/google-tv/pages/setup.html",
    "assets/google-tv/pages/setup-phone.html", "assets/google-tv/pages/kit/index.html",
    "assets/google-tv/pages/kit/kit.css", "assets/google-tv/pages/kit/kit.js",
    // The Home page and the keyboard it loaded: Native Tv Home is the native client's.
    "assets/google-tv/pages/home/index.html", "assets/google-tv/pages/home/home.css",
    "assets/google-tv/pages/home/home.js", "assets/google-tv/pages/home/quick.js",
    "assets/google-tv/pages/home/content.js", "assets/google-tv/shared/keyboard.js",
    "assets/google-tv/shared/key-delete.svg", "assets/google-tv/shared/key-done.svg",
    "assets/google-tv/shared/key-left.svg", "assets/google-tv/shared/key-right.svg",
    "assets/google-tv/shared/key-shift.svg", "assets/google-tv/shared/key-space.svg",
    "assets/ui/update.js", "lib/updater.js",
    // The dashboard tile was a web page before it became the native client.
    "assets/dashboard-app/index.html",
    // The Node server of the releases before the daemon was the whole server.
    "tvweb.js", "lib/adblock-domains.js", "lib/apps.js", "lib/backup.js", "lib/children.js",
    "lib/config_lock.js", "lib/controls.js", "lib/devtools.js", "lib/fetch.js",
    "lib/game.js", "lib/ha.js", "lib/history.js", "lib/home-content.js",
    "lib/home-discover.js", "lib/home-picture.js", "lib/home-recent-apps.js",
    "lib/home-recent.js", "lib/home-takeover.js", "lib/home-wallpapers.js", "lib/home.js",
    "lib/homescreen-webos10.js", "lib/homescreen-webos9.js", "lib/homescreen.js",
    "lib/iconserver.js", "lib/installer.js", "lib/lgsettings.js", "lib/logs.js",
    "lib/luna.js", "lib/mqtt-state.js", "lib/mqtt.js", "lib/names.js",
    "lib/native-frontdoor.js", "lib/native-metrics.js", "lib/notifications.js",
    "lib/ntp.js", "lib/oled.js", "lib/piccap.js", "lib/power.js", "lib/preload.js",
    "lib/privacy.js", "lib/prometheus.js", "lib/quickaccess.js", "lib/remotebattery.js",
    "lib/remotebuttons.js", "lib/remotebuttons.py", "lib/remotekeys.js", "lib/repo.js",
    "lib/retired.js", "lib/routes.js", "lib/samrescan.js", "lib/say.js",
    "lib/screensavers.js", "lib/screentime.js", "lib/services.js", "lib/sshkeys.js",
    "lib/state.js", "lib/syslog.js", "lib/telemetry.js", "lib/timers.js", "lib/topics.js",
    "lib/util.js", "lib/version.js"};
// Emptied by the removals above; removed only if nothing else is in them.
const char *const kFolders[] = {"assets/i18n", "assets/google-tv/components",
                                "assets/google-tv/pages/kit", "assets/google-tv/pages/home",
                                "lib"};

// The developer client, com.tvweb.native, which only deploy.py ever put on the
// TV and which sam was never told of. The Home stub opens the Native Tv tile's
// app (com.tvweb.dashboard) instead.
const char *const kApps[] = {"com.tvweb.native"};

int removeEntry(const char *path, const struct stat *, int, struct FTW *) {
    return remove(path);
}

} // namespace

size_t removeRetiredApps(const std::string &appsRoot) {
    size_t gone = 0;
    for (const char *id : kApps) {
        bool found = false;
        for (const char *folder : {"/applications/", "/packages/"}) {
            const std::string path = appsRoot + folder + id;
            struct stat info;
            if (lstat(path.c_str(), &info) != 0) continue;
            found = true;
            // Depth first, links not followed: a link in it goes, not what it points at.
            (void)nftw(path.c_str(), removeEntry, 16, FTW_DEPTH | FTW_PHYS);
        }
        if (found) ++gone;
    }
    return gone;
}

size_t removeRetiredFiles(const std::string &serverDirectory) {
    size_t gone = 0;
    for (const char *relative : kFiles)
        if (unlink((serverDirectory + "/" + relative).c_str()) == 0) ++gone;
    for (const char *relative : kFolders)
        (void)rmdir((serverDirectory + "/" + relative).c_str());
    return gone;
}

} // namespace daemon
} // namespace nativetv
