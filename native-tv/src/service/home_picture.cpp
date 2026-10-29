#include "service/home_picture.hpp"
#include "domain/names.hpp"
#include <cmath>
#include <cstdlib>
#include <regex>
#include <vector>

namespace nativetv {
namespace service {

std::string homePictureRange(const std::string &hdr) {
    if (hdr == "dolby_vision")
        return "dolbyHdr";
    if (hdr == "hdr10" || hdr == "hlg")
        return "hdr";
    return "sdr";
}

Json homePictureResponse(const std::string &input, const std::string &range,
                         const Json &signal, const std::string &mode,
                         const Json &light, bool oled) {
    Json result = Json::object();
    result.set("ok", Json::flag(true));
    result.set("input", Json::text(input));
    result.set("signal", signal);
    result.set("range", range.empty() ? Json() : Json::text(range));
    if (mode.empty()) {
        result.set("mode", Json());
    } else {
        Json pictureMode = Json::object();
        pictureMode.set("raw", Json::text(mode));
        pictureMode.set("display", names::pictureMode(mode)["display"]);
        result.set("mode", pictureMode);
    }
    result.set("light", light);
    result.set("oled", Json::flag(oled));
    return result;
}

Json homePictureHdmiSignal(const std::string &status, const std::string &packet,
                           bool packetAvailable, const Json &facts) {
    const Json timing = facts["timing"];
    const Json width = timing["width"];
    const Json height = timing["height"];
    if (!timing["connected"].boolean() || !width.isNumber() || !height.isNumber())
        return Json();

    Json signal = Json::object();
    signal.set("width", width);
    signal.set("height", height);

    double hz = timing["refreshHz"].number();
    static const std::regex frequency("cvi_vfreq\\[(\\d+)\\]", std::regex::icase);
    std::smatch frequencyMatch;
    if (std::regex_search(status, frequencyMatch, frequency)) {
        const double raw = std::strtod(frequencyMatch[1].str().c_str(), nullptr);
        if (raw > 0) hz = raw / 100.0;
    }
    signal.set("hz", hz > 0 && std::isfinite(hz)
                          ? Json::numeric(std::round(hz * 100.0) / 100.0)
                          : Json());

    const std::string rawChroma = facts["link"]["chroma"].string();
    const Json chroma = rawChroma.empty() ? Json() : names::hdmiChroma(rawChroma)["display"];
    signal.set("chroma", chroma);

    static const std::regex depth("DeepColorMode\\[\\s*(\\d+)BIT\\]", std::regex::icase);
    std::smatch depthMatch;
    signal.set("bits", std::regex_search(status, depthMatch, depth)
                            ? Json::numeric(std::strtod(depthMatch[1].str().c_str(), nullptr))
                            : Json());

    static const std::regex dolby("Dolby HDR Info\\[\\s*1\\s*\\]", std::regex::icase);
    std::string hdr;
    if (std::regex_search(status, dolby)) {
        hdr = "dolby_vision";
    } else if (!packetAvailable || packet.empty()) {
        hdr.clear();
    } else {
        const size_t packetAt = packet.find("=>  DRM ");
        if (packetAt == std::string::npos) {
            hdr = "sdr";
        } else {
            const size_t nextPacket = packet.find("\nData Packet", packetAt);
            const std::string block = packet.substr(
                packetAt, nextPacket == std::string::npos
                              ? std::string::npos : nextPacket - packetAt);
            static const std::regex byte("Hex\\[\\s*0x([[:xdigit:]]+)\\s*\\]",
                                         std::regex::icase);
            std::vector<unsigned> bytes;
            for (std::sregex_iterator it(block.begin(), block.end(), byte), end;
                 it != end; ++it) {
                bytes.push_back(static_cast<unsigned>(std::strtoul((*it)[1].str().c_str(),
                                                                   nullptr, 16)));
            }
            if (bytes.size() < 5 || bytes[0] != 0x87) {
                hdr = "sdr";
            } else {
                const Json eotf = names::signalEotf(bytes[4]);
                const std::string label = eotf["label"].string();
                hdr = label == "pq" ? "hdr10" : label == "hlg" ? "hlg" : "sdr";
            }
        }
    }
    signal.set("hdr", hdr.empty() ? Json() : Json::text(hdr));
    signal.set("vrr", Json::flag(facts["vrr"].boolean()));
    signal.set("allm", Json::flag(facts["allm"].boolean()));
    return signal;
}

Json homePictureAppVideo(const Json &reply) {
    const Json videos = reply["video"];
    if (!videos.isArray()) return Json();
    for (const Json &video : videos.items()) {
        const std::string hdrType = video["videoInfo"]["hdrType"].string();
        const std::string appId = video["appId"].string();
        const double width = video["width"].number();
        const double height = video["height"].number();
        if (video["sink"].string() != "MAIN" ||
            !video["connected"].boolean() ||
            video["connectedSource"].string() != "VDEC" ||
            appId.empty() || !video["videoInfo"].isObject() ||
            width <= 0 || height <= 0)
            continue;

        Json signal = Json::object();
        signal.set("width", Json::numeric(width));
        signal.set("height", Json::numeric(height));
        const double frameRate = video["frameRate"].number();
        signal.set("hz", frameRate > 0
                             ? Json::numeric(std::round(frameRate * 100.0) / 100.0)
                             : Json());
        signal.set("chroma", Json());
        signal.set("bits", Json());
        const std::string hdr = hdrType == "DolbyVision" ? "dolby_vision"
                                : hdrType == "HDR10" || hdrType == "HDR10Plus" ? "hdr10"
                                : hdrType == "HLG" ? "hlg" : "sdr";
        signal.set("hdr", Json::text(hdr));
        signal.set("vrr", Json::flag(false));
        signal.set("allm", Json::flag(false));
        signal.set("played", Json::flag(true));
        Json result = Json::object();
        result.set("appId", Json::text(appId));
        result.set("signal", signal);
        return result;
    }
    return Json();
}

} // namespace service
} // namespace nativetv
