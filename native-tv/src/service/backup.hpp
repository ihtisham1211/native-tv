#pragma once

#include "core/json.hpp"
#include <string>

namespace nativetv {
namespace service {

// Builds the version 1 settings export while filtering private and unsupported
// config fields. Inputs are the rows and developer apps collected by the daemon.
Json createSettingsBackup(const std::string &created, const Json &model,
                          const Json &version, const Json &config,
                          const Json &lgRows, const Json &home,
                          const Json &apps);
// Returns "notBackup", "newer", or "valid" for the legacy restore envelope.
std::string backupRestoreEnvelopeResult(const Json &backup);
Json safeBackupConfig(const Json &config);

// Today's daily backup name from an ISO timestamp, or empty while daily backups
// are off or the clock is unset (before 2026, as Node's CLOCK_FLOOR_MS).
std::string dailyBackupNameDue(bool enabled, const std::string &isoTimestamp);
bool validSavedBackupName(const std::string &name);
bool savedBackupExists(const std::string &directory, const std::string &name,
                       bool &exists, std::string *error = nullptr);
bool readSavedBackup(const std::string &directory, const std::string &name,
                     std::string &contents);
bool listSavedBackups(const std::string &directory, Json &backups,
                     std::string *error = nullptr);
bool saveDailyBackup(const std::string &directory, const std::string &name,
                     const Json &backup, std::string *error = nullptr);
bool backupAppAllowed(const std::string &id);
Json planBackupRestoreApps(const Json &apps, const Json &installedIds,
                           const Json &catalogIds, bool catalogAvailable,
                           const std::string &catalogError);
Json readPendingBackupApps(const std::string &path);
Json pendingBackupApps(const Json &pending, const Json &installedIds);
bool savePendingBackupApps(const std::string &path, const Json &saved,
                           const Json &apps, std::string *error = nullptr);
bool clearPendingBackupApps(const std::string &path,
                            std::string *error = nullptr);

} // namespace service
} // namespace nativetv
