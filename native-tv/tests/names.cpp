#include "domain/names.hpp"
#include <cstdio>
#include <stdexcept>
#include <string>

using namespace nativetv;

namespace {
unsigned checks = 0;

void check(bool value, const char *message) {
    ++checks;
    if (!value)
        throw std::runtime_error(message);
}

void named(const Json &value, const std::string &display, const std::string &label,
           const char *message) {
    check(value["display"].string() == display && value["label"].string() == label, message);
}
} // namespace

int main() {
    using namespace nativetv::names;
    try {
        check(snakeCase("HdrTypeNew") == "hdr_type_new", "Camel case labels");
        check(mapped(Json::parse("{\"knownMode\":\"mapped\"}"), "knownMode") == "mapped" &&
                  mapped(Json::object(), "Hdr10Plus") == "hdr10_plus",
              "Mapped values preserve unknown labels");

        named(hdmiPhyMode("FRL 12G 4L(R6)"), "FRL 48 Gbps", "frl_48",
             "FRL lane rate");
        check(hdmiPhyMode("FRL 6G 3L")["bitsPerSecond"].number() == 18e9,
              "FRL rate is lane rate times lanes");
        check(hdmiPhyMode("6G", Json::numeric(594000))["bitsPerSecond"].number() ==
                  594000.0 * 1000 * 10 * 3,
              "TMDS character clock rate");
        check(!hdmiPhyMode("3G")["bitsPerSecond"].valid(), "TMDS without clock has null rate");
        named(hdmiPhyMode("FRL CTS"), "FRL CTS", "other", "Unknown PHY mode");
        named(hdmiChroma("Y420"), "YCbCr 4:2:0", "ycbcr_420", "HDMI chroma map");
        named(hdmiChroma("Y440"), "Y440", "y440", "Unknown chroma fallback");
        named(hdmiHdcp("HDCP23"), "HDCP 2.3", "2_3", "HDCP map");
        named(hdmiHdcp("HDCP2X"), "HDCP2X", "hdcp2_x", "Unknown HDCP fallback");

        Json range = dynamicRange("dolbyHdrALLM");
        check(range["display"].string() == "Dolby Vision · Low latency" &&
                  range["label"].string() == "dolby_vision" && range["lowLatency"].boolean(),
              "Dynamic range reads ALLM separately");
        range = dynamicRange("hdr10Plus");
        check(range["display"].string() == "HDR10 Plus" &&
                  range["label"].string() == "hdr10_plus" && !range["lowLatency"].boolean(),
              "Unknown dynamic range remains readable");
        named(signalHdrType("DOLBY_LL"), "Dolby Vision (low latency)",
              "dolby_vision_low_latency", "Dolby low latency HDR compatibility label");
        named(signalHdrType("HDR10_PLUS"), "HDR10 Plus", "hdr10_plus", "Unknown HDR type");
        check(signalEotf(3)["label"].string() == "hlg" && !signalEotf(4).valid(),
              "EOTF reserved values have no mapping");
        check(signalColorimetry("BT2020_RGBORYCbCr")["label"].string() ==
                  "bt2020_rgb_or_ycbcr" && signalEncoding("YCbCr440")["label"].string() ==
                  "ycb_cr440",
              "Colorimetry and pixel encoding labels");
        check(powerOnReason("wakeOnWiFi")["label"].string() == "wake_on_wifi" &&
                  powerOnReason("rebootByOnRegular")["label"].string() ==
                      "reboot_by_on_regular",
              "Power-on reason labels");

        check(inputs()["livetv"].string() == "Live TV", "Input name table");
        check(hdmiKeyForPortId("HDMI_3").string() == "hdmi3" &&
                  hdmiKeyForPortId("HDMI4").string() == "hdmi4" &&
                  !hdmiKeyForPortId("HDMI_5").valid(),
              "HDMI port IDs");
        check(hdmiPort("hdmi2").number() == 2 && !hdmiPort("hdmi5").valid() &&
                  isHdmiKey("hdmi1") && !isHdmiKey("livetv"),
              "HDMI keys and port numbers");
        check(shortAppId("com.webos.app.hdmi3") == "hdmi3" &&
                  shortAppId("com.sling.tv", Json::parse("{\"com.sling.tv\":\"hdmi3\"}")) ==
                      "hdmi3" &&
                  inputAppId("hdmi2") == "com.webos.app.hdmi2",
              "Input app IDs and aliases");
        named(input("av1"), "av1", "av1", "Unknown input fallback");
        check(inputTitle("hdmi2", Json::text("Apple TV")) == "Apple TV (HDMI2)" &&
                  inputTitle("netflix") == "netflix",
              "Input title formatting");

        check(powerState(Json::text("Active Standby"))["display"].string() == "Standby" &&
                  !powerState(Json::text("screen_off"))["screenOn"].boolean() &&
                  powerState(Json::text("Warm Boot"))["display"].string() == "Warm Boot" &&
                  powerState(Json())["display"].string() == "Unknown",
              "Power state normalization and fallback");
        check(powerStates().has("active") && powerStates().has("processing") &&
                  powerState(Json::text("Screen Saver"))["screenOn"].boolean(),
              "Power state table coverage");

        // test-power-state.js mapPowerState: which states have the screen on.
        for (const char *active : {"Active", "active", "On", "Screen Saver", "screensaver"})
            check(powerState(Json::text(active))["screenOn"].boolean() &&
                      powerState(Json::text(active))["systemOn"].boolean(),
                  (std::string("active and screensaver states have the screen on: ") + active).c_str());
        check(!powerState(Json::text("Screen Off"))["screenOn"].boolean() &&
                  powerState(Json::text("Screen Off"))["systemOn"].boolean(),
              "Screen Off is the system on with the screen off");
        for (const char *dark : {"Active Standby", "active_standby", "Standby", "standby", "Suspend",
                                 "suspend", "Prepare Suspend", "preparesuspend", "Request Power Off",
                                 "requestpoweroff", "Power Off", "poweroff", "Off", "off",
                                 "Processing", "processing", "Starting up", "prepared"})
            check(!powerState(Json::text(dark))["screenOn"].boolean(),
                  (std::string("standby, suspend and transitional states have the screen off: ") + dark).c_str());
        for (const char *unknown : {"", "Unknown", "something_unknown"})
            check(!powerState(Json::text(unknown))["screenOn"].boolean() &&
                      !powerState(Json::text(unknown))["systemOn"].boolean(),
                  (std::string("an unknown state defaults to off: ") + unknown).c_str());
        check(!powerState(Json())["screenOn"].boolean() && !powerState(Json())["systemOn"].boolean(),
              "a missing state defaults to off");

        named(pictureMode("dolbyHdrCinemaBright"), "Dolby Vision Cinema Bright", "cinema_bright",
              "Picture mode table");
        named(pictureMode("hdrExternal"), "HDR External", "standard",
              "Picture mode with label only");
        named(pictureMode("dolbyHdrCinemaHome"), "Dolby Vision Cinema Home",
              "dolby_hdr_cinema_home", "Picture mode with display only");
        named(soundOutput("external_arc"), "HDMI ARC", "external_arc", "Sound output map");
        named(soundOutput("usb_speaker"), "Usb Speaker", "usb_speaker", "Unknown sound output");
        named(audioCodec(Json::text("EAC3")), "Dolby Digital Plus", "eac3", "Audio codec map");
        named(audioCodec(Json::text("alac")), "ALAC", "alac", "Unknown audio codec");
        check(!audioImmersive(Json::text("none")).valid() &&
                  audioImmersive(Json::text("DOLBY_ATMOS"))["display"].string() == "Dolby Atmos" &&
                  audioImmersive(Json::text("dtsx"))["label"].string() == "dts_x",
              "Immersive audio aliases and empty values");

        check(logoDimming()["strong"].string() == "High" && energySavingSteps().size() == 6,
              "Picture setting names");
        check(refresherStatus("cancel_schedule") == "Scheduled" &&
                  refresherStatus("unknown") == "Idle" &&
                  compensationStatus(true)["display"].string() == "Running" &&
                  compensationStatus(false)["detail"].string() == "Idle",
              "Panel maintenance status names");
        check(emmcEol(1)["label"].string() == "normal" && !emmcEol(0).valid() &&
                  vrrType(Json::text("freeSync"))["label"].string() == "free_sync" &&
                  vrrType(Json())["label"].string() == "off",
              "eMMC and VRR labels");

        std::printf("%u native names checks passed\n", checks);
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
