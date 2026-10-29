#include "service/home_picture.hpp"
#include <cassert>

using namespace nativetv;

int main() {
    assert(service::homePictureRange("sdr") == "sdr");
    assert(service::homePictureRange("hdr10") == "hdr");
    assert(service::homePictureRange("hlg") == "hdr");
    assert(service::homePictureRange("dolby_vision") == "dolbyHdr");

    Json facts = Json::object();
    Json timing = Json::object();
    timing.set("connected", Json::flag(true));
    timing.set("width", Json::numeric(3840));
    timing.set("height", Json::numeric(2160));
    timing.set("refreshHz", Json::numeric(23));
    facts.set("timing", timing);
    Json link = Json::object();
    link.set("chroma", Json::text("Y444"));
    facts.set("link", link);
    facts.set("vrr", Json::flag(false));
    facts.set("allm", Json::flag(false));
    const std::string status =
        "Dolby HDR Info[0] DeepColorMode[10BIT] cvi_vfreq[2400]";
    const std::string packet =
        "Data Packet y  =>  DRM  \n"
        "[DRM][HB00] - Hex[  0x87  ]\n"
        "[DRM][HB01] - Hex[  0x01  ]\n"
        "[DRM][HB02] - Hex[  0x1a  ]\n"
        "[DRM][ B00] - Hex[  0x74  ]\n"
        "[DRM][ B01] - Hex[  0x02  ]\n";
    Json hdmi = service::homePictureHdmiSignal(status, packet, true, facts);
    assert(hdmi["width"].number() == 3840);
    assert(hdmi["height"].number() == 2160);
    assert(hdmi["hz"].number() == 24);
    assert(hdmi["chroma"].string() == "YCbCr 4:4:4");
    assert(hdmi["bits"].number() == 10);
    assert(hdmi["hdr"].string() == "hdr10");
    assert(!hdmi["vrr"].boolean() && !hdmi["allm"].boolean());
    assert(service::homePictureHdmiSignal(status, packet, false, facts)["hdr"].valid() == false);
    assert(service::homePictureHdmiSignal(
               "Dolby HDR Info[1]", packet, true, facts)["hdr"].string() == "dolby_vision");

    Json video = Json::parse(R"({"video":[
      {"sink":"SUB0","connected":true,"connectedSource":"VDEC",
       "appId":"wrong","width":1920,"height":1080,
       "frameRate":24,"videoInfo":{"hdrType":"HDR10"}},
      {"sink":"MAIN","connected":true,"connectedSource":"VDEC",
       "appId":"netflix","width":3840,"height":2160,
       "frameRate":23.9760246,"videoInfo":{"hdrType":"DolbyVision"}}
    ]})");
    Json appVideo = service::homePictureAppVideo(video);
    assert(appVideo["appId"].string() == "netflix");
    assert(appVideo["signal"]["width"].number() == 3840);
    assert(appVideo["signal"]["height"].number() == 2160);
    assert(appVideo["signal"]["hz"].number() == 23.98);
    assert(appVideo["signal"]["hdr"].string() == "dolby_vision");
    assert(appVideo["signal"]["played"].boolean());
    video = Json::parse(R"({"video":[
      {"sink":"MAIN","connected":false,"connectedSource":"unknown",
       "appId":"netflix","width":0,"height":0,"videoInfo":null}
    ]})");
    assert(!service::homePictureAppVideo(video).valid());

    Json signal = Json::object();
    signal.set("width", Json::numeric(3840));
    signal.set("height", Json::numeric(2160));
    Json response = service::homePictureResponse("dtv", "sdr", signal,
                                                 "standard", Json::numeric(50), true);
    assert(response["ok"].boolean());
    assert(response["input"].string() == "dtv");
    assert(response["signal"]["width"].number() == 3840);
    assert(response["range"].string() == "sdr");
    assert(response["mode"]["raw"].string() == "standard");
    assert(response["mode"]["display"].string() == "Standard");
    assert(response["light"].number() == 50);
    assert(response["oled"].boolean());

    response = service::homePictureResponse("hdmi2", "", Json(), "", Json(), false);
    assert(response["signal"].valid() == false);
    assert(response["range"].valid() == false);
    assert(response["mode"].valid() == false);
    assert(response["light"].valid() == false);
    assert(!response["oled"].boolean());
    return 0;
}
