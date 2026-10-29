#include "service/backup.hpp"
#include "service/lg_settings.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace nativetv;

namespace {
void require(bool condition, const std::string &message) {
    if (condition) return;
    std::fprintf(stderr, "%s\n", message.c_str());
    std::exit(1);
}
}

int main() {
    require(!service::backupAppAllowed("com.tvweb.splash"),
            "the native startup splash must stay out of user backup imports");

    const Json config = Json::parse(
        "{\"host\":\"0.0.0.0\",\"token\":\"secret\","
        "\"allowControl\":false,\"mqtt\":{\"password\":\"private\"},"
        "\"logging\":{\"level\":\"info\",\"unknown\":true},"
        "\"syslog\":{\"server\":\"nas\",\"port\":514,"
          "\"hostname\":\"living-room\",\"sources\":[\"system\",\"kernel\"],"
          "\"redact\":true},"
        "\"screenTime\":{\"enabled\":true,\"dailyMinutes\":90,"
          "\"action\":\"off\",\"badKey\":\"omit\"},"
        "\"update\":{\"check\":false,\"intervalHours\":24}}");
    const Json rows = Json::parse(
        "[{\"id\":\"homePromotion\",\"type\":\"switch\",\"on\":false},"
        "{\"id\":\"soundMode\",\"type\":\"choice\",\"value\":\"cinema\"}]");
    const Json home = Json::parse("{\"rows\":[\"one\"],\"picture\":\"movie\"}");
    const Json apps = Json::parse("[{\"id\":\"netflix\",\"title\":\"Netflix\",\"version\":\"1.0\"}]");
    const Json backup = service::createSettingsBackup(
        "2026-10-26T12:34:56.789Z", Json::text("OLED65C4"),
        Json::text("1.2.3"), config, rows, home, apps);

    require(backup["format"].string() == "nativetv-backup", "backup format changed");
    require(backup["version"].number() == 1, "backup version changed");
    require(backup["created"].string() == "2026-10-26T12:34:56.789Z",
            "backup timestamp changed");
    require(backup["from"]["model"].string() == "OLED65C4", "model was lost");
    require(backup["from"]["nativetv"].string() == "1.2.3", "version was lost");
    require(backup["lg"]["homePromotion"].valid() &&
            !backup["lg"]["homePromotion"].boolean(), "switch state changed");
    require(backup["lg"]["soundMode"].string() == "cinema", "choice state changed");
    require(backup["nativetv"]["logging"]["level"].string() == "info",
            "allowed config setting was dropped");
    require(backup["nativetv"]["syslog"].has("server") &&
            !backup["nativetv"]["syslog"].has("hostname"),
            "per-TV syslog hostname was not excluded");
    require(backup["nativetv"]["screenTime"]["dailyMinutes"].number() == 90 &&
            !backup["nativetv"]["screenTime"].has("badKey"),
            "screen time settings were not filtered");
    require(backup["nativetv"]["update"]["check"].valid() &&
            !backup["nativetv"]["update"].has("intervalHours"),
            "update settings were not filtered");
    require(!backup.has("host") && !backup.has("token") && !backup.has("mqtt"),
            "private server or MQTT settings leaked");
    require(backup["home"]["picture"].string() == "movie", "Home layout was lost");
    require(backup["apps"].isArray() && backup["apps"].size() == 1,
            "developer app list was lost");
    require(service::backupRestoreEnvelopeResult(backup) == "valid",
            "the native backup envelope validator rejected a version 1 export");
    require(service::backupRestoreEnvelopeResult(Json::parse(
                "{\"format\":\"other\",\"version\":1}")) == "notBackup" &&
            service::backupRestoreEnvelopeResult(Json::parse(
                "{\"format\":\"nativetv-backup\",\"version\":true}")) == "notBackup" &&
            service::backupRestoreEnvelopeResult(Json::parse("null")) == "notBackup",
            "the native backup envelope validator accepted an invalid format or version");
    require(service::backupRestoreEnvelopeResult(Json::parse(
                "{\"format\":\"nativetv-backup\",\"version\":2}")) == "newer" &&
            service::backupRestoreEnvelopeResult(Json::parse(
                "{\"format\":\"nativetv-backup\",\"version\":-1}")) == "valid",
            "the native backup envelope validator changed legacy version handling");

    const Json restoreConfig = service::safeBackupConfig(Json::parse(
        "{\"logging\":{\"level\":\"debug\",\"invalid\":true},"
        "\"syslog\":{\"server\":\"nas\",\"hostname\":\"other-tv\","
          "\"port\":70000},"
        "\"screenTime\":{\"enabled\":true,\"dailyMinutes\":90,"
          "\"apps\":{\"com.example.app\":30},\"action\":\"launch\"},"
        "\"token\":\"must-not-restore\",\"mqtt\":{\"password\":\"secret\"}}"));
    require(restoreConfig["logging"]["level"].string() == "debug" &&
            !restoreConfig["logging"].has("invalid"),
            "backup restore config validation did not keep only known logging values");
    require(restoreConfig["syslog"]["server"].string() == "nas" &&
            !restoreConfig["syslog"].has("hostname") &&
            !restoreConfig["syslog"].has("port"),
            "backup restore config accepted private or out-of-range syslog values");
    require(restoreConfig["screenTime"]["enabled"].boolean() &&
            restoreConfig["screenTime"]["dailyMinutes"].number() == 90 &&
            !restoreConfig["screenTime"].has("action") &&
            !restoreConfig.has("token") && !restoreConfig.has("mqtt"),
            "backup restore config did not filter unsupported and unsafe values");

    std::string unicodeServer;
    for (size_t index = 0; index < 130; ++index)
        unicodeServer += "\xf0\x9f\x98\x80";
    std::string validAppKey;
    for (size_t index = 0; index < 100; ++index)
        validAppKey += "\xf0\x9f\x98\x80";
    std::string invalidAppKey = validAppKey + "\xf0\x9f\x98\x80";
    Json unicodeConfig = Json::object();
    Json unicodeSyslog = Json::object();
    unicodeSyslog.set("server", Json::text(unicodeServer));
    unicodeConfig.set("syslog", unicodeSyslog);
    Json unicodeScreenTime = Json::object();
    Json unicodeApps = Json::object();
    unicodeApps.set(validAppKey, Json::numeric(30));
    unicodeScreenTime.set("apps", unicodeApps);
    unicodeConfig.set("screenTime", unicodeScreenTime);
    const Json safeUnicode = service::safeBackupConfig(unicodeConfig);
    require(safeUnicode["syslog"]["server"].string() == unicodeServer,
            "backup config used UTF-8 byte length instead of JavaScript string length");
    require(safeUnicode["screenTime"].isObject() &&
            safeUnicode["screenTime"]["apps"].isObject(),
            "backup screen-time app mapping was rejected");
    require(safeUnicode["screenTime"]["apps"].has(validAppKey),
            "backup screen-time app name at 200 UTF-16 units was rejected");
    Json invalidScreenTime = Json::object();
    Json invalidApps = Json::object();
    invalidApps.set(invalidAppKey, Json::numeric(45));
    invalidScreenTime.set("apps", invalidApps);
    unicodeConfig.set("screenTime", invalidScreenTime);
    const Json safeInvalidUnicode = service::safeBackupConfig(unicodeConfig);
    require(!safeInvalidUnicode["screenTime"].has("apps"),
            "backup screen-time app name over 200 UTF-16 units was accepted");

    std::string longBackupTitle;
    std::string expectedBackupTitle;
    for (size_t index = 0; index < 41; ++index) {
        longBackupTitle += "\xf0\x9f\x98\x80";
        if (index < 40) expectedBackupTitle += "\xf0\x9f\x98\x80";
    }
    Json restorePlanInputApps = Json::parse(
        "[{\"id\":\"com.example.present\",\"title\":\"Present\"},"
        "{\"id\":\"youtube.leanback.v4\",\"title\":\"YouTube\"},"
        "{\"id\":\"my.sideloaded.tool\",\"title\":\"My tool\"},"
        "{\"id\":\"org.webosbrew.hbchannel\",\"title\":\"Protected\"},"
        "{\"id\":\"../../etc\",\"title\":\"Invalid\"}]" );
    Json installedRestoreApps = Json::array();
    installedRestoreApps.add(Json::text("com.example.present"));
    Json catalogRestoreApps = Json::array();
    catalogRestoreApps.add(Json::text("youtube.leanback.v4"));
    const Json restorePlan = service::planBackupRestoreApps(
        restorePlanInputApps, installedRestoreApps, catalogRestoreApps, true, "");
    require(restorePlan["toInstall"].size() == 1 &&
            restorePlan["toInstall"][0]["id"].string() == "youtube.leanback.v4" &&
            restorePlan["manual"].size() == 1 &&
            restorePlan["manual"][0]["id"].string() == "my.sideloaded.tool" &&
            restorePlan["present"].number() == 1 &&
            restorePlan["catalogError"].dump() == "null",
            "native restore planner did not split installed, catalog and sideloaded apps");
    Json offlinePlan = service::planBackupRestoreApps(
        restorePlanInputApps, Json::array(), Json::array(), false, "no catalog");
    require(offlinePlan["toInstall"].size() == 3 &&
            offlinePlan["manual"].size() == 0 &&
            offlinePlan["catalogError"].string() == "no catalog",
            "native restore planner changed the no-catalog fallback");
    Json restorePlanLongTitleApp = Json::object();
    restorePlanLongTitleApp.set("id", Json::text("com.example.title"));
    restorePlanLongTitleApp.set("title", Json::text(longBackupTitle));
    Json restorePlanLongTitleApps = Json::array();
    restorePlanLongTitleApps.add(restorePlanLongTitleApp);
    const Json truncatedPlan = service::planBackupRestoreApps(
        restorePlanLongTitleApps, Json::array(), Json::array(), false, "no catalog");
    require(truncatedPlan["toInstall"][0]["title"].string() == expectedBackupTitle,
            "native restore planner did not cap titles at 80 UTF-16 units");

    const Json switchRow = Json::parse(
        "{\"id\":\"homePromotion\",\"type\":\"switch\",\"on\":false}");
    const service::LgSettingWrite switchWrite = service::lgSettingsRestoreWrite(
        "homePromotion", Json::flag(true), switchRow);
    require(switchWrite.handled && switchWrite.valid &&
            switchWrite.category == "general" && switchWrite.key == "homePromotion" &&
            switchWrite.storedValue.string() == "on",
            "native backup restore did not map a switch to LG's stored value");

    const Json choiceRow = Json::parse(
        "{\"id\":\"soundMode\",\"type\":\"choice\",\"value\":\"movie\","
        "\"choices\":[{\"value\":\"movie\"},{\"value\":\"sports\"}]}");
    const service::LgSettingWrite choiceWrite = service::lgSettingsRestoreWrite(
        "soundMode", Json::text("sports"), choiceRow);
    require(choiceWrite.handled && choiceWrite.valid &&
            choiceWrite.category == "sound" && choiceWrite.key == "soundMode" &&
            choiceWrite.storedValue.string() == "sports",
            "native backup restore did not accept a TV-offered choice");
    require(!service::lgSettingsRestoreWrite(
                "soundMode", Json::text("cinema"), choiceRow).valid,
            "native backup restore accepted a choice the TV did not offer");

    const Json numberRow = Json::parse(
        "{\"id\":\"audioBalance\",\"type\":\"number\",\"value\":0,"
        "\"min\":-40,\"max\":40}");
    const service::LgSettingWrite numberWrite = service::lgSettingsRestoreWrite(
        "audioBalance", Json::numeric(12), numberRow);
    require(numberWrite.handled && numberWrite.valid &&
            numberWrite.category == "sound" && numberWrite.key == "audioBalance" &&
            numberWrite.storedValue.string() == "12",
            "native backup restore did not retain LG's string-stored number: " +
            numberWrite.category + "/" + numberWrite.key + "/" +
            numberWrite.storedValue.dump());
    require(!service::lgSettingsRestoreWrite(
                "audioBalance", Json::numeric(41), numberRow).valid &&
            !service::lgSettingsRestoreWrite(
                "audioBalance", Json::numeric(1.5), numberRow).valid,
            "native backup restore accepted an out-of-range or fractional number");

    char directoryTemplate[] = "/tmp/nativetv-backup-test-XXXXXX";
    char *directoryName = mkdtemp(directoryTemplate);
    require(directoryName != nullptr, "could not create backup test directory");
    const std::string directory(directoryName);
    const std::string name = "nativetv-backup-2026-10-26.json";
    const std::string content = "{\"format\":\"nativetv-backup\",\"version\":1}\n";
    const std::string path = directory + "/" + name;
    {
        std::ofstream output(path.c_str(), std::ios::binary);
        output << content;
    }
    std::string saved;
    bool backupExists = false;
    require(service::validSavedBackupName(name), "daily backup name was rejected");
    require(!service::validSavedBackupName("../config.json"),
            "path traversal backup name was accepted");
    require(service::savedBackupExists(directory, name, backupExists) && backupExists,
            "existing daily backup was not detected");
    require(service::savedBackupExists(directory, "nativetv-backup-2026-10-25.json",
                                      backupExists) && !backupExists,
            "missing daily backup was reported as present");
    require(service::readSavedBackup(directory, name, saved) && saved == content,
            "saved backup bytes changed");
    require(!service::readSavedBackup(directory, "nativetv-backup-2026-10-25.json",
                                      saved),
            "missing saved backup was reported as present");
    Json listedBackups = Json::array();
    require(service::listSavedBackups(directory, listedBackups) &&
                listedBackups.size() == 1 &&
                listedBackups[0]["name"].string() == name &&
                listedBackups[0]["date"].string() == "2026-10-26" &&
                listedBackups[0]["size"].number() ==
                    static_cast<double>(content.size()),
            "saved backup listing did not return its name, date and byte size");

    const std::string pendingPath = directory + "/restore-apps.json";
    {
        std::ofstream output(pendingPath.c_str(), std::ios::binary);
        output << R"({"saved":123,"apps":[{"id":"com.example.present","title":"Present"},{"id":"com.example.missing","title":"Missing"}]})";
    }
    const Json pending = service::readPendingBackupApps(pendingPath);
    Json installed = Json::array();
    installed.add(Json::text("com.example.present"));
    const Json missingApps = service::pendingBackupApps(pending, installed);
    require(missingApps.size() == 1 &&
            missingApps[0]["id"].string() == "com.example.missing" &&
            missingApps[0]["title"].string() == "Missing",
            "pending app projection did not retain only apps not installed");

    Json restoreApps = Json::array();
    Json restoreApp = Json::object();
    restoreApp.set("id", Json::text("com.example.restore"));
    restoreApp.set("title", Json::text("Restore me"));
    restoreApps.add(restoreApp);
    std::string pendingError;
    require(service::savePendingBackupApps(pendingPath, Json::numeric(456),
                                            restoreApps, &pendingError),
            "valid pending app list was not saved: " + pendingError);
    const Json savedApps = service::readPendingBackupApps(pendingPath);
    require(savedApps.size() == 1 &&
            savedApps[0]["id"].string() == "com.example.restore" &&
            savedApps[0]["title"].string() == "Restore me",
            "saved pending app list changed shape");
    struct stat pendingInfo = {};
    require(stat(pendingPath.c_str(), &pendingInfo) == 0 &&
            (pendingInfo.st_mode & 0777) == 0600,
            "pending app list permissions are not private");

    Json unsafeApps = Json::array();
    Json unsafeApp = Json::object();
    unsafeApp.set("id", Json::text("../config"));
    unsafeApp.set("title", Json::text("Unsafe"));
    unsafeApps.add(unsafeApp);
    require(!service::savePendingBackupApps(pendingPath, Json::numeric(456),
                                            unsafeApps, &pendingError),
            "path-like pending app id was accepted");
    unsafeApp.set("id", Json::text("com.webos.app.settings"));
    unsafeApps = Json::array();
    unsafeApps.add(unsafeApp);
    require(!service::savePendingBackupApps(pendingPath, Json::numeric(456),
                                            unsafeApps, &pendingError),
            "protected pending app id was accepted");
    Json longTitleApps = Json::array();
    Json longTitleApp = Json::object();
    longTitleApp.set("id", Json::text("com.example.restore"));
    longTitleApp.set("title", Json::text(std::string(81, 'x')));
    longTitleApps.add(longTitleApp);
    require(!service::savePendingBackupApps(pendingPath, Json::numeric(456),
                                            longTitleApps, &pendingError),
            "overlong pending app title was accepted");
    require(!service::savePendingBackupApps(pendingPath, Json::numeric(456.5),
                                            restoreApps, &pendingError),
            "fractional pending app timestamp was accepted");
    require(!service::savePendingBackupApps(pendingPath, Json::numeric(-1),
                                            restoreApps, &pendingError),
            "negative pending app timestamp was accepted");
    require(service::clearPendingBackupApps(pendingPath, &pendingError) &&
            access(pendingPath.c_str(), F_OK) != 0,
            "pending app list was not cleared");
    require(service::clearPendingBackupApps(pendingPath, &pendingError),
            "clearing a missing pending app list should succeed");

    const std::string dailyDirectory = directory + "/daily";
    require(mkdir(dailyDirectory.c_str(), 0700) == 0,
            "could not create daily backup fixture directory");
    for (int day = 10; day <= 16; ++day) {
        char filename[64];
        std::snprintf(filename, sizeof(filename),
                      "nativetv-backup-2026-10-%02d.json", day);
        const std::string oldPath = dailyDirectory + "/" + filename;
        std::ofstream output(oldPath.c_str(), std::ios::binary);
        output << "old";
        output.close();
        require(output.good(), "could not create older daily backup fixture");
    }
    Json dailyDocument = Json::object();
    dailyDocument.set("format", Json::text("nativetv-backup"));
    dailyDocument.set("version", Json::numeric(1));
    dailyDocument.set("created", Json::text("2026-10-17T00:00:00.000Z"));
    const std::string dailyName = "nativetv-backup-2026-10-17.json";
    require(service::saveDailyBackup(dailyDirectory, dailyName, dailyDocument,
                                     &pendingError),
            "valid daily backup was not saved: " + pendingError);
    require(service::readSavedBackup(dailyDirectory, dailyName, saved) &&
            Json::parse(saved)["created"].string() == "2026-10-17T00:00:00.000Z",
            "saved daily backup could not be read back");
    require(service::listSavedBackups(dailyDirectory, listedBackups) &&
                listedBackups.size() == 7 &&
                listedBackups[0]["name"].string() == dailyName &&
                listedBackups[6]["name"].string() ==
                    "nativetv-backup-2026-10-11.json",
            "saved backup listing is not newest-first or includes expired backups");
    require(service::dailyBackupNameDue(true, "2026-10-15T09:00:00.000Z") ==
                "nativetv-backup-2026-10-15.json" &&
            service::dailyBackupNameDue(true, "2026-10-15T23:59:59.999Z") ==
                "nativetv-backup-2026-10-15.json" &&
            service::dailyBackupNameDue(true, "2026-10-16T00:00:00.000Z") ==
                "nativetv-backup-2026-10-16.json",
            "daily backups are not named by the UTC date");
    require(service::dailyBackupNameDue(false, "2026-10-15T09:00:00.000Z").empty(),
            "a daily backup was due while the switch is off");
    require(service::dailyBackupNameDue(true, "1970-01-02T00:00:00.000Z").empty() &&
            service::dailyBackupNameDue(true, "2025-12-31T23:59:59.999Z").empty() &&
            service::dailyBackupNameDue(true, "2026-01-01T00:00:00.000Z") ==
                "nativetv-backup-2026-01-01.json" &&
            service::dailyBackupNameDue(true, "").empty(),
            "a daily backup was due before the clock was set");
    require(service::validSavedBackupName(
                service::dailyBackupNameDue(true, "2026-10-15T09:00:00.000Z")),
            "a due name is not one that can be read back");
    const std::string expiredDaily = dailyDirectory + "/nativetv-backup-2026-10-10.json";
    require(access(expiredDaily.c_str(), F_OK) != 0,
            "daily backup retention did not remove the oldest file");
    require(!service::readSavedBackup(dailyDirectory, "nativetv-backup-2026-10-10.json", saved) &&
            !service::readSavedBackup(dailyDirectory, "nativetv-backup-2026-10-17.json.tmp", saved) &&
            !service::readSavedBackup(dailyDirectory, "../config.json", saved),
            "a pruned, temporary or path-like backup could be read");
    struct stat dailyInfo = {};
    require(stat((dailyDirectory + "/" + dailyName).c_str(), &dailyInfo) == 0 &&
            (dailyInfo.st_mode & 0777) == 0600,
            "daily backup permissions are not private");
    require(!service::saveDailyBackup(dailyDirectory, "../config.json", dailyDocument,
                                      &pendingError),
            "path-like daily backup filename was accepted");
    Json invalidDailyDocument = Json::object();
    invalidDailyDocument.set("format", Json::text("other"));
    invalidDailyDocument.set("version", Json::numeric(1));
    require(!service::saveDailyBackup(dailyDirectory, dailyName, invalidDailyDocument,
                                      &pendingError),
            "daily backup with an unknown format was accepted");
    for (int day = 10; day <= 17; ++day) {
        char filename[64];
        std::snprintf(filename, sizeof(filename),
                      "nativetv-backup-2026-10-%02d.json", day);
        unlink((dailyDirectory + "/" + filename).c_str());
    }
    rmdir(dailyDirectory.c_str());

    const std::string outside = directory + "/outside.json";
    {
        std::ofstream output(outside.c_str(), std::ios::binary);
        output << "private";
    }
    require(unlink(path.c_str()) == 0 && symlink(outside.c_str(), path.c_str()) == 0,
            "could not prepare symlink backup fixture");
    require(!service::readSavedBackup(directory, name, saved),
            "symlinked backup was followed");
    require(service::listSavedBackups(directory, listedBackups) &&
                listedBackups.size() == 0,
            "saved backup listing included a symlink");
    require(service::savedBackupExists(directory, name, backupExists) && backupExists,
            "symlinked backup name was not treated as occupied");
    const std::string directoryLink = directory + "-link";
    require(symlink(directory.c_str(), directoryLink.c_str()) == 0,
            "could not prepare symlink directory fixture");
    require(!service::readSavedBackup(directoryLink, name, saved),
            "symlinked backup directory was followed");
    unlink(directoryLink.c_str());
    unlink(path.c_str());
    unlink(outside.c_str());
    unlink(pendingPath.c_str());
    rmdir(directory.c_str());
    return 0;
}
