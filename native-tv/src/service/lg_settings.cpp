#include "service/lg_settings.hpp"
#include "domain/names.hpp"
#include "domain/say.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace nativetv {
namespace service {
namespace {
enum class Kind { Switch, Choice, Number };

struct Choice {
    const char *value;
    const char *labelKey;
    const char *label;
};

struct Definition {
    const char *id;
    const char *category;
    const char *key;
    Kind kind;
    double minimum;
    double maximum;
    const char *titleKey;
    const char *title;
    const char *descriptionKey;
    const char *description;
    const Choice *choices;
    size_t choiceCount;

    Definition(const char *idValue, const char *categoryValue, const char *keyValue,
               Kind kindValue, double minimumValue, double maximumValue,
               const char *titleKeyValue, const char *titleValue,
               const char *descriptionKeyValue, const char *descriptionValue,
               const Choice *choicesValue, size_t choiceCountValue)
        : id(idValue), category(categoryValue), key(keyValue), kind(kindValue),
          minimum(minimumValue), maximum(maximumValue), titleKey(titleKeyValue),
          title(titleValue), descriptionKey(descriptionKeyValue),
          description(descriptionValue), choices(choicesValue),
          choiceCount(choiceCountValue) {}
};

struct DashboardRow {
    const char *id;
    const char *section;
    const char *category;
    const char *key;
    Kind kind;
    double minimum;
    double maximum;
    const char *titleKey;
    const char *title;
    const char *descriptionKey;
    const char *description;
    const Choice *choices;
    size_t choiceCount;
    bool greyable;
    unsigned port;
    const char *column;
    const char *unless;
    bool storeAsString;

