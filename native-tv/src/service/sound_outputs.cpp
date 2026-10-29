#include "service/sound_outputs.hpp"
#include <algorithm>

namespace nativetv {
namespace service {
namespace {
const std::vector<std::string> kOutputOrder = {
    "tv_speaker", "external_optical", "tv_external_speaker", "external_arc",
    "tv_speaker_external_arc", "bt_soundbar", "tv_speaker_bluetooth", "wow_cast",
    "tv_speaker_wow_cast", "lineout", "headphone", "usb_speaker",
    "tv_speaker_headphone", "mobile_phone", "wisa_speaker"};

bool has(const Json &values, const std::string &wanted) {
    if (!values.isArray())
        return false;
    for (const Json &value : values.items())
        if (value.isString() && value.string() == wanted)
            return true;
    return false;
}

bool equals(const Json &object, const char *key, const char *value) {
    const Json field = object[key];
    return field.isString() && field.string() == value;
}

bool falseValue(const Json &object, const char *key) {
    const Json field = object[key];
    return field.isBoolean() && !field.boolean();
}

bool hidden(const std::string &id, const Json &configs) {
    if (id == "tv_speaker_optical_arc" || id == "tv_speakerbar" ||
        id == "builtin_soundbar" || id == "tv_speakerbar_headphone" ||
        id == "bt_audio" || id == "tv_speaker_external_arc" ||
        id == "tv_speaker_wow_cast")
        return true;

    const Json bluetooth = configs["system.supportBluetoothFeatures"];
    const Json profile = configs["profile.list"];
    if (id == "bt_soundbar")
        return bluetooth.isArray() && !has(bluetooth, "btsound");
    if (id == "tv_speaker_bluetooth")
        return bluetooth.isArray() && !has(bluetooth, "bluetoothPlusTvSpeaker");
    if (id == "lineout")
        return falseValue(configs, "tv.model.supportAudioLineOut") ||
               falseValue(configs, "tv.model.supportHeadPhone");
    if (id == "headphone" || id == "tv_speaker_headphone")
        return falseValue(configs, "tv.model.supportHeadPhone");
    if (id == "external_optical" || id == "tv_external_speaker") {
        const Json optical = configs["tv.model.supportOpticalJack"];
        return optical.valid() && !equals(configs, "tv.model.supportOpticalJack", "On");
    }
    if (id == "wisa_speaker")
        return configs["tv.model.supportWiSA"].boolean(false) == false;
    if (id == "wow_cast")
        return configs["com.webos.service.wowplay.supportWowCast"].boolean(false) == false;
    if (id == "usb_speaker")
        return !(has(profile, "gaming") && has(profile, "usbSoundOutput"));
    return false;
}
} // namespace

const std::vector<std::string> &soundOutputConfigNames() {
    static const std::vector<std::string> names = {
        "tv.model.supportHeadPhone", "tv.model.supportOpticalJack",
        "tv.model.supportAudioLineOut", "tv.model.supportWiSA",
        "system.supportBluetoothFeatures",
        "com.webos.service.wowplay.supportWowCast", "profile.list"};
    return names;
}

Json offeredSoundOutputs(const Json &values, const Json &configs,
                         const std::string &current) {
    const Json entries = values.isArray() ? values : values["arrayExt"];
    Json offered = Json::array();
    for (const Json &entry : entries.items()) {
        const Json value = entry["value"];
        if (!value.isString() || value.string().empty() ||
            !entry["visible"].boolean(false) || !entry["active"].boolean(false) ||
            has(offered, value.string()))
            continue;
        if (value.string() == current || !hidden(value.string(), configs))
            offered.add(value);
    }
    if (offered.size() == 0)
        return offered;
    if (!current.empty() && !has(offered, current))
        offered.add(Json::text(current));

    std::vector<std::string> ids;
    ids.reserve(offered.size());
    for (const Json &value : offered.items())
        ids.push_back(value.string());
    std::stable_sort(ids.begin(), ids.end(), [](const std::string &left,
                                                const std::string &right) {
        const auto leftAt = std::find(kOutputOrder.begin(), kOutputOrder.end(), left);
        const auto rightAt = std::find(kOutputOrder.begin(), kOutputOrder.end(), right);
        const size_t leftIndex = leftAt == kOutputOrder.end()
            ? kOutputOrder.size() : static_cast<size_t>(leftAt - kOutputOrder.begin());
        const size_t rightIndex = rightAt == kOutputOrder.end()
            ? kOutputOrder.size() : static_cast<size_t>(rightAt - kOutputOrder.begin());
        return leftIndex < rightIndex;
    });
    Json result = Json::array();
    for (const std::string &id : ids)
        result.add(Json::text(id));
    return result;
}

} // namespace service
} // namespace nativetv
