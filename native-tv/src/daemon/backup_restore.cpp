#include "daemon/backup_restore.hpp"
#include "domain/say.hpp"
#include "service/backup.hpp"
#include "service/preload.hpp"

namespace nativetv {
namespace daemon {
namespace {

Json emptyLgResult() {
    Json result = Json::object();
    result.set("applied", Json::array());
    result.set("unchanged", Json::numeric(0));
    result.set("skipped", Json::array());
    result.set("failed", Json::array());
    return result;
}

Json lgSettingsResult(const BackupRestoreDeps &deps, const Json &backup) {
    const Json given = backup["lg"];
    // A list has no setting names: each of its places is a setting the TV lacks.
    if (given.isArray()) {
        Json result = emptyLgResult();
        Json skipped = Json::array();
        for (size_t index = 0; index < given.size(); ++index) {
            Json issue = Json::object();
            issue.set("id", Json::text(std::to_string(index)));
            issue.set("reason", Json::text("absent"));
            skipped.add(issue);
        }
        result.set("skipped", skipped);
        return result;
    }
    const Json wanted = given.isObject() ? given : Json::object();
    Json result;
    std::string error;
    if (deps.restoreLg && deps.restoreLg(wanted, result, error) && result.isObject()) {
        // Only the four lists are the answer; the service's own `ok` is not.
        Json settings = Json::object();
        for (const char *key : {"applied", "unchanged", "skipped", "failed"})
            settings.set(key, result[key]);
        return settings;
    }
    if (error.empty()) error = "invalid native restore result";
    if (deps.warn) deps.warn("backup: native LG restore failed: " + error);
    Json issue = Json::object();
    issue.set("id", Json::text("restore"));
    issue.set("error", Json::text(error));
    Json failed = Json::array();
    failed.add(issue);
    Json failure = emptyLgResult();
    failure.set("failed", failed);
    return failure;
}

// The apps this TV lacks, kept on file as the restore may restart the server.
Json appsResult(const BackupRestoreDeps &deps, const Json &backup) {
    const Json apps = backup["apps"].isArray() ? backup["apps"] : Json::array();
    Json plan;
    std::string error;
    const bool planned = deps.planApps && deps.planApps(apps, plan, error) &&
                         plan["toInstall"].isArray() && plan["manual"].isArray() &&
                         plan["present"].isNumber();
    if (!planned) {
        if (deps.warn)
            deps.warn("backup: native restore planner failed" +
                      (error.empty() ? std::string() : ": " + error));
        plan = Json::object();
        plan.set("toInstall", Json::array());
        plan.set("manual", Json::array());
        plan.set("present", Json::numeric(0));
        plan.set("catalogError", Json::text(error.empty() ? "invalid native restore plan" : error));
    }
    std::string saveError;
    const bool saved = plan["toInstall"].size()
        ? (deps.savePendingApps &&
           deps.savePendingApps(plan["toInstall"], deps.nowMs ? deps.nowMs() : 0, saveError))
        : (deps.clearPendingApps && deps.clearPendingApps(saveError));
    if (!saved && deps.warn)
        deps.warn(std::string("backup: could not ") +
                  (plan["toInstall"].size() ? "save" : "clear") +
                  " pending restore-app list: " + saveError);
    Json out = Json::object();
    out.set("toInstall", plan["toInstall"]);
    out.set("manual", plan["manual"]);
    out.set("present", plan["present"]);
    out.set("catalogError", service::javascriptTruthy(plan["catalogError"])
        ? plan["catalogError"] : Json());
    return out;
}

} // namespace

BackupRestoreOutcome restoreBackup(const BackupRestoreDeps &deps, const Json &backup) {
    BackupRestoreOutcome outcome;
    const auto refuse = [&outcome](const std::string &message) {
        outcome.status = 400;
        outcome.body = Json::object();
        outcome.body.set("ok", Json::flag(false));
        outcome.body.set("error", Json::text(message));
        return outcome;
    };
    const std::string envelope = service::backupRestoreEnvelopeResult(backup);
    if (envelope == "newer")
        return refuse(say::msg("srv.backup.newer",
                               "That backup is from a newer Native Tv: update this TV first"));
    if (envelope != "valid")
        return refuse(say::msg("srv.backup.notBackup", "That file is not a Native Tv backup"));

    const Json lg = lgSettingsResult(deps, backup);
    const Json apps = appsResult(deps, backup);

    // The layout is checked and trimmed by Native Tv Home as the dashboard's is.
    const Json layout = backup["home"];
    Json homeAnswer;
    if (deps.restoreHome && (layout.isObject() || layout.isArray()))
        homeAnswer = deps.restoreHome(layout);

    const Json section = backup["nativetv"].isObject() ? backup["nativetv"] : Json::object();
    Json changed = Json::array();
    std::string writeError;
    const bool written = deps.writeConfig && deps.writeConfig(section, changed, writeError);
    if (!written || !changed.isArray()) changed = Json::array();

    Json body = Json::object();
    body.set("ok", Json::flag(written));
    if (!written) {
        Json variables = Json::object();
        variables.set("error", Json::text(writeError));
        body.set("error", Json::text(say::msg(
            "srv.backup.saveFailed",
            "Could not save Native Tv\xE2\x80\x99" "s own settings: {error}", variables)));
    }
    body.set("lg", lg);
    body.set("nativetv", changed);
    outcome.restart = written && changed.size() > 0;
    body.set("restart", Json::flag(outcome.restart));
    body.set("apps", apps);
    body.set("home", Json::flag(homeAnswer.isObject() && service::javascriptTruthy(homeAnswer["ok"])));
    if (homeAnswer.valid() && !(homeAnswer.isObject() && service::javascriptTruthy(homeAnswer["ok"]))) {
        Json variables = Json::object();
        const Json reason = homeAnswer["error"];
        variables.set("error", Json::text(service::javascriptTruthy(reason)
            ? service::javascriptString(reason) : "unknown error"));
        body.set("ok", Json::flag(false));
        body.set("error", Json::text(say::msg(
            "srv.home.backupFailed",
            "Other settings may have been restored, but the Home layout could not be restored: {error}",
            variables)));
    }
    outcome.status = body["ok"].boolean() ? 200 : 400;
    outcome.body = body;
    return outcome;
}

} // namespace daemon
} // namespace nativetv