    DashboardRow(const char *idValue, const char *sectionValue,
                 const char *categoryValue, const char *keyValue, Kind kindValue,
                 double minimumValue, double maximumValue,
                 const char *titleKeyValue, const char *titleValue,
                 const char *descriptionKeyValue, const char *descriptionValue,
                 const Choice *choicesValue = nullptr, size_t choiceCountValue = 0,
                 bool greyableValue = false, unsigned portValue = 0,
                 const char *columnValue = nullptr, const char *unlessValue = nullptr,
                 bool storeAsStringValue = false)
        : id(idValue), section(sectionValue), category(categoryValue), key(keyValue),
          kind(kindValue), minimum(minimumValue), maximum(maximumValue),
          titleKey(titleKeyValue), title(titleValue),
          descriptionKey(descriptionKeyValue), description(descriptionValue),
          choices(choicesValue), choiceCount(choiceCountValue),
          greyable(greyableValue), port(portValue), column(columnValue),
          unless(unlessValue), storeAsString(storeAsStringValue) {}
};

const Choice kGameGenreChoices[] = {
    {"Standard", "srv.lgs.gameGenre.standard", "Standard"},
    {"FPS", "srv.lgs.gameGenre.fps", "First-person shooter"},
    {"RPG", "srv.lgs.gameGenre.rpg", "Role-playing"},
    {"RTS", "srv.lgs.gameGenre.rts", "Real-time strategy"},
    {"Sports", "srv.lgs.gameGenre.sports", "Sports"},
    {"USER", "srv.lgs.gameGenre.user", "User"},
};
const Choice kInputOptimizationChoices[] = {
    {"auto", "srv.lgs.inputOptimization.auto", "Standard"},
    {"on", "srv.lgs.inputOptimization.on", "Boost"},
};
const Choice kDarkModeChoices[] = {
    {"off", "srv.lgs.darkMode.off", "Off"},
    {"level1", "srv.lgs.darkMode.level1", "Level 1"},
    {"level2", "srv.lgs.darkMode.level2", "Level 2"},
};
const Choice kSoundOutputChoices[] = {
    {"tv_speaker", "srv.lgs.soundOutput.tvSpeaker", "TV Speaker"},
    {"external_arc", "srv.lgs.soundOutput.externalArc", "HDMI ARC"},
    {"external_optical", "srv.lgs.soundOutput.externalOptical", "Optical"},
    {"bt_soundbar", "srv.lgs.soundOutput.btSoundbar", "Bluetooth"},
    {"headphone", "srv.lgs.soundOutput.headphone", "Headphone / AUX"},
    {"tv_speaker_external_arc", "srv.lgs.soundOutput.tvSpeakerExternalArc",
     "TV Speaker + HDMI ARC"},
    {"tv_speaker_bluetooth", "srv.lgs.soundOutput.tvSpeakerBluetooth",
     "TV Speaker + Bluetooth"},
    {"tv_speaker_headphone", "srv.lgs.soundOutput.tvSpeakerHeadphone",
     "TV Speaker + Headphone"},
};
const Choice kSoundModeChoices[] = {
    {"aiSoundPlus", "srv.lgs.soundMode.aiSoundPlus", "AI Sound Pro"},
    {"aiSound", "srv.lgs.soundMode.aiSound", "AI Sound"},
    {"standard", "srv.lgs.soundMode.standard", "Standard"},
    {"news", "srv.lgs.soundMode.news", "Clear Voice"},
    {"movie", "srv.lgs.soundMode.movie", "Cinema"},
    {"sports", "srv.lgs.soundMode.sports", "Sports"},
    {"music", "srv.lgs.soundMode.music", "Music"},
    {"game", "srv.lgs.soundMode.game", "Game Optimizer"},
};
const Choice kDigitalOutputChoices[] = {
    {"auto", "srv.lgs.soundOutputDigital.auto", "Auto"},
    {"pcm", "srv.lgs.soundOutputDigital.pcm", "PCM"},
    {"passThrough", "srv.lgs.soundOutputDigital.passThrough", "Pass Through"},
};
const Choice kDeepColorChoices[] = {
    {"off", "srv.lgs.deepColor.off", "Off"},
    {"on", "srv.lgs.deepColor.on", "On"},
    {"4k", "", "4K"},
    {"8k", "", "8K"},
    {"auto", "srv.lgs.deepColor.auto", "Auto"},
};
const Choice kAudioFormatChoices[] = {
    {"bitstream", "srv.lgs.audioFormat.bitstream", "Bitstream"},
    {"pcm", "srv.lgs.audioFormat.pcm", "PCM"},
};

const DashboardRow kDashboardRows[] = {
    {"screenSaverAd", "promotions", "general", "screenSaverAd", Kind::Switch, 0, 0,
     "srv.lgs.screenSaverAd", "Screen saver ads", "srv.lgs.screenSaverAd.desc",
     "Adverts that some apps, such as LG Channels, show in the screen saver. LG calls this Screen Saver Promotion."},
    {"homePromotion", "promotions", "general", "homePromotion", Kind::Switch, 0, 0,
     "srv.lgs.homePromotion", "Sponsored tiles on Home", "srv.lgs.homePromotion.desc",
     "Adverts marked Sponsored on the Home screen. LG calls this Home Promotion."},
    {"contentRecommendation", "promotions", "other", "contentRecommendation", Kind::Switch, 0, 0,
     "srv.lgs.contentRecommendation", "Recommendations on Home",
     "srv.lgs.contentRecommendation.desc",
     "Rows of recommended programmes and films on the Home screen. LG calls this Content Recommendation."},
    {"livePromotion", "promotions", "option", "livePromotion", Kind::Switch, 0, 0,
     "srv.lgs.livePromotion", "Ads while watching", "srv.lgs.livePromotion.desc",
     "Adverts and offers shown over what is on screen, while LG's Live Plus is on. LG calls this Live Promotion."},
    {"aiNudge", "promotions", "general", "aiNudge", Kind::Switch, 0, 0,
     "srv.lgs.aiNudge", "Smart tips", "srv.lgs.aiNudge.desc",
     "Suggestions for TV features that pop up while watching or using an app. LG calls this Smart Tips."},
    {"aiSettingsNudge", "promotions", "general", "aiSettingsNudge", Kind::Switch, 0, 0,
     "srv.lgs.aiSettingsNudge", "Smart tips in Settings",
     "srv.lgs.aiSettingsNudge.desc",
     "Suggested settings shown in LG's Settings menu. LG calls this Smart Tips in Settings."},

    {"soundOutput", "sound", "sound", "soundOutput", Kind::Choice, 0, 0,
     "srv.lgs.soundOutput", "Sound output", "srv.lgs.soundOutput.desc",
     "Where the TV plays its sound. Sound mode and balance apply only to the TV speakers.",
     kSoundOutputChoices, sizeof(kSoundOutputChoices) / sizeof(kSoundOutputChoices[0])},
    {"soundMode", "sound", "sound", "soundMode", Kind::Choice, 0, 0,
     "srv.lgs.soundMode", "Sound mode", "srv.lgs.soundMode.desc",
     "How the TV tunes its sound, through its own speakers only. AI Sound Pro adjusts it to what is on by itself.",
     kSoundModeChoices, sizeof(kSoundModeChoices) / sizeof(kSoundModeChoices[0])},
    {"soundOutputDigital", "sound", "sound", "soundOutputDigital", Kind::Choice, 0, 0,
     "srv.lgs.soundOutputDigital", "Digital sound output",
     "srv.lgs.soundOutputDigital.desc",
     "The audio sent to a soundbar or receiver over HDMI ARC or optical. Pass Through sends Dolby and DTS as they are; PCM turns them into plain stereo.",
     kDigitalOutputChoices,
     sizeof(kDigitalOutputChoices) / sizeof(kDigitalOutputChoices[0])},
    {"audioBalance", "sound", "sound", "audioBalance", Kind::Number, -50, 50,
     "srv.lgs.audioBalance", "Balance", "srv.lgs.audioBalance.desc",
     "Between the left and right TV speakers, and only those. At 0 both play at the same level.",
     nullptr, 0, false, 0, nullptr, nullptr, true},
    {"autoVolume", "sound", "sound", "autoVolume", Kind::Switch, 0, 0,
     "srv.lgs.autoVolume", "Automatic volume", "srv.lgs.autoVolume.desc",
     "Keeps the volume level when switching channels. LG calls this Automatic Volume Adjustment."},
    {"btSpeakerMode", "sound", "general", "blueToothSpeakerMode", Kind::Switch, 0, 0,
     "srv.lgs.btSpeakerMode", "Bluetooth speaker mode", "srv.lgs.btSpeakerMode.desc",
     "Lets phones in Bluetooth range connect and play music through the TV, even while it is switched off. LG calls this Bluetooth Speaker Mode."},
    {"eArcSupport", "sound", "sound", "eArcSupport", Kind::Switch, 0, 0,
     "srv.lgs.eArcSupport", "eARC", "srv.lgs.eArcSupport.desc",
     "Sends lossless and Dolby Atmos audio to a soundbar or receiver on the HDMI (eARC) input, where both support it."},

    {"deepColor", "hdmi", "other", "uhdDeepColor", Kind::Choice, 0, 0,
     "srv.lgs.deepColor", "HDMI Deep Colour", "srv.lgs.deepColor.desc",
     "For the input on screen. Lets it take 4K HDR and high frame rates. The picture can drop out for a moment while the TV and the device agree the change, and turning it off also turns off Game Optimizer for that input.",
     kDeepColorChoices, sizeof(kDeepColorChoices) / sizeof(kDeepColorChoices[0]),
     true, 0, "deepColor"},
    {"deepColorHDMI1", "hdmi", "other", "uhdDeepColorHDMI1", Kind::Switch, 0, 0,
     "srv.lgs.deepColorHdmi1", "HDMI 1 Deep Colour", "srv.lgs.deepColorHdmi.desc",
     "Lets the input take 4K HDR and high frame rates. The picture can drop out for a moment while the TV and the device agree the change.",
     nullptr, 0, true, 1, "deepColor", "uhdDeepColor"},
    {"deepColorHDMI2", "hdmi", "other", "uhdDeepColorHDMI2", Kind::Switch, 0, 0,
     "srv.lgs.deepColorHdmi2", "HDMI 2 Deep Colour", "srv.lgs.deepColorHdmi.desc",
     "Lets the input take 4K HDR and high frame rates. The picture can drop out for a moment while the TV and the device agree the change.",
     nullptr, 0, true, 2, "deepColor", "uhdDeepColor"},
    {"deepColorHDMI3", "hdmi", "other", "uhdDeepColorHDMI3", Kind::Switch, 0, 0,
     "srv.lgs.deepColorHdmi3", "HDMI 3 Deep Colour", "srv.lgs.deepColorHdmi.desc",
     "Lets the input take 4K HDR and high frame rates. The picture can drop out for a moment while the TV and the device agree the change.",
     nullptr, 0, true, 3, "deepColor", "uhdDeepColor"},
    {"deepColorHDMI4", "hdmi", "other", "uhdDeepColorHDMI4", Kind::Switch, 0, 0,
     "srv.lgs.deepColorHdmi4", "HDMI 4 Deep Colour", "srv.lgs.deepColorHdmi.desc",
     "Lets the input take 4K HDR and high frame rates. The picture can drop out for a moment while the TV and the device agree the change.",
     nullptr, 0, true, 4, "deepColor", "uhdDeepColor"},
    {"audioFormatHDMI1", "hdmi", "sound", "inputAudioFormatHDMI1", Kind::Choice, 0, 0,
     "srv.lgs.audioFormatHdmi1", "HDMI 1 audio format",
     "srv.lgs.audioFormatHdmi.desc",
     "What the TV asks the device on this input to send. Bitstream passes Dolby Atmos and DTS on to a soundbar or receiver; PCM asks for plain audio.",
     kAudioFormatChoices, sizeof(kAudioFormatChoices) / sizeof(kAudioFormatChoices[0]),
     true, 1, "audioFormat"},
    {"audioFormatHDMI2", "hdmi", "sound", "inputAudioFormatHDMI2", Kind::Choice, 0, 0,
     "srv.lgs.audioFormatHdmi2", "HDMI 2 audio format",
     "srv.lgs.audioFormatHdmi.desc",
     "What the TV asks the device on this input to send. Bitstream passes Dolby Atmos and DTS on to a soundbar or receiver; PCM asks for plain audio.",
     kAudioFormatChoices, sizeof(kAudioFormatChoices) / sizeof(kAudioFormatChoices[0]),
     true, 2, "audioFormat"},
    {"audioFormatHDMI3", "hdmi", "sound", "inputAudioFormatHDMI3", Kind::Choice, 0, 0,
     "srv.lgs.audioFormatHdmi3", "HDMI 3 audio format",
     "srv.lgs.audioFormatHdmi.desc",
     "What the TV asks the device on this input to send. Bitstream passes Dolby Atmos and DTS on to a soundbar or receiver; PCM asks for plain audio.",
     kAudioFormatChoices, sizeof(kAudioFormatChoices) / sizeof(kAudioFormatChoices[0]),
     true, 3, "audioFormat"},
    {"audioFormatHDMI4", "hdmi", "sound", "inputAudioFormatHDMI4", Kind::Choice, 0, 0,
     "srv.lgs.audioFormatHdmi4", "HDMI 4 audio format",
     "srv.lgs.audioFormatHdmi.desc",
     "What the TV asks the device on this input to send. Bitstream passes Dolby Atmos and DTS on to a soundbar or receiver; PCM asks for plain audio.",
     kAudioFormatChoices, sizeof(kAudioFormatChoices) / sizeof(kAudioFormatChoices[0]),
     true, 4, "audioFormat"},

    {"simplinkEnable", "devices", "other", "simplinkEnable", Kind::Switch, 0, 0,
     "srv.lgs.simplinkEnable", "SIMPLINK (HDMI-CEC)", "srv.lgs.simplinkEnable.desc",
     "Lets the TV remote control devices connected over HDMI, such as a soundbar or console."},
    {"simplinkAutoPowerOn", "devices", "other", "simplinkAutoPowerOn", Kind::Switch, 0, 0,
     "srv.lgs.simplinkAutoPowerOn", "Auto power sync",
     "srv.lgs.simplinkAutoPowerOn.desc",
     "Switching the TV off switches connected devices off, and switching a device on switches the TV on."},
    {"ipControl", "devices", "option", "enableIpControl", Kind::Switch, 0, 0,
     "srv.lgs.ipControl", "IP control", "srv.lgs.ipControl.desc",
     "Lets home automation systems control the TV over the network with LG's own protocol. Off unless something needs it."},
};

const Definition kGameSettings[] = {
    {"gameGenre", "other", "gameGenre", Kind::Choice, 0, 0,
     "srv.lgs.gameGenre", "Game genre", "srv.lgs.gameGenre.desc",
     "Tunes the picture for the kind of game. Each genre keeps its own stabilizer settings.",
     kGameGenreChoices, sizeof(kGameGenreChoices) / sizeof(kGameGenreChoices[0])},
    {"inputOptimization", "other", "inputOptimization", Kind::Choice, 0, 0,
     "srv.lgs.inputOptimization", "Prevent input delay",
     "srv.lgs.inputOptimization.desc",
     "Standard cuts input lag for the kind of content; Boost cuts it further by matching the console's frame rate.",
     kInputOptimizationChoices,
     sizeof(kInputOptimizationChoices) / sizeof(kInputOptimizationChoices[0])},
    {"gameOptimization", "other", "gameOptimization", Kind::Switch, 0, 0,
     "srv.lgs.gameOptimization", "VRR & G-Sync",
     "srv.lgs.gameOptimization.desc",
     "Lets the TV follow the frame rate a console or PC sends, so motion stays smooth without tearing.",
     nullptr, 0},
    {"freesync", "other", "freesync", Kind::Switch, 0, 0,
     "srv.lgs.freesync", "AMD FreeSync Premium", "srv.lgs.freesync.desc",
     "The same for AMD graphics cards and consoles that use FreeSync.", nullptr, 0},
    {"enableALLM", "other", "enableALLM", Kind::Switch, 0, 0,
     "srv.lgs.enableALLM", "ALLM", "srv.lgs.enableALLM.desc",
     "Switches the TV to its low-latency game mode by itself when a connected device starts a game.",
     nullptr, 0},
    {"darkMode", "other", "darkMode", Kind::Choice, 0, 0,
     "srv.lgs.darkMode", "Dark room mode", "srv.lgs.darkMode.desc",
     "Lowers the brightness for playing in a dark room.", kDarkModeChoices,
     sizeof(kDarkModeChoices) / sizeof(kDarkModeChoices[0])},
    {"blackStabilizer", "other", "blackStabilizer", Kind::Number, 0, 20,
     "srv.lgs.blackStabilizer", "Black stabilizer",
     "srv.lgs.blackStabilizer.desc",
     "Lifts dark areas so detail in shadows is easier to see.", nullptr, 0},
    {"whiteStabilizer", "other", "whiteStabilizer", Kind::Number, 0, 20,
     "srv.lgs.whiteStabilizer", "White stabilizer",
     "srv.lgs.whiteStabilizer.desc",
     "Tames bright areas so detail in highlights is easier to see.", nullptr, 0},
    {"aigamesound", "sound", "aigamesound", Kind::Switch, 0, 0,
     "srv.lgs.aigamesound", "AI Game Sound", "srv.lgs.aigamesound.desc",
     "Sets the sound for the game being played: effects, clear voices and surround.",
     nullptr, 0},
};

const Definition *definitionFor(const std::string &id) {
    for (const Definition &definition : kGameSettings)
        if (id == definition.id)
            return &definition;
    return nullptr;
}

const DashboardRow *dashboardDefinitionFor(const std::string &id) {
    for (const DashboardRow &definition : kDashboardRows)
        if (id == definition.id)
            return &definition;
    return nullptr;
}

bool choiceValue(const Definition &definition, const std::string &value) {
    for (size_t index = 0; index < definition.choiceCount; ++index)
        if (value == definition.choices[index].value)
            return true;
    return false;
}

bool sameValue(const Json &left, const Json &right) {
    if (left.isString() && right.isString())
        return left.string() == right.string();
    if (left.isNumber() && right.isNumber())
        return left.number() == right.number();
    if (left.isBoolean() && right.isBoolean())
        return left.boolean() == right.boolean();
    return !left.valid() && !right.valid();
}

// A whole-category reply lists the value the settings service stores for each key in `settings`
// and what the TV uses now in `dimension`. They differ for a key kept once per dimension: a B3
// playing through its speaker stores soundOutput "external_arc" (the last HDMI ARC pick) and its
// dimension says "tv_speaker", which is also what a request for that key alone returns.
// A copy, since the daemon's cache holds the originals.
Json settingsInEffect(const Json &settingsByCategory, const Json &dimensionsByCategory) {
    Json result = Json::object();
    for (const std::string &category : settingsByCategory.keys()) {
        const Json stored = settingsByCategory[category];
        const Json dimension = dimensionsByCategory[category];
        if (!stored.isObject() || !dimension.isObject()) {
            result.set(category, stored);
            continue;
        }
        Json settings = Json::object();
        for (const std::string &key : stored.keys())
            settings.set(key, dimension.has(key) ? dimension[key] : stored[key]);
        result.set(category, settings);
    }
    return result;
}

// The TV client sends a number as a decimal (11.0); every writer here has checked that it is
// whole, and it is kept as 11 so the TV's settings and each screen reading them show 11.
Json wholeNumber(const Json &value) {
    return Json::parse(std::to_string(static_cast<long long>(value.number())));
}

bool jsTruthy(const Json &value) {
    if (!value.valid())
        return false;
    if (value.isBoolean())
        return value.boolean();
    if (value.isNumber())
        return value.number() != 0;
    if (value.isString())
        return !value.string().empty();
    return true;
}

std::string jsString(const Json &value) {
    if (!value.valid())
        return "null";
    if (value.isString())
        return value.string();
    if (value.isBoolean())
        return value.boolean() ? "true" : "false";
    if (value.isNumber())
        return compactNumber(value.number(), 15);
    if (value.isArray()) {
        const std::vector<Json> items = value.items();
        std::string result;
        for (size_t index = 0; index < items.size(); ++index) {
            if (index)
                result += ',';
            if (items[index].valid())
                result += jsString(items[index]);
        }
        return result;
    }
    return "[object Object]";
}

Json jsNumber(const Json &value) {
    if (!value.valid())
        return Json::numeric(0);
    if (value.isNumber())
        return value;
    if (value.isBoolean())
        return Json::numeric(value.boolean() ? 1 : 0);
    if (!value.isString())
        return Json();
    const std::string raw = value.string();
    const char *start = raw.c_str();
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n' ||
           *start == '\f' || *start == '\v')
        ++start;
    if (!*start)
        return Json::numeric(0);
    char *end = nullptr;
    const double number = std::strtod(start, &end);
    while (end && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n' ||
                   *end == '\f' || *end == '\v'))
        ++end;
    return end != start && end && !*end && std::isfinite(number)
               ? Json::numeric(number) : Json();
}

