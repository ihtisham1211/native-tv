#include "daemon/backup_restore.hpp"
#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <vector>

using namespace nativetv;
using namespace nativetv::daemon;

namespace {
void check(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}

// Scripted services: what each is asked, and what it answers.
struct Services {
    Json lgResult = Json::parse("{\"ok\":true,\"applied\":[\"soundMode\"],\"unchanged\":1,"
                                "\"skipped\":[],\"failed\":[]}");
    bool lgRuns = true;
    Json plan = Json::parse("{\"toInstall\":[{\"id\":\"a\",\"title\":\"A\"}],\"manual\":[],"
                            "\"present\":2,\"catalogError\":null}");
    bool planRuns = true;
    Json homeAnswer = Json::parse("{\"ok\":true,\"layoutVersion\":\"1\"}");
    Json changed = Json::parse("[\"ntp.server\"]");
    bool configWrites = true;
    std::string configError = "disk full";
    std::vector<std::string> log;
    Json lgAsked, apps, layoutAsked, sectionAsked;
    std::vector<std::string> warnings;

    BackupRestoreDeps deps() {
        BackupRestoreDeps d;
        d.restoreLg = [this](const Json &wanted, Json &result, std::string &error) {
            log.push_back("lg");
            lgAsked = wanted;
            if (!lgRuns) { error = "native backup LG restore returned HTTP 400"; return false; }
            result = lgResult;
            return true;
        };
        d.planApps = [this](const Json &list, Json &result, std::string &error) {
            log.push_back("plan");
            apps = list;
            if (!planRuns) { error = "planner down"; return false; }
            result = plan;
            return true;
        };
        d.savePendingApps = [this](const Json &toInstall, long long saved, std::string &) {
            log.push_back("save:" + std::to_string(toInstall.size()) + "@" + std::to_string(saved));
            return true;
        };
        d.clearPendingApps = [this](std::string &) { log.push_back("clear"); return true; };
        d.restoreHome = [this](const Json &layout) {
            log.push_back("home");
            layoutAsked = layout;
            return homeAnswer;
        };
        d.writeConfig = [this](const Json &section, Json &out, std::string &error) {
            log.push_back("config");
            sectionAsked = section;
            if (!configWrites) { error = configError; return false; }
            out = changed;
            return true;
        };
        d.warn = [this](const std::string &text) { warnings.push_back(text); };
        d.nowMs = []() { return 1700000000000LL; };
        return d;
    }
};

Json backup(const char *extra = "") {
    return Json::parse(std::string("{\"format\":\"nativetv-backup\",\"version\":1") + extra + "}");
}
} // namespace

