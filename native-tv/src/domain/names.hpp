#pragma once

#include "core/json.hpp"
#include <string>
#include <utility>
#include <vector>

namespace nativetv {
namespace names {
std::string snakeCase(const std::string &value);
// What a TV is called when its owner has not named it: the series of its model, "B3" for
// OLED65B3PUA, "C2" for OLED42C24LA, "UQ8000" for 55UQ8000PUA; "LGTV" when the model is unknown.
std::string tvDefaultName(const std::string &model);
std::string mapped(const Json &table, const std::string &value);

Json hdmiPhyMode(const std::string &raw, const Json &tmdsClockKhz = Json());
Json hdmiChroma(const std::string &raw);
Json hdmiHdcp(const std::string &raw);
Json dynamicRange(const std::string &raw);
Json signalHdrType(const std::string &raw);
Json signalEotf(unsigned code);
Json signalColorimetry(const std::string &raw);
Json signalEncoding(const std::string &raw);
Json powerOnReason(const std::string &raw);

Json inputs();
std::string hdmiKey(unsigned port);
Json hdmiKeyForPortId(const std::string &portId);
Json hdmiPort(const std::string &key);
bool isHdmiKey(const std::string &key);
std::string shortAppId(const std::string &appId, const Json &aliases = Json());
std::string inputAppId(const std::string &key);
Json input(const std::string &id);
std::string inputTitle(const std::string &id, const Json &name = Json());

Json powerStates();
Json powerState(const Json &raw);
Json pictureModes();
Json pictureMode(const std::string &raw);
Json soundOutputs();
Json soundOutput(const std::string &raw);
Json audioCodec(const Json &raw);
Json audioImmersive(const Json &raw);

Json logoDimming();
Json energySavingSteps();
Json refresherStatuses();
std::string refresherStatus(const std::string &raw);
Json compensationStatuses();
Json compensationStatus(bool running);
Json emmcEolStates();
Json emmcEol(unsigned code);
Json vrrType(const Json &raw);

// The remote's buttons whose presses are reported as Remote Button events, by the code the
// remote's input device sends (Linux evdev, a 16-bit value) and the Qt key code LG's input log
// names (above 65535). The codes are from Glasshouse's measurements, not Native Tv's.
std::vector<std::pair<unsigned, std::string>> remoteButtonCodes();
// The button a code is, or "" for a code that is not reported.
std::string remoteButton(unsigned code);
// Every reported button once: the Remote Button entity's event types.
std::vector<std::string> remoteButtons();
} // namespace names
} // namespace nativetv
