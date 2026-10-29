#include "service/sound_outputs.hpp"
#include <cassert>

using nativetv::Json;
using nativetv::service::offeredSoundOutputs;

namespace {
Json parse(const char *text) { return Json::parse(text); }

Json expected(const char *text) { return Json::parse(text); }
} // namespace

int main() {
    const Json values = parse(R"json([
      {"value":"tv_speaker","visible":true,"active":true},
      {"value":"tv_speakerbar","visible":true,"active":true},
      {"value":"external_optical","visible":true,"active":true},
      {"value":"tv_external_speaker","visible":true,"active":true},
      {"value":"external_arc","visible":true,"active":true},
      {"value":"bt_soundbar","visible":true,"active":true},
      {"value":"wisa_speaker","visible":true,"active":true},
      {"value":"mobile_phone","visible":true,"active":true},
      {"value":"lineout","visible":true,"active":true},
      {"value":"headphone","visible":true,"active":true},
      {"value":"tv_speaker_headphone","visible":true,"active":true},
      {"value":"tv_speaker_bluetooth","visible":true,"active":true},
      {"value":"tv_speaker_external_arc","visible":true,"active":true},
      {"value":"usb_speaker","visible":true,"active":true},
      {"value":"tv_speaker_optical_arc","visible":false,"active":false},
      {"value":"wow_cast","visible":true,"active":true},
      {"value":"tv_speaker_wow_cast","visible":true,"active":true}
    ])json");
    const Json c2 = parse(R"json({
      "system.supportBluetoothFeatures":["btsound","bluetoothPlusTvSpeaker"],
      "tv.model.supportHeadPhone":true,
      "com.webos.service.wowplay.supportWowCast":true,
      "tv.model.supportOpticalJack":"On",
      "tv.model.supportAudioLineOut":true,
      "tv.model.supportWiSA":false
    })json");
    assert(offeredSoundOutputs(values, c2, "tv_speaker").dump() == expected(
        R"json(["tv_speaker","external_optical","tv_external_speaker","external_arc","bt_soundbar","tv_speaker_bluetooth","wow_cast","lineout","headphone","tv_speaker_headphone","mobile_phone"])json").dump());

    Json inUse = offeredSoundOutputs(values, c2, "tv_speaker_external_arc");
    bool foundInUse = false;
    size_t inUseIndex = inUse.size(), bluetoothIndex = inUse.size();
    for (size_t index = 0; index < inUse.size(); ++index) {
        if (inUse[index].string() == "tv_speaker_external_arc") {
            foundInUse = true;
            inUseIndex = index;
        }
        if (inUse[index].string() == "bt_soundbar") bluetoothIndex = index;
    }
    assert(foundInUse && inUseIndex < bluetoothIndex);

    const Json olderValues = parse(R"json([
      {"value":"optical","visible":true,"active":true},
      {"value":"tv_speaker","visible":true,"active":true},
      {"value":"external_arc","visible":true,"active":true},
      {"value":"wisa_speaker","visible":true,"active":true},
      {"value":"soundbar","visible":true,"active":true},
      {"value":"headphone","visible":true,"active":true}
    ])json");
    assert(offeredSoundOutputs(olderValues, Json::object(), "tv_speaker").dump() == expected(
        R"json(["tv_speaker","external_arc","headphone","optical","soundbar"])json").dump());
    assert(offeredSoundOutputs(Json::array(), c2, "tv_speaker").size() == 0);
    assert(offeredSoundOutputs(values, c2, "tv_speaker").size() == 11);
    assert(nativetv::service::soundOutputConfigNames().size() == 7);
    return 0;
}
