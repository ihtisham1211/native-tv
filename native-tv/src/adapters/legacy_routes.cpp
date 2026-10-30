#include "adapters/legacy_routes.hpp"
#include <stdexcept>

namespace nativetv {
std::string legacyAssetRoute(const std::string &asset) {
    if (asset.compare(0, 4, "app:") == 0)
        return "/api/home/icon?id=" + encode(asset.substr(4));
    if (asset.compare(0, 8, "youtube:") == 0)
        return "/api/home/image?u=" +
               encode("https://i.ytimg.com/vi/" + asset.substr(8) + "/hqdefault.jpg");
    if (asset.compare(0, 5, "tmdb:") == 0) {
        size_t split = asset.find(':', 5);
        if (split == std::string::npos)
            throw std::invalid_argument("Invalid artwork reference");
        std::string size = asset.substr(5, split - 5);
        return "/api/home/image?u=" +
               encode("https://image.tmdb.org/t/p/" + size + asset.substr(split + 1)) +
               (size == "w1280" ? "&priority=hero" : "");
    }
    if (asset.compare(0, 7, "recent:") == 0) {
        size_t split = asset.find(':', 7);
        if (split == std::string::npos)
            return "/api/home/recents/image?id=" + encode(asset.substr(7));
        return "/api/home/recents/image?id=" + encode(asset.substr(split + 1)) +
               "&v=" + encode(asset.substr(7, split - 7));
    }
    if (asset.compare(0, 10, "wallpaper:") == 0) {
        size_t revision = asset.find('@', 10);
        std::string slot =
            asset.substr(10, revision == std::string::npos ? std::string::npos : revision - 10);
        std::string route = "/api/home/wallpaper?slot=" + encode(slot);
        if (revision != std::string::npos)
            route += "&v=" + encode(asset.substr(revision + 1));
        return route;
    }
    throw std::invalid_argument("Unknown artwork reference");
}
std::string legacyRoute(Operation operation, const Json &arguments) {
    switch (operation) {
    case Operation::Capabilities:
        return "/api/caps";
    case Operation::Metrics:
        return "/api/stats";
    case Operation::SystemSettings:
        return "/api/stats?with=settings";
    case Operation::Apps:
        return "/api/apps";
    case Operation::OledCare:
        return "/api/oledcare";
    case Operation::Game:
        return "/api/game";
    case Operation::ScreenSaver:
        return "/api/screensaver";
    case Operation::HomeStatus:
        return "/api/home?with=catalogue,channels";
    case Operation::HomeCatalogue:
        return "/api/home/catalogue?view=1";
    case Operation::HomeDiscover:
        return "/api/home/discover";
    case Operation::HomeGuide:
        return "/api/home/guide";
    case Operation::HomeNow:
        return "/api/home/now";
    case Operation::HomeBeat:
        return "/api/home/beat";
    case Operation::HomePicture: {
        std::string route = "/api/home/picture?kind=" + encode(arguments["kind"].string());
        if (arguments["kind"].string() == "input")
            route += "&port=" + encode(arguments["port"].string());
        if (arguments["kind"].string() == "app")
            route += "&app=" + encode(arguments["app"].string());
        return route;
    }
    case Operation::Privacy:
        return "/api/privacy";
    case Operation::ServiceMenu:
        return "/api/servicemenu";
    case Operation::Setup:
        if (arguments["action"].string() == "network")
            return "/api/setup/network";
        if (arguments["action"].string() == "done")
            return "/api/setup/done";
        if (arguments["action"].string() == "alwaysReady")
            return "/api/setup/always-ready";
        if (arguments["action"].string() == "handoff")
            return "/api/setup/handoff";
        if (arguments["action"].string() == "handoffStop")
            return "/api/setup/handoff-stop";
        return "/api/setup";
    case Operation::AppCatalog:
        return "/api/apps/catalog";
    case Operation::InstallStatus:
        return "/api/apps/install/status";
    case Operation::TitleDetails:
        return "/api/home/details?type=" + encode(arguments["type"].string()) +
               "&id=" + encode(arguments["id"].string());
    case Operation::SearchTitles:
        return "/api/home/search?q=" + encode(arguments["query"].string());
    case Operation::NetflixTitle:
        return "/api/home/netflix?type=" + encode(arguments["type"].string()) +
               "&id=" + encode(arguments["id"].string());
    case Operation::Recents:
        return "/api/home/recents";
    case Operation::Control:
        return "/api/control";
    case Operation::OpenHomeItem:
        return "/api/home/open";
    case Operation::SetProfile:
        return "/api/home/profile";
    case Operation::SetHomeLayout:
        return "/api/home/layout";
    case Operation::EnableHome:
        return "/api/home/enable";
    case Operation::DisableHome:
        return "/api/home/disable";
    case Operation::SetHomeStartLayout:
        return "/api/home/start-layout";
    case Operation::SetHomeStartup:
        return "/api/home/startup";
    case Operation::UninstallApp:
        return "/api/apps/uninstall";
    case Operation::SetAppHidden:
        return "/api/apps/hide";
    case Operation::SetAppVisible:
        return "/api/apps/unhide";
    case Operation::SetTileHiding:
        return "/api/apps/tile-hiding";
    case Operation::SetLgHomeLayout:
        return "/api/homescreen";
    case Operation::SetPreload:
        return "/api/preload/toggle";
    case Operation::SetBackgroundService:
        return "/api/services/toggle";
    case Operation::FetchPackage:
        return "/api/apps/install/fetch";
    case Operation::ConfirmPackage:
        return "/api/apps/install/confirm";
    case Operation::CancelPackage:
        return "/api/apps/install/cancel";
    case Operation::Settings:
        return "/api/settings";
    case Operation::AdminUnlock:
        return "/api/admin/unlock";
    case Operation::None:
        break;
    }
    throw std::invalid_argument("No backend operation selected");
}
} // namespace nativetv