Json descriptionFor(const Json &descriptions, const std::string &key) {
    if (!descriptions.isArray())
        return Json();
    for (const Json &item : descriptions.items())
        if (item["key"].string() == key)
            return item;
    return Json();
}

Json choiceRows(const Choice *choices, size_t choiceCount, const Json &value,
                const Json &descriptionValues) {
    Json result = Json::array();
    const Json visibleChoices = descriptionValues["arrayExt"];
    for (size_t index = 0; index < choiceCount; ++index) {
        const Choice &choice = choices[index];
        bool visible = !visibleChoices.valid();
        if (visibleChoices.isArray()) {
            for (const Json &candidate : visibleChoices.items()) {
                if (sameValue(candidate["value"], Json::text(choice.value)) &&
                    !(candidate["visible"].isBoolean() &&
                      !candidate["visible"].boolean())) {
                    visible = true;
                    break;
                }
            }
        }
        if (!visible)
            continue;
        Json row = Json::object();
        row.set("value", Json::text(choice.value));
        row.set("label", Json::text(say::msg(choice.labelKey, choice.label)));
        result.add(row);
    }

    bool known = false;
    for (const Json &choice : result.items())
        if (sameValue(choice["value"], value)) {
            known = true;
            break;
        }
    if (!known) {
        Json row = Json::object();
        row.set("value", value);
        row.set("label", Json::text(jsString(value)));
        result.add(row);
    }
    return result;
}

