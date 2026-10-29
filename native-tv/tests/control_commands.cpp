#include "service/control_commands.hpp"
#include "service/lg_settings.hpp"
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
    const Json explicitNull = Json::parse("{\"time\":null}");
    check(explicitNull.has("time") && !explicitNull["time"].valid() &&
              !explicitNull.has("missing"),
          "JSON object membership distinguishes explicit null from a missing key");

    check(service::controlVolumeTarget(Json::numeric(72)) == 72,
          "numeric volume is preserved");
    check(service::controlVolumeTarget(Json::numeric(1e30)) == 1,
          "large numeric volume follows JavaScript exponent string coercion");
    check(service::controlVolumeStep(Json::text(" -140steps")) == -100 &&
              service::controlVolumeStep(Json::text("140steps")) == 100,
          "volume steps clamp to the legacy minus/plus one hundred range");
    check(service::controlVolumeStep(Json::text("invalid")) == 1,
          "invalid volume steps use the legacy default");
    check(service::controlVolumeTarget(Json::text("  38percent")) == 38,
          "string volume follows parseInt prefix semantics");
    check(service::controlVolumeTarget(Json::text("-4")) == 0,
          "volume is clamped at zero");
    check(service::controlVolumeTarget(Json::text("118")) == 100,
          "volume is clamped at one hundred");
    check(service::controlVolumeTarget(Json::text("not a number")) == 10,
          "invalid volume uses the legacy default");
    check(service::controlVolumeTarget(Json::flag(true)) == 10,
          "boolean volume uses the legacy default");
    check(service::controlVolumeTarget(Json::parse("[47]")) == 47,
          "single-item array uses JavaScript string coercion");
    check(service::controlVolumeTarget(Json::parse("{}")) == 10,
          "object volume uses the legacy default");
    check(service::controlMuteTarget(Json::flag(true)) &&
              service::controlMuteTarget(Json::numeric(1)) &&
              service::controlMuteTarget(Json::text("ON")) &&
              service::controlMuteTarget(Json::text("1")),
          "mute accepts the legacy true values");
    check(!service::controlMuteTarget(Json::text("on")) &&
              !service::controlMuteTarget(Json::numeric(0)) &&
              !service::controlMuteTarget(Json::parse("null")),
          "mute preserves exact string matching and false defaults");
    check(service::controlToastMessage(Json::text("")) == "hello" &&
              service::controlToastMessage(Json::flag(false)) == "hello" &&
              service::controlToastMessage(Json::numeric(0)) == "hello" &&
              service::controlToastMessage(Json::text("hello")) == "hello",
          "toast values preserve the legacy falsy default");
    check(service::controlToastMessage(Json::text(std::string(130, 'x'))) ==
              std::string(120, 'x'),
          "toast text is capped at 120 UTF-16 code units");
    const std::string unicodeToast = std::string(118, 'x') + u8"🎮" + "tail";
    check(service::controlToastMessage(Json::text(unicodeToast)) ==
              std::string(118, 'x') + u8"🎮",
          "toast limit counts supplementary characters as two UTF-16 units");
    check(service::controlServiceMenuTarget(
              Json::parse("{\"menu\":\"inStart\"}")) == "inStart" &&
              service::controlServiceMenuTarget(
                  Json::parse("{\"menu\":\"unsupported\"}")) == "ezAdjust" &&
              service::controlServiceMenuTarget(Json::text("inStart")) == "ezAdjust",
          "service-menu names preserve the legacy two-entry allow-list and default");
    check(service::controlRemoteKeyCode(Json::text(" RIGHT ")) == 106 &&
              service::controlRemoteKeyCode(Json::text("ok")) == 28 &&
              service::controlRemoteKeyCode(Json::text("BACK")) == 412 &&
              service::controlRemoteKeyCode(Json::text("settings")) == 139,
          "RCU navigation names are trimmed and case-folded to LG key codes");
    check(service::controlRemoteKeyCode(Json::text("home")) == 0 &&
              service::controlRemoteKeyCode(Json::text("eject_disc")) == 0 &&
              service::controlRemoteKeyCode(Json::numeric(2)) == 0,
          "Home is kept separate from the network-input navigation keys");
    // Every navigation name and its LG code, as controls.js RCU_KEYS (back is
    // LG's own 412 rather than evdev's 158, which acts as a dismissal).
    {
        const struct { const char *name; int code; } table[] = {
            {"up", 103}, {"down", 108}, {"left", 105}, {"right", 106},
            {"ok", 28}, {"back", 412}, {"settings", 139}};
        for (const auto &entry : table)
            check(service::controlRemoteKeyCode(Json::text(entry.name)) == entry.code,
                  std::string("RCU key ") + entry.name + " keeps its LG code");
    }
    check(service::controlMediaKeyCode(Json::text("play")) == 207 &&
              service::controlMediaKeyCode(Json::text("pause")) == 119 &&
              service::controlMediaKeyCode(Json::text("stop")) == 128 &&
              service::controlMediaKeyCode(Json::text("fastForward")) == 208 &&
              service::controlMediaKeyCode(Json::text("fastforward")) == 208 &&
              service::controlMediaKeyCode(Json::text("rewind")) == 168,
          "media keys keep the RCU_KEY_CODES table");
    check(service::controlMediaKeyCode(Json::text("Play")) == 0 &&
              service::controlMediaKeyCode(Json::text("eject_disc")) == 0 &&
              service::controlMediaKeyCode(Json::numeric(207)) == 0,
          "media key names match exactly and unknown ones have no code");
    check(service::controlRemoteHome(Json::text(" Home ")) &&
              service::controlRemoteHome(Json::array()) == false &&
              !service::controlRemoteHome(Json::text("eject_disc")),
          "Home action matches the legacy string coercion and normalization");
    check(service::controlInputAppId(Json::text("hdmi2")) == "com.webos.app.hdmi2" &&
              service::controlInputAppId(Json::text("livetv")) == "com.webos.app.livetv",
          "input controls map supported input keys to legacy app ids");
    check(service::controlInputAppId(Json::text("hdmi9")).empty() &&
              service::controlInputAppId(Json::numeric(2)).empty(),
          "input controls reject keys outside the legacy input table");
    check(service::controlPictureModeTarget(Json::text("  cinema  ")) == "cinema" &&
              service::controlPictureModeTarget(Json::numeric(3)) == "3" &&
              service::controlPictureModeTarget(Json::flag(false)).empty(),
          "picture mode preserves legacy coercion, trimming and empty values");
    std::string energySaving;
    check(service::controlEnergySavingTarget(Json::text(" MAX "), energySaving) &&
              energySaving == "max" &&
              service::controlEnergySavingTarget(Json::text("screen_off"), energySaving) &&
              !service::controlEnergySavingTarget(Json::text("maximum"), energySaving),
          "energy saving accepts and normalizes only the legacy setting values");
    int backlight = -1;
    check(service::controlBacklightTarget(Json::text(" 40.6percent"), backlight) &&
              backlight == 41 &&
              service::controlBacklightTarget(Json::text("0x10"), backlight) &&
              backlight == 0 &&
              service::controlBacklightTarget(Json::text("1e+"), backlight) &&
              backlight == 1 &&
              service::controlBacklightTarget(Json::numeric(-0.5), backlight) &&
              backlight == 0 &&
              !service::controlBacklightTarget(Json::text("100.5"), backlight) &&
              !service::controlBacklightTarget(Json::text("off"), backlight),
          "backlight uses JavaScript parseFloat, Math.round and zero-to-one-hundred bounds");
    check(service::controlIndicatorLightTarget(Json::flag(true)) &&
              service::controlIndicatorLightTarget(Json::text("on")) &&
              service::controlIndicatorLightTarget(Json::text("ON")) &&
              service::controlIndicatorLightTarget(Json::text("true")) &&
              !service::controlIndicatorLightTarget(Json::text("1")) &&
              !service::controlIndicatorLightTarget(Json::text("True")) &&
              !service::controlIndicatorLightTarget(Json::numeric(1)),
          "indicator lights preserve the legacy exact truth-value set");
    std::string logoDimming;
    check(service::controlLogoDimmingTarget(Json::text(" STRONG "), logoDimming) &&
              logoDimming == "strong" &&
              service::controlLogoDimmingTarget(Json::text("Light"), logoDimming) &&
              logoDimming == "light" &&
              service::controlLogoDimmingTarget(Json::text("off"), logoDimming) &&
              !service::controlLogoDimmingTarget(Json::text("High"), logoDimming) &&
              !service::controlLogoDimmingTarget(Json::parse("null"), logoDimming),
          "logo dimming preserves legacy string coercion and supported setting keys");
    int offStartHour = -1;
    check(service::controlAlwaysReadyOffStartHour(Json::text(" 7:45"), offStartHour) &&
              offStartHour == 7 &&
              service::controlAlwaysReadyOffStartHour(Json::text("23"), offStartHour) &&
              offStartHour == 23 &&
              service::controlAlwaysReadyOffStartHour(Json::text("2tail:00"), offStartHour) &&
              offStartHour == 2 &&
              !service::controlAlwaysReadyOffStartHour(Json::text("25:00"), offStartHour) &&
              !service::controlAlwaysReadyOffStartHour(Json::text("-2:00"), offStartHour) &&
              !service::controlAlwaysReadyOffStartHour(Json::text("bad"), offStartHour),
          "Always-on hours preserve JavaScript parseInt prefix behavior and 0-23 bounds");
    const service::LgSettingWrite genre = service::lgGameSettingWrite(Json::parse(
        "{\"id\":\"gameGenre\",\"value\":\"FPS\"}"));
    check(genre.handled && genre.valid && genre.category == "other" &&
              genre.key == "gameGenre" && genre.storedValue.string() == "FPS",
          "Game Optimizer choices map to LG's category and exact stored value");
    const service::LgSettingWrite badGenre = service::lgGameSettingWrite(Json::parse(
        "{\"id\":\"gameGenre\",\"value\":\"Racing\"}"));
    check(badGenre.handled && !badGenre.valid &&
              badGenre.error == "That value is not one this setting takes",
          "unsupported Game Optimizer choices are rejected before Luna");
    const service::LgSettingWrite stabilizer = service::lgGameSettingWrite(Json::parse(
        "{\"id\":\"blackStabilizer\",\"value\":20}"));
    const service::LgSettingWrite invalidStabilizer = service::lgGameSettingWrite(
        Json::parse("{\"id\":\"blackStabilizer\",\"value\":20.5}"));
    check(stabilizer.handled && stabilizer.valid && stabilizer.storedValue.number() == 20 &&
              invalidStabilizer.handled && !invalidStabilizer.valid,
          "Game Optimizer numeric settings require whole values within LG's range");
    const service::LgSettingWrite enabledSwitch = service::lgGameSettingWrite(Json::parse(
        "{\"id\":\"freesync\",\"value\":true}"));
    const service::LgSettingWrite stringSwitch = service::lgGameSettingWrite(Json::parse(
        "{\"id\":\"freesync\",\"value\":\"true\"}"));
    check(enabledSwitch.valid && enabledSwitch.storedValue.string() == "on" &&
              stringSwitch.valid && stringSwitch.storedValue.string() == "off",
          "LG setting switches treat only Boolean true as enabled");
    const service::LgSettingWrite onPropertyOverridesValue =
        service::lgGameSettingWrite(Json::parse(
            "{\"id\":\"gameGenre\",\"on\":true,\"value\":\"FPS\"}"));
    check(onPropertyOverridesValue.handled && !onPropertyOverridesValue.valid,
          "an explicit on property takes precedence over a choice value like Node");
    const service::LgSettingWrite soundSetting = service::lgGameSettingWrite(Json::parse(
        "{\"id\":\"aigamesound\",\"on\":true}"));
    check(soundSetting.handled && soundSetting.valid && soundSetting.category == "sound" &&
              soundSetting.key == "aigamesound" &&
              soundSetting.storedValue.string() == "on",
          "AI Game Sound writes the sound category and switch value");
    const service::LgSettingWrite otherSection = service::lgGameSettingWrite(Json::parse(
        "{\"id\":\"homePromotion\",\"value\":false}"));
    check(!otherSection.handled,
          "LG settings outside the native Game Optimizer subset remain on Node");
    std::string sleepTimer;
    check(service::controlSleepTimerTarget(Json(), sleepTimer) && sleepTimer == "off" &&
              service::controlSleepTimerTarget(Json::numeric(30), sleepTimer) &&
              sleepTimer == "30" &&
              service::controlSleepTimerTarget(Json::text(" 120 "), sleepTimer) &&
              sleepTimer == "120" &&
              !service::controlSleepTimerTarget(Json::numeric(15), sleepTimer) &&
              !service::controlSleepTimerTarget(Json::text("Off"), sleepTimer),
          "sleep timer preserves null default, trimming and LG's accepted values");
    service::PowerTimerChange timerChange;
    std::string timerError;
    const Json validTimer = Json::parse(
        "{\"timer\":\"off\",\"time\":\"23:30\",\"days\":[0,6]}");
    check(service::controlPowerTimerChange(validTimer, timerChange, timerError) &&
              timerChange.timer == "off" && timerChange.enableKey == "offTimerEnable" &&
              timerChange.settings["offTimerHour"].string() == "23" &&
              timerChange.settings["offTimerMinute"].string() == "30" &&
              timerChange.settings["offTimerWeekday"].string() == "65",
          "timer updates normalize time and build LG's Sunday-first day bitmask");
    check(!service::controlPowerTimerChange(
              Json::parse("{\"timer\":\"on\",\"days\":null}"),
              timerChange, timerError) &&
              timerError == "days must be a list of 0 (Sunday) to 6 (Saturday)",
          "an explicit null day list is rejected instead of treated as omitted");
    check(!service::controlPowerTimerChange(
              Json::parse("{\"timer\":\"on\",\"time\":null}"),
              timerChange, timerError) &&
              timerError == "the time must be HH:MM, from 00:00 to 23:59",
          "an explicit null time is validated as a supplied value");
    check(!service::controlPowerTimerChange(Json::parse("{\"timer\":\"on\"}"),
                                            timerChange, timerError) &&
              timerError == "nothing to change",
          "a timer without changes or an enable request is rejected");
    check(service::controlCloseAppId(Json::text("  com.webos.app.netflix  ")) ==
              "com.webos.app.netflix" &&
              service::controlCloseAppId(Json::numeric(0)).empty(),
          "close-app values use the legacy falsey check and trimming");
    check(service::controlSoundOutputTarget(Json::text("  optical  ")) == "optical" &&
              service::controlSoundOutputTarget(Json::numeric(0)).empty() &&
              service::controlSoundOutputAlias("optical") == "external_optical" &&
              service::controlSoundOutputAlias("external_optical") == "optical" &&
              service::controlSoundOutputAlias("tv_speaker") == "internal" &&
              service::controlSoundOutputAlias("internal") == "tv_speaker" &&
              service::controlSoundOutputAlias("external_arc").empty(),
          "sound output accepts trimmed legacy values and only known firmware aliases");
    std::string launchUrl;
    check(service::controlLaunchUrlTarget(Json::text(" HTTPS://example.com/x "), launchUrl) &&
              launchUrl == "HTTPS://example.com/x" &&
              !service::controlLaunchUrlTarget(Json::text("file:///etc/passwd"), launchUrl) &&
              !service::controlLaunchUrlTarget(Json::text("http://bad host"), launchUrl),
          "launch URLs preserve case while restricting schemes and whitespace");
    check(service::volumeScenarioUnsupported(Json::parse(
              "{\"errorText\":\"Current Scenario doesn't support volume\"}")),
          "unsupported output scenario is detected without case sensitivity");
    check(!service::volumeScenarioUnsupported(Json::parse(
              "{\"errorText\":\"permission denied\"}")),
          "other Luna refusals are not classified as unsupported scenarios");
    if (failures)
        return 1;
    std::cout << "native control command checks passed\n";
    return 0;
}