int main() {
    try {
        {   // Not a backup, and one from the future: refused before anything is written.
            Services s;
            for (const char *text : {"null", "{}", "[]", "\"x\"", "{\"format\":\"other\",\"version\":1}",
                                     "{\"format\":\"nativetv-backup\"}"}) {
                const BackupRestoreOutcome out = restoreBackup(s.deps(), Json::parse(text));
                check(out.status == 400 && !out.body["ok"].boolean(), "refused");
                check(out.body["error"].string() == "That file is not a Native Tv backup", "not a backup");
            }
            const BackupRestoreOutcome newer = restoreBackup(
                s.deps(), Json::parse("{\"format\":\"nativetv-backup\",\"version\":2}"));
            check(newer.status == 400 &&
                  newer.body["error"].string() ==
                      "That backup is from a newer Native Tv: update this TV first", "newer");
            check(s.log.empty(), "nothing written for a refused file");
        }
        {   // The whole restore, in order, with the answer the legacy route gave.
            Services s;
            const BackupRestoreOutcome out = restoreBackup(
                s.deps(), backup(",\"lg\":{\"soundMode\":\"movie\"},\"apps\":[{\"id\":\"a\"}],"
                                 "\"home\":{\"rows\":[\"app\"]},\"nativetv\":{\"ntp\":{\"server\":\"x\"}}"));
            check(out.status == 200 && out.body["ok"].boolean(), "ok");
            check(s.log == (std::vector<std::string>{"lg", "plan", "save:1@1700000000000", "home", "config"}),
                  "order: LG, apps, pending list, Home, Native Tv");
            check(s.lgAsked["soundMode"].string() == "movie", "LG settings passed on");
            check(s.apps.size() == 1 && s.layoutAsked["rows"].size() == 1, "apps and layout passed on");
            check(s.sectionAsked["ntp"]["server"].string() == "x", "Native Tv section passed on");
            check(out.body["lg"]["applied"][0].string() == "soundMode" &&
                  out.body["lg"]["unchanged"].number() == 1, "LG result");
            check(out.body["lg"].keys() ==
                  (std::vector<std::string>{"applied", "unchanged", "skipped", "failed"}),
                  "the service's own ok is not part of the answer");
            check(out.body["nativetv"].size() == 1 && out.restart && out.body["restart"].boolean(),
                  "changed settings restart the server");
            check(out.body["apps"]["present"].number() == 2 &&
                  out.body["apps"]["toInstall"].size() == 1 &&
                  out.body["apps"]["manual"].size() == 0, "app plan");
            check(out.body["apps"].has("catalogError") && !out.body["apps"]["catalogError"].valid(),
                  "no catalog error is null");
            check(out.body["home"].boolean(), "Home restored");
            const std::vector<std::string> keys = out.body.keys();
            check(keys == (std::vector<std::string>{"ok", "lg", "nativetv", "restart", "apps", "home"}),
                  "keys in the legacy order");
        }
        {   // Nothing to install clears the pending list; nothing changed does not restart.
            Services s;
            s.plan = Json::parse("{\"toInstall\":[],\"manual\":[{\"id\":\"m\"}],\"present\":0,"
                                 "\"catalogError\":\"no catalog\"}");
            s.changed = Json::parse("[]");
            const BackupRestoreOutcome out = restoreBackup(s.deps(), backup());
            check(s.log == (std::vector<std::string>{"lg", "plan", "clear", "config"}),
                  "no Home layout, so Home is not asked");
            check(!out.restart && !out.body["restart"].boolean() && !out.body["home"].boolean(), "no restart");
            check(out.body["apps"]["catalogError"].string() == "no catalog", "catalog error kept");
            check(s.lgAsked.isObject() && s.lgAsked.keys().empty(), "no lg is an empty object");
            check(s.sectionAsked.isObject() && s.sectionAsked.keys().empty(), "no section is an empty object");
        }
        {   // A list of settings names none the TV has; a bare value names none.
            Services s;
            const BackupRestoreOutcome out = restoreBackup(s.deps(), backup(",\"lg\":[true,\"x\"]"));
            check(out.body["lg"]["skipped"].size() == 2 &&
                  out.body["lg"]["skipped"][1]["id"].string() == "1" &&
                  out.body["lg"]["skipped"][1]["reason"].string() == "absent", "list skipped");
            check(std::count(s.log.begin(), s.log.end(), "lg") == 0, "the TV is not asked");
            Services t;
            (void)restoreBackup(t.deps(), backup(",\"lg\":\"x\",\"home\":3,\"nativetv\":[1]"));
            check(t.lgAsked.isObject() && t.lgAsked.keys().empty() && !t.layoutAsked.valid() &&
                  t.sectionAsked.isObject() && t.sectionAsked.keys().empty(), "bare values count as none");
        }
        {   // The LG service failing is reported in `failed`, and the rest goes on.
            Services s;
            s.lgRuns = false;
            const BackupRestoreOutcome out = restoreBackup(s.deps(), backup());
            check(out.status == 200 && out.body["ok"].boolean(), "restore continues");
            check(out.body["lg"]["failed"][0]["id"].string() == "restore" &&
                  out.body["lg"]["failed"][0]["error"].string() ==
                      "native backup LG restore returned HTTP 400", "failure named");
            check(out.body["lg"]["applied"].size() == 0 && out.body["lg"]["unchanged"].number() == 0,
                  "nothing applied");
            check(s.warnings.size() == 1, "warned");
        }
        {   // The planner failing leaves an empty plan with the reason.
            Services s;
            s.planRuns = false;
            const BackupRestoreOutcome out = restoreBackup(s.deps(), backup(",\"apps\":\"x\""));
            check(out.body["apps"]["toInstall"].size() == 0 && out.body["apps"]["catalogError"].string() ==
                  "planner down", "planner failure");
            check(s.apps.size() == 0, "bare apps are an empty list");
        }
        {   // Native Tv's settings not saving, and the Home layout not restoring.
            Services s;
            s.configWrites = false;
            const BackupRestoreOutcome out = restoreBackup(s.deps(), backup());
            check(out.status == 400 && !out.body["ok"].boolean() && !out.restart, "config failure");
            check(out.body["error"].string() ==
                  "Could not save Native Tv\xE2\x80\x99s own settings: disk full", "config error text");
            check(out.body["nativetv"].size() == 0 && !out.body["restart"].boolean(), "nothing changed");
            check(out.body.keys()[1] == "error", "error follows ok");

            Services h;
            h.homeAnswer = Json::parse("{\"ok\":false,\"error\":\"could not save the layout\"}");
            const BackupRestoreOutcome home = restoreBackup(h.deps(), backup(",\"home\":{}"));
            check(home.status == 400 && !home.body["ok"].boolean() && !home.body["home"].boolean(), "home failure");
            check(home.body["error"].string() ==
                  "Other settings may have been restored, but the Home layout could not be restored: "
                  "could not save the layout", "home error text");
            check(home.body["nativetv"].size() == 1 && home.restart, "the rest was restored");
            const std::vector<std::string> keys = home.body.keys();
            check(keys.back() == "error", "error added last");

            Services u;
            u.homeAnswer = Json::parse("{\"ok\":false}");
            const BackupRestoreOutcome unknown = restoreBackup(u.deps(), backup(",\"home\":[]"));
            check(unknown.body["error"].string().find("unknown error") != std::string::npos, "unknown error");
        }
    } catch (const std::exception &error) {
        fprintf(stderr, "backup restore test failed: %s\n", error.what());
        return 1;
    }
    return 0;
}