Json dashboardRow(const DashboardRow &definition, const Json &settings,
                  const Json &descriptions) {
    Json row = Json::object();
    row.set("id", Json::text(definition.id));
    row.set("section", Json::text(definition.section));
    row.set("type", Json::text(definition.kind == Kind::Switch ? "switch" :
                                definition.kind == Kind::Choice ? "choice" : "number"));
    row.set("title", Json::text(say::msg(definition.titleKey, definition.title)));
    row.set("desc", Json::text(say::msg(definition.descriptionKey,
                                        definition.description)));
    if (definition.port)
        row.set("port", Json::numeric(definition.port));
    if (definition.column)
        row.set("column", Json::text(definition.column));

    const Json value = settings[definition.key];
    const Json description = descriptionFor(descriptions, definition.key);
    if (definition.greyable && description.valid() &&
        description["ui"]["active"].isBoolean() &&
        !description["ui"]["active"].boolean())
        row.set("active", Json::flag(false));

    if (definition.kind == Kind::Switch) {
        row.set("on", Json::flag(value.isString() && value.string() == "on"));
    } else if (definition.storeAsString) {
        row.set("value", jsNumber(value));
    } else {
        row.set("value", value);
    }

    const Json descriptionValues = description["values"];
    if (definition.kind == Kind::Choice) {
        row.set("choices", choiceRows(definition.choices, definition.choiceCount,
                                       value, descriptionValues));
    } else if (definition.kind == Kind::Number) {
        const Json minimum = descriptionValues["min"];
        const Json maximum = descriptionValues["max"];
        row.set("min", minimum.isNumber() ? minimum : Json::numeric(definition.minimum));
        row.set("max", maximum.isNumber() ? maximum : Json::numeric(definition.maximum));
    }
    return row;
}

