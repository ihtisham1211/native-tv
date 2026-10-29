#pragma once

#include "core/json.hpp"
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace nativetv {
namespace service {

struct NtpSettings {
    std::string server;
    uint16_t port = 123;
};

struct NtpAnswer {
    double atMs = 0;
    double atMonoMs = 0;
    double offsetMs = 0;
    double roundTripMs = 0;
    unsigned stratum = 0;
};

struct NtpResult {
    bool ok = false;
    NtpAnswer answer;
    std::string error;
};

struct NtpServerInput {
    bool configured = false;
    bool valid = true;
    NtpSettings settings;
    std::string error;
};

NtpServerInput parseNtpServerInput(const std::string &text);
bool readNtpSettings(const Json &config, NtpSettings &settings);
std::array<uint8_t, 48> buildNtpRequest(double nowMs);
bool ntpRequestAnswered(const uint8_t *reply, size_t replySize,
                        const std::array<uint8_t, 48> &request);
double readNtpTimestamp(const uint8_t *packet, size_t offset);
void writeNtpTimestamp(uint8_t *packet, size_t offset, double unixMs);
NtpResult parseNtpReply(const uint8_t *reply, size_t replySize, double t1Ms,
                        double t4Ms, double t4MonoMs);
Json ntpSetTimePayload(const NtpAnswer &answer);
NtpResult queryNtp(const NtpSettings &settings,
                   std::chrono::milliseconds timeout = std::chrono::milliseconds(2000));

} // namespace service
} // namespace nativetv
