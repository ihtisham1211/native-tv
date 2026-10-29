#include "ui/application.hpp"
#include "ui/controller_helpers.hpp"
#include <algorithm>

namespace nativetv {
void Application::rebuildMenu() {
    auto &menu = state_.menu;
    if (state_.screen == Screen::Dashboard)
        menu.rows =
            dashboardRows(dashboardTabs()[menu.tab].key, state_.menuData, state_.caps, menu.sub);
    else if (state_.screen == Screen::Setup)
        menu.rows = setupRows(menu.level, state_.setup);
    else if (state_.screen == Screen::Catalog) {
        menu.rows.clear();
        Json install = state_.install;
        std::string status = install["state"].string();
        if (status == "awaiting-confirm") {
            Json preview = install["preview"];
            if (preview["vetted"].boolean())
                menu.rows.push_back(option("Run with root privileges", navigate("install-root"),
                                           state_.installRoot ? "On" : "Off",
                                           "Only offered for vetted catalog packages."));
            if (preview["storeInstalled"].boolean())
                menu.rows.push_back(option("Replace the store version", navigate("install-replace"),
                                           state_.installReplace ? "On" : "Off"));
            menu.rows.push_back(option(preview["title"].string(preview["id"].string()), Command(),
                                       preview["version"].string(),
                                       preview["description"].string()));
            menu.rows.push_back(
                option("Install",
                       post(Operation::ConfirmPackage,
                            fields({{"jobId", install["jobId"]},
                                    {"elevate",
                                     Json::flag(state_.installRoot && preview["vetted"].boolean())},
                                    {"replaceStore", Json::flag(state_.installReplace)}}),
                            true),
                       "", "Installs the package using the selected privileges."));
            menu.rows.push_back(option(
                "Cancel", post(Operation::CancelPackage, fields({{"jobId", install["jobId"]}}))));
        } else if (!status.empty() && status != "idle" && status != "installed" &&
                   status != "error" && status != "interrupted") {
            menu.rows.push_back(
                option("Installing application", Command(), status, install["message"].string()));
            menu.rows.push_back(option(
                "Cancel", post(Operation::CancelPackage, fields({{"jobId", install["jobId"]}}))));
        } else {
            if (status == "error" || status == "interrupted")
                menu.rows.push_back(
                    option("Installation failed", Command(), "", install["error"].string()));
            Json apps = state_.catalog["apps"];
            if (!apps.isArray())
                apps = state_.catalog["catalog"];
            for (const auto &app : apps.items())
                menu.rows.push_back(
                    option(app["title"].string(app["id"].string()),
                           post(Operation::FetchPackage,
                                fields({{"id", app["id"]}, {"auto", Json::flag(true)}})),
                           app["version"].string(), app["description"].string()));
            if (menu.rows.empty())
                menu.rows.push_back(option(state_.catalog.valid() ? "No applications available"
                                                                  : "Loading catalog…",
                                           navigate("refresh")));
        }
    }
    menu.selected = clampIndex(menu.selected, menu.rows.size());
    while (menu.selected + 1 < menu.rows.size() && menu.rows[menu.selected].heading)
        ++menu.selected;
    for (auto &row : menu.rows)
        if (row.command.route == "theme")
            row.value = state_.light ? "Light" : "Dark";
    dirty_ = true;
}
} // namespace nativetv