bool sectionSelected(const std::vector<std::string> &sections, const char *section) {
    return std::find(sections.begin(), sections.end(), section) != sections.end();
}

void addUnique(std::vector<std::string> &values, const char *value) {
    if (std::find(values.begin(), values.end(), value) == values.end())
        values.push_back(value);
}

Json gameRows(const Json &settingsByCategory, const Json &descriptionsByCategory) {
    Json rows = Json::array();
    for (const Definition &definition : kGameSettings) {
        const Json settings = settingsByCategory[definition.category];
        if (!settings.has(definition.key))
            continue;
        const Json value = settings[definition.key];
        const Json description = descriptionFor(
            descriptionsByCategory[definition.category], definition.key);
        const Json descriptionValues = description["values"];
        Json row = Json::object();
        row.set("id", Json::text(definition.id));
        row.set("section", Json::text("game"));
        row.set("type", Json::text(definition.kind == Kind::Switch ? "switch" :
                                   definition.kind == Kind::Choice ? "choice" : "number"));
        row.set("title", Json::text(say::msg(definition.titleKey, definition.title)));
        row.set("desc", Json::text(say::msg(definition.descriptionKey,
                                            definition.description)));
        if (definition.kind == Kind::Switch) {
            row.set("on", Json::flag(value.isString() && value.string() == "on"));
        } else {
            row.set("value", value);
        }
        if (definition.kind == Kind::Choice) {
            row.set("choices", choiceRows(definition.choices,
                                           definition.choiceCount, value,
                                           descriptionValues));
        } else if (definition.kind == Kind::Number) {
            const Json minimum = descriptionValues["min"];
            const Json maximum = descriptionValues["max"];
            row.set("min", minimum.isNumber() ? minimum : Json::numeric(definition.minimum));
            row.set("max", maximum.isNumber() ? maximum : Json::numeric(definition.maximum));
        }
        rows.add(row);
    }
    return rows;
}
} // namespace

