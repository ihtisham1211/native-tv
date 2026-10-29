#pragma once

#include "core/json.hpp"
#include <functional>
#include <string>

namespace nativetv {
namespace daemon {

// What a restore needs from the daemon. The sequence and the answer are
// BackupRestore's own; everything that touches the TV or the disk comes
// through here, so the flow runs against scripted services in tests.
struct BackupRestoreDeps {
    // Writes the backup's LG settings: {applied, unchanged, skipped, failed}.
    // False, with the reason, when the service could not run.
    std::function<bool(const Json &wanted, Json &result, std::string &error)> restoreLg;
    // The apps the backup lists that this TV lacks: {toInstall, manual,
    // present, catalogError}.
    std::function<bool(const Json &apps, Json &plan, std::string &error)> planApps;
    std::function<bool(const Json &toInstall, long long savedMs, std::string &error)> savePendingApps;
    std::function<bool(std::string &error)> clearPendingApps;
    // The Home layout, answered as the dashboard's layout route answers.
    std::function<Json(const Json &layout)> restoreHome;
    // Native Tv's own settings; changed gets the keys written ("section.key").
    std::function<bool(const Json &section, Json &changed, std::string &error)> writeConfig;
    std::function<void(const std::string &warning)> warn;
    std::function<long long()> nowMs;
};

struct BackupRestoreOutcome {
    int status = 200;
    Json body;
    // Native Tv's own settings changed: they take effect when it restarts.
    bool restart = false;
};

// POST /api/backup once the request is accepted: the legacy restore
// (legacy lib/backup.js) in the same order and with the same answer.
BackupRestoreOutcome restoreBackup(const BackupRestoreDeps &deps, const Json &backup);

} // namespace daemon
} // namespace nativetv
