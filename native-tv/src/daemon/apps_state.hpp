#pragma once

#include "core/json.hpp"
#include <string>

namespace nativetv {
namespace daemon {

// Persist the legacy newline-separated hidden_apps list and tile-hiding flag.
bool persistAppTileState(const std::string &hiddenAppsPath,
                        const std::string &tileHidingFlagPath,
                        const Json &state, std::string *error = nullptr);

// An upgrade that finds tile hiding running keeps it (legacy lib/apps.js
// migrateConfig): allowTileHiding is saved true, so turning it off leaves it
// possible to turn on again. Where the stock screen saver is not QML (webOS 10
// and later) #381 held tile hiding back, so it carries over only with the
// experimental screen savers allowed.
bool tileHidingCarriesOver(const Json &config, bool flagEnabled, const std::string &stockType);

// Reads the flag and the stock screen saver type under dataRoot and saves
// allowTileHiding: true into the config file when the above says so. True when
// it saved.
bool migrateTileHidingConfig(const std::string &configPath, const Json &config,
                             const std::string &dataRoot, std::string *error = nullptr);

// Node's other start-up migration (tvweb.js migrateConfigFile; the PicCap one is
// service::piccapConfigMigration): `allowOnWebos10` became
// `allowExperimentalScreensavers`. True when the file changed.
bool migrateLegacyConfig(const std::string &configPath, const Json &config,
                         std::string *error = nullptr);

// Whether an install or uninstall must have sam read the apps again for the home
// ribbon to show it (legacy lib/apps.js refreshLauncher). Once sam has been
// restarted the ribbon stops taking in new launch points; the boot hook restarts
// it when tile hiding is on with apps hidden, except on a Homebrew Channel
// install. Never where tile hiding is not allowed (allowTileHiding in config,
// else whether it is on). restartedThisRun: the daemon restarted sam since start.
bool launcherNeedsRefresh(const std::string &dataRoot, const Json &config, bool restartedThisRun);

} // namespace daemon
} // namespace nativetv