LgSettingWrite lgGameSettingWrite(const Json &value) {
    LgSettingWrite write;
    if (!value.isObject() || !value["id"].isString())
        return write;

    write.id = value["id"].string();
    const Definition *definition = definitionFor(write.id);
    if (!definition)
        return LgSettingWrite();

    write.handled = true;
    write.category = definition->category;
    write.key = definition->key;
    write.error = "That value is not one this setting takes";
    if (definition->kind == Kind::Switch) {
        const Json flag = value.has("on") ? value["on"] : value["value"];
        write.storedValue = Json::text(flag.isBoolean() && flag.boolean() ? "on" : "off");
        write.valid = true;
        write.error.clear();
        return write;
    }
    const Json selected = value.has("on")
        ? Json::flag(value["on"].isBoolean() && value["on"].boolean())
        : value["value"];
    if (definition->kind == Kind::Choice) {
        if (selected.isString() && choiceValue(*definition, selected.string())) {
            write.storedValue = selected;
            write.valid = true;
            write.error.clear();
        }
        return write;
    }

    if (selected.isNumber() && std::isfinite(selected.number()) &&
        std::floor(selected.number()) == selected.number() &&
        selected.number() >= definition->minimum &&
        selected.number() <= definition->maximum) {
        write.storedValue = wholeNumber(selected);
        write.valid = true;
        write.error.clear();
    }
    return write;
}

