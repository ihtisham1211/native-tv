// test/test-lgsettings.js, against the native rows and writes.
#include "domain/names.hpp"
#include "service/lg_settings.hpp"
#include <cassert>
#include <string>
#include <vector>

using nativetv::Json;
using namespace nativetv::service;

namespace {
Json row(const Json &response, const std::string &id) {
    for (const Json &candidate : response["rows"].items())
        if (candidate["id"].string() == id) return candidate;
    return Json();
}

std::vector<std::string> ids(const Json &response) {
    std::vector<std::string> out;
    for (const Json &candidate : response["rows"].items()) out.push_back(candidate["id"].string());
    return out;
}

bool has(const std::vector<std::string> &list, const std::string &id) {
    for (const std::string &candidate : list)
        if (candidate == id) return true;
    return false;
}

Json response(const std::vector<std::string> &sections, const char *settings,
              const char *descriptions = "{}") {
    return lgSettingsResponse(sections, Json::parse(settings), Json::parse(descriptions),
                              Json::object(), Json::object());
}
} // namespace

int main() {
    // 0. The screen saver's start time: LG's four values only, and a row only on a TV that reports it.
    {
        assert((lgSettingsCategories({"screensaver"}) == std::vector<std::string>{"option"}));
        assert((lgSettingsKeys("option", {"screensaver"}) == std::vector<std::string>{"screenSaverTimer"}));
        assert(ids(response({"screensaver"}, R"({"option":{}})")).empty());
        const Json r = response({"screensaver"}, R"({"option":{"screenSaverTimer":"10"}})");
        assert((ids(r) == std::vector<std::string>{"screenSaverTimer"}));
        assert(row(r, "screenSaverTimer")["value"].string() == "10" && row(r, "screenSaverTimer")["choices"].size() == 4);
        const LgSettingWrite twenty = lgSettingWrite(Json::parse(R"({"id":"screenSaverTimer","value":"20"})"));
        assert(twenty.valid && twenty.category == "option" && twenty.key == "screenSaverTimer" &&
               twenty.storedValue.string() == "20");
        // Any other time, such as 2 or 4, stops the screen saver coming at all on a C5.
        assert(!lgSettingWrite(Json::parse(R"({"id":"screenSaverTimer","value":"4"})")).valid);
    }
    // 1. Only the rows the TV has come back, one read per category of the section.
    // A B8-like TV: no screen saver ads, no smart tips, no Content Recommendation.
    {
        const std::vector<std::string> categories = lgSettingsCategories({"promotions"});
        assert((categories == std::vector<std::string>{"general", "other", "option"}));
        const Json r = response({"promotions"},
            R"({"general":{"homePromotion":"on","adCookie":"on"},"other":{},"option":{"livePromotion":"off"}})");
        assert((ids(r) == std::vector<std::string>{"homePromotion", "livePromotion"}));
        assert(row(r, "homePromotion")["on"].boolean());
        assert(!row(r, "livePromotion")["on"].boolean());
    }

    // 2. Writing stores LG's value for the row.
    {
        Json request = Json::object();
        request.set("id", Json::text("homePromotion"));
        request.set("on", Json::flag(false));
        const LgSettingWrite write = lgSettingWrite(request);
        assert(write.handled && write.valid);
        assert(write.category == "general" && write.key == "homePromotion");
        assert(write.storedValue.string() == "off");
        assert(!write.greyable);
    }

    // 3. Choices and numbers take only their own values, and the TV's own keys.
    {
        const Json r = response({"game"},
            R"({"other":{"gameGenre":"Standard","blackStabilizer":10},"sound":{"aigamesound":"on"}})");
        assert(row(r, "gameGenre")["type"].string() == "choice");
        assert(row(r, "gameGenre")["value"].string() == "Standard");
        assert(row(r, "blackStabilizer")["min"].number() == 0);
        assert(row(r, "aigamesound")["on"].boolean());
        assert(!row(r, "enableALLM").valid());  // a key the TV lacks is left out

        const auto write = [](const char *id, const char *value) {
            return lgSettingWrite(Json::parse(std::string("{\"id\":\"") + id + "\",\"value\":" + value + "}"));
        };
        assert(write("gameGenre", "\"FPS\"").valid);
        assert(write("gameGenre", "\"FPS\"").storedValue.string() == "FPS");
        assert(!write("gameGenre", "\"Racing\"").valid);
        assert(!write("gameGenre", "\"Racing\"").error.empty());
        assert(!write("blackStabilizer", "21").valid);
        assert(!write("blackStabilizer", "1.5").valid);
        assert(!write("blackStabilizer", "\"15\"").valid);
        assert(write("blackStabilizer", "15").valid);
        assert(write("blackStabilizer", "15").storedValue.number() == 15);
    }

    // 4. Choices and ranges follow what the TV describes; text numbers stay text.
    {
        const Json r = response({"sound"},
            R"({"sound":{"soundMode":"standard","audioBalance":"0"}})",
            R"({"sound":[
                {"key":"soundMode","values":{"arrayExt":[{"value":"standard"},{"value":"movie"},{"value":"news","visible":false}]}},
                {"key":"audioBalance","values":{"min":-50,"max":50}}]})");
        const Json choices = row(r, "soundMode")["choices"];
        assert(choices.size() == 2);
        assert(choices.items()[0]["value"].string() == "standard");
        assert(choices.items()[1]["value"].string() == "movie");
        assert(row(r, "audioBalance")["value"].isNumber());
        assert(row(r, "audioBalance")["value"].number() == 0);
        assert(row(r, "audioBalance")["min"].number() == -50);

        const LgSettingWrite balance = lgSettingWrite(Json::parse(R"({"id":"audioBalance","value":-5})"));
        assert(balance.valid && balance.category == "sound");
        assert(balance.storedValue.isString() && balance.storedValue.string() == "-5");
        assert(!lgSettingWrite(Json::parse(R"({"id":"audioBalance","value":51})")).valid);
    }

    // A value set outside the list is kept, by its own name.
    {
        const Json r = response({"sound"}, R"({"sound":{"soundOutput":"wisa_speaker"}})");
        const Json out = row(r, "soundOutput");
        const std::vector<Json> choices = out["choices"].items();
        assert(choices.back()["value"].string() == "wisa_speaker");
        assert(choices.back()["label"].string() == "wisa_speaker");
        assert(out["value"].string() == "wisa_speaker");
    }

    // A per-port row gives way where the TV has the one setting, and a setting
    // the TV has greyed out comes back marked so.
    {
        const Json r = response({"hdmi"},
            R"({"other":{"uhdDeepColor":"off","uhdDeepColorHDMI1":"off"}})",
            R"({"other":[{"key":"uhdDeepColor","values":{"arrayExt":[{"value":"off"},{"value":"4k"}]},"ui":{"active":false}}]})");
        assert(has(ids(r), "deepColor"));
        assert(!has(ids(r), "deepColorHDMI1"));
        assert(row(r, "deepColor")["active"].isBoolean() && !row(r, "deepColor")["active"].boolean());

        const Json port = response({"hdmi"}, R"({"other":{"uhdDeepColorHDMI1":"on"}})");
        assert((ids(port) == std::vector<std::string>{"deepColorHDMI1"}));
        assert(!row(port, "deepColorHDMI1")["active"].valid());
        assert(row(port, "deepColorHDMI1")["port"].number() == 1);
    }

    // A greyed-out setting is not written although the TV would store it: the
    // write says which rows the caller must check against the description.
    {
        const LgSettingWrite format = lgSettingWrite(Json::parse(R"({"id":"audioFormatHDMI1","value":"pcm"})"));
        assert(format.valid && format.greyable && format.key == "inputAudioFormatHDMI1");
        assert(lgSettingWrite(Json::parse(R"({"id":"deepColorHDMI2","on":true})")).greyable);
        assert(!lgSettingWrite(Json::parse(R"({"id":"soundOutput","value":"tv_speaker"})")).greyable);
    }

    // 5. Only rows in the table can be written.
    {
        assert(!lgSettingWrite(Json::parse(R"({"id":"systemPin","on":true})")).handled);
        assert(!lgSettingWrite(Json::parse(R"({"on":true})")).handled);
        assert(!lgSettingWrite(Json::parse("[]")).handled);
    }

    // The sound outputs read as the names module names them everywhere else.
    {
        const Json r = response({"sound"}, R"({"sound":{"soundOutput":"tv_speaker"}})");
        for (const Json &choice : row(r, "soundOutput")["choices"].items())
            assert(choice["label"].string() ==
                   nativetv::names::soundOutput(choice["value"].string())["display"].string());
    }

    // A backup value is checked against the row the TV reports now.
    {
        const Json current = Json::parse(R"({"choices":[{"value":"standard"},{"value":"movie"}]})");
        assert(lgSettingsRestoreWrite("soundMode", Json::text("movie"), current).valid);
        assert(!lgSettingsRestoreWrite("soundMode", Json::text("news"), current).valid);
        assert(!lgSettingsRestoreWrite("soundMode", Json::text("movie"), Json()).valid);
        assert(lgSettingsRestoreWrite("homePromotion", Json::flag(true), Json::object()).storedValue.string() == "on");
        assert(!lgSettingsRestoreWrite("homePromotion", Json::text("on"), Json::object()).valid);
        assert(!lgSettingsRestoreWrite("systemPin", Json::flag(true), Json::object()).handled);
        const Json range = Json::parse(R"({"min":-10,"max":10})");
        assert(lgSettingsRestoreWrite("audioBalance", Json::numeric(-5), range).storedValue.string() == "-5");
        assert(!lgSettingsRestoreWrite("audioBalance", Json::numeric(11), range).valid);
    }

    // A B3 on its speaker: the stored soundOutput is the last HDMI ARC pick, the dimension is the output in use.
    {
        const Json r = lgSettingsResponse(
            {"sound"},
            Json::parse(R"({"sound":{"soundOutput":"external_arc","soundMode":"movie"}})"), Json::parse("{}"),
            Json::parse(R"({"sound":{"audioInput":"hdmi1","soundMode":"movie","soundOutput":"tv_speaker"}})"),
            Json::object());
        assert(row(r, "soundOutput")["value"].string() == "tv_speaker");
        assert(row(r, "soundMode")["value"].string() == "movie");
        // A key the dimension does not name keeps the stored value.
        const Json other = lgSettingsResponse(
            {"sound"}, Json::parse(R"({"sound":{"soundOutput":"external_arc"}})"), Json::parse("{}"),
            Json::parse(R"({"sound":{"audioInput":"hdmi1"}})"), Json::object());
        assert(row(other, "soundOutput")["value"].string() == "external_arc");
    }

    // A number comes from the TV client as a decimal (11.0) and is kept as the whole number it is, so
    // the TV settings and every screen that reads them show 11, not 11.0.
    {
        for (const char *id : {"blackStabilizer", "audioBalance"}) {
            const LgSettingWrite write = lgSettingWrite(Json::parse(
                std::string("{\"id\":\"") + id + "\",\"value\":" + (std::string(id) == "audioBalance" ? "-5.0" : "11.0") + "}"));
            assert(write.valid);
            assert(write.storedValue.dump() == (std::string(id) == "audioBalance" ? "\"-5\"" : "11"));
        }
        const Json current = Json::parse(R"({"min":0,"max":20})");
        assert(lgSettingsRestoreWrite("blackStabilizer", Json::numeric(7), current).storedValue.dump() == "7");
    }

    // The input names the HDMI rows are shown under.
    {
        const Json inputs = lgSettingsInputNames(Json::parse(
            R"([{"id":"HDMI_1","label":"Console"},{"id":"HDMI_2"},{"id":"AV_1","label":"x"}])"));
        assert(inputs["hdmi1"].string() == "Console");
        assert(inputs["hdmi2"].isString() && !inputs["hdmi2"].string().empty());
        assert(inputs.keys().size() == 2);
    }
    return 0;
}
