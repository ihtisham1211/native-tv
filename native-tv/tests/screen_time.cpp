#include "service/screen_time.hpp"
#include <iostream>
#include <string>

using namespace nativetv;

namespace {
int failures = 0;

void check(bool condition, const std::string &message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}
} // namespace

int main() {
    Json value;
    std::string error;
    check(service::validateScreenTimePatch(
              Json::parse("{\"dailyMinutes\":90,\"action\":\"screenOff\","
                          "\"bedtimeStart\":\"\"}"), value, error) &&
              value["dailyMinutes"].number() == 90 && error.empty(),
          "valid partial settings are copied");
    check(!service::validateScreenTimePatch(Json::parse("{\"dailyMinutes\":2000}"),
                                            value, error) &&
              error == "invalid dailyMinutes",
          "daily limits above 1440 are rejected with the legacy error");
    check(!service::validateScreenTimePatch(Json::parse("{\"dailyMinutes\":1.5}"),
                                            value, error) &&
              error == "invalid dailyMinutes",
          "fractional minute limits are rejected");
    check(!service::validateScreenTimePatch(Json::parse("{\"bedtimeStart\":\"25:00\"}"),
                                            value, error) &&
              error == "invalid bedtimeStart",
          "invalid local clock strings are rejected");
    check(!service::validateScreenTimePatch(Json::parse("{\"action\":\"home\"}"),
                                            value, error) &&
              error == "invalid action",
          "Home remains an app action only");
    check(!service::validateScreenTimePatch(Json::parse("{\"apps\":{\"netflix\":0}}"),
                                            value, error) &&
              error == "invalid apps",
          "app limits must be positive whole minutes");
    check(!service::validateScreenTimePatch(Json::parse("{\"allowPower\":true}"),
                                            value, error) &&
              error == "unknown setting: allowPower",
          "patches cannot change fields outside screen-time settings");
    check(!service::validateScreenTimePatch(Json::parse("[1]"), value, error) &&
              error == "unknown setting: 0",
          "array patches preserve the legacy unknown-index error");
    check(!service::validateScreenTimePatch(Json::parse("{\"apps\":[]}"), value, error) &&
              error == "invalid apps",
          "app limits must be a map");
    Json tooMany = Json::object();
    for (int index = 0; index < 51; ++index)
        tooMany.set("app" + std::to_string(index), Json::numeric(1));
    Json apps = Json::object();
    apps.set("apps", tooMany);
    check(!service::validateScreenTimePatch(apps, value, error) && error == "invalid apps",
          "at most fifty app limits are accepted");
    Json longKey = Json::object();
    longKey.set(std::string(199, 'a') + "\xf0\x9f\x8e\xac", Json::numeric(1));
    apps = Json::object();
    apps.set("apps", longKey);
    check(!service::validateScreenTimePatch(apps, value, error) && error == "invalid apps",
          "app key length uses UTF-16 code units");

    Json normalized = service::screenTimeSettings(
        Json::parse("{\"screenTime\":{\"enabled\":true,\"dailyMinutes\":2000,"
                    "\"appAction\":\"home\"}}"), true, false);
    check(normalized["enabled"].boolean() && normalized["dailyMinutes"].number() == 0 &&
              normalized["appAction"].string() == "off" && normalized["apps"].isObject(),
          "stored settings use per-field defaults and Home fallback without the Home app");
    normalized = service::screenTimeSettings(
        Json::parse("{\"screenTime\":{\"appAction\":\"home\"}}"), false, false);
    check(normalized["appAction"].string() == "screenOff",
          "Home fallback respects a disabled power policy");

    const Json overnight = Json::parse(
        "{\"bedtimeStart\":\"22:30\",\"bedtimeEnd\":\"06:00\"}");
    const int64_t localTwoAm = 1792029600000LL;
    service::ScreenTimeBedtime bed = service::screenTimeBedtime(overnight, localTwoAm);
    check(bed.configured && bed.inside && bed.untilStartSeconds == 20 * 3600 + 30 * 60,
          "overnight bedtime includes the early morning and counts to its next start");
    bed = service::screenTimeBedtime(overnight, localTwoAm + 4 * 3600 * 1000LL);
    check(!bed.inside, "bedtime ends at its configured minute");
    check(!service::screenTimeBedtime(
               Json::parse("{\"bedtimeStart\":\"\",\"bedtimeEnd\":\"06:00\"}"),
               localTwoAm).configured,
          "a missing bedtime endpoint disables the window");

    Json settings = Json::parse(
        "{\"dailyMinutes\":60,\"action\":\"screenOff\",\"apps\":{"
        "\"netflix\":30},\"appAction\":\"warn\"}");
    Json today = Json::parse(
        "{\"netflix\":1800,\"hdmi2\":1900,\"screen_off\":9000,"
        "\"screen_saver\":9000}");
    service::ScreenTimeDue due = service::screenTimeTvDue(settings, localTwoAm, today);
    check(due.due && due.id == "daily" && due.action == "screenOff" &&
              due.text == "Today's screen time is used up.",
          "daily watched totals exclude screen-off and screen-saver time");
    today = Json::parse("{\"netflix\":1800,\"hdmi2\":1000}");
    check(!service::screenTimeTvDue(settings, localTwoAm, today).due,
          "the daily limit is not due while watched time remains");
    due = service::screenTimeAppDue(settings, "netflix", 1800);
    check(due.due && due.id == "app:netflix" && due.action == "warn",
          "app limits select the per-app action when exhausted");
    check(!service::screenTimeAppDue(settings, "netflix", 1799.9).due,
          "an app limit is not due before its recorded seconds are exhausted");

    if (failures)
        return 1;
    std::cout << "native screen-time policy checks passed\n";
    return 0;
}