LgSettingWrite lgSettingWrite(const Json &value) {
    LgSettingWrite game = lgGameSettingWrite(value);
    if (game.handled)
        return game;
    if (!value.isObject() || !value["id"].isString())
        return LgSettingWrite();
    const DashboardRow *row = dashboardDefinitionFor(value["id"].string());
    if (!row)
        return LgSettingWrite();

    LgSettingWrite write;
    write.handled = true;
    write.id = row->id;
    write.category = row->category;
    write.key = row->key;
    write.greyable = row->greyable;
    write.error = "That value is not one this setting takes";
    // A switch takes `on`; any other row takes `value`.
    const Json selected = value.has("on") ? Json::flag(value["on"].isBoolean() && value["on"].boolean())
                                          : value["value"];
    if (row->kind == Kind::Switch) {
        write.storedValue = Json::text(selected.isBoolean() && selected.boolean() ? "on" : "off");
    } else if (row->kind == Kind::Choice) {
        bool known = false;
        if (selected.isString())
            for (size_t index = 0; index < row->choiceCount; ++index)
                known = known || selected.string() == row->choices[index].value;
        if (!known)
            return write;
        write.storedValue = selected;
    } else {
        if (!selected.isNumber() || !std::isfinite(selected.number()) ||
            std::floor(selected.number()) != selected.number() ||
            selected.number() < row->minimum || selected.number() > row->maximum)
            return write;
        write.storedValue = row->storeAsString
            ? Json::text(std::to_string(static_cast<int>(selected.number()))) : wholeNumber(selected);
    }
    write.valid = true;
    write.error.clear();
    return write;
}

LgSettingWrite lgSettingsRestoreWrite(const std::string &id,
                                     const Json &value,
                                     const Json &currentRow) {
    LgSettingWrite write;
    write.id = id;

    const DashboardRow *dashboard = dashboardDefinitionFor(id);
    const Definition *game = dashboard ? nullptr : definitionFor(id);
    if (!dashboard && !game)
        return write;

    write.handled = true;
    write.category = dashboard ? dashboard->category : game->category;
    write.key = dashboard ? dashboard->key : game->key;
    write.error = "That value is not one this setting takes";
    if (!currentRow.isObject())
        return write;

    const std::string type = dashboard
        ? (dashboard->kind == Kind::Switch ? "switch" :
           dashboard->kind == Kind::Choice ? "choice" : "number")
        : (game->kind == Kind::Switch ? "switch" :
           game->kind == Kind::Choice ? "choice" : "number");
    if (type == "switch") {
        if (!value.isBoolean())
            return write;
        write.storedValue = Json::text(value.boolean() ? "on" : "off");
        write.valid = true;
        write.error.clear();
        return write;
    }
    if (type == "choice") {
        if (!value.isString() || !currentRow["choices"].isArray())
            return write;
        for (const Json &choice : currentRow["choices"].items()) {
            if (!sameValue(choice["value"], value))
                continue;
            write.storedValue = value;
            write.valid = true;
            write.error.clear();
            return write;
        }
        return write;
    }

    const double defaultMinimum = dashboard ? dashboard->minimum : game->minimum;
    const double defaultMaximum = dashboard ? dashboard->maximum : game->maximum;
    const double minimum = currentRow["min"].isNumber()
        ? currentRow["min"].number() : defaultMinimum;
    const double maximum = currentRow["max"].isNumber()
        ? currentRow["max"].number() : defaultMaximum;
    if (!value.isNumber() || !std::isfinite(value.number()) ||
        std::floor(value.number()) != value.number() ||
        value.number() < minimum || value.number() > maximum)
        return write;

    const bool storeAsString = dashboard && dashboard->storeAsString;
    write.storedValue = storeAsString
        ? Json::text(std::to_string(static_cast<int>(value.number()))) : wholeNumber(value);
    write.valid = true;
    write.error.clear();
    return write;
}

std::vector<std::string> lgGameSettingCategories() {
    std::vector<std::string> categories;
    for (const Definition &definition : kGameSettings) {
        if (std::find(categories.begin(), categories.end(), definition.category) ==
            categories.end())
            categories.push_back(definition.category);
    }
    return categories;
}

std::vector<std::string> lgGameSettingKeys(const std::string &category) {
    std::vector<std::string> keys;
    for (const Definition &definition : kGameSettings)
        if (category == definition.category)
            keys.push_back(definition.key);
    return keys;
}

std::vector<std::string> lgSettingsCategories(
    const std::vector<std::string> &sections) {
    std::vector<std::string> categories;
    for (const DashboardRow &row : kDashboardRows)
        if (std::string(row.section) == "promotions" &&
            sectionSelected(sections, row.section))
            addUnique(categories, row.category);
    if (sectionSelected(sections, "game"))
        for (const Definition &definition : kGameSettings)
            addUnique(categories, definition.category);
    for (const DashboardRow &row : kDashboardRows)
        if (std::string(row.section) != "promotions" &&
            sectionSelected(sections, row.section))
            addUnique(categories, row.category);
    return categories;
}

std::vector<std::string> lgSettingsKeys(
    const std::string &category, const std::vector<std::string> &sections) {
    std::vector<std::string> keys;
    for (const DashboardRow &row : kDashboardRows)
        if (std::string(row.section) == "promotions" &&
            sectionSelected(sections, row.section) && category == row.category)
            keys.push_back(row.key);
    if (sectionSelected(sections, "game"))
        for (const Definition &definition : kGameSettings)
            if (category == definition.category)
                keys.push_back(definition.key);
    for (const DashboardRow &row : kDashboardRows)
        if (std::string(row.section) != "promotions" &&
            sectionSelected(sections, row.section) && category == row.category)
            keys.push_back(row.key);
    return keys;
}

Json lgSettingsResponse(const std::vector<std::string> &sections,
                        const Json &settingsByCategory,
                        const Json &descriptionsByCategory,
                        const Json &dimensionsByCategory,
                        const Json &inputs) {
    const Json settingsNow = settingsInEffect(settingsByCategory, dimensionsByCategory);
    Json rows = Json::array();
    const auto appendDashboardRows = [&](bool promotions) {
        for (const DashboardRow &definition : kDashboardRows) {
            if ((std::string(definition.section) == "promotions") != promotions ||
                !sectionSelected(sections, definition.section))
                continue;
            const Json settings = settingsNow[definition.category];
            if (!settings.has(definition.key) ||
                (definition.unless && settings.has(definition.unless)))
                continue;
            rows.add(dashboardRow(definition, settings,
                                  descriptionsByCategory[definition.category]));
        }
    };
    appendDashboardRows(true);

    if (sectionSelected(sections, "game")) {
        const Json game = gameRows(settingsNow, descriptionsByCategory);
        for (const Json &row : game.items())
            rows.add(row);
    }
    appendDashboardRows(false);

    Json response = Json::object();
    response.set("ok", Json::flag(true));
    response.set("rows", rows);
    response.set("dimensions", dimensionsByCategory.isObject()
                                   ? dimensionsByCategory : Json::object());
    response.set("inputs", inputs.isObject() ? inputs : Json::object());
    return response;
}

Json lgSettingsInputNames(const Json &devices) {
    Json inputs = Json::object();
    if (!devices.isArray())
        return inputs;
    for (const Json &device : devices.items()) {
        const std::string id = device["id"].string();
        const Json key = names::hdmiKeyForPortId(id);
        if (!key.isString())
            continue;
        std::string label = device["label"].string();
        if (label.empty())
            label = names::input(key.string())["display"].string();
        inputs.set(key.string(), Json::text(label));
    }
    return inputs;
}

Json lgGameResponse(const Json &settingsByCategory,
                    const Json &descriptionsByCategory,
                    const Json &dimensionsByCategory,
                    const Json &pictureSettings) {
    const Json rows = gameRows(settingsInEffect(settingsByCategory, dimensionsByCategory),
                               descriptionsByCategory);
    Json response = Json::object();
    response.set("ok", Json::flag(true));
    if (!rows.size()) {
        response.set("available", Json::flag(false));
        return response;
    }

    response.set("available", Json::flag(true));
    response.set("rows", rows);
    const Json otherDimension = dimensionsByCategory["other"];
    if (jsTruthy(otherDimension)) {
        if (otherDimension.has("gameInput"))
            response.set("input", otherDimension["gameInput"]);
    } else {
        response.set("input", Json());
    }
    const Json pictureMode = pictureSettings["pictureMode"];
    response.set("pictureMode", jsTruthy(pictureMode) ? pictureMode : Json());
    return response;
}

} // namespace service
} // namespace nativetv
