#include "service/control_commands.hpp"
#include "service/remote_input.hpp"
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

using namespace nativetv;

namespace {
void check(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}

std::string temporaryDirectory() {
    char pattern[] = "/tmp/nativetv-remote-input-XXXXXX";
    char *created = mkdtemp(pattern);
    check(created != nullptr, "could not create remote input test directory");
    return created;
}

void makeDirectory(const std::string &path) {
    check(mkdir(path.c_str(), 0700) == 0, "could not create " + path);
}

std::vector<unsigned char> bytes(const std::string &path) {
    std::ifstream input(path.c_str(), std::ios::binary);
    return std::vector<unsigned char>(std::istreambuf_iterator<char>(input),
                                      std::istreambuf_iterator<char>());
}

unsigned read16(const std::vector<unsigned char> &data, size_t offset) {
    return static_cast<unsigned>(data[offset]) |
           (static_cast<unsigned>(data[offset + 1]) << 8);
}

uint32_t read32(const std::vector<unsigned char> &data, size_t offset) {
    return static_cast<uint32_t>(data[offset]) |
           (static_cast<uint32_t>(data[offset + 1]) << 8) |
           (static_cast<uint32_t>(data[offset + 2]) << 16) |
           (static_cast<uint32_t>(data[offset + 3]) << 24);
}

void checkEvent(const std::vector<unsigned char> &data, size_t offset,
                unsigned type, unsigned code, int value) {
    check(read16(data, offset + 8) == type &&
              read16(data, offset + 10) == code &&
              read32(data, offset + 12) == static_cast<uint32_t>(value),
          "input event bytes do not match the legacy Linux input_event layout");
}
} // namespace

int main() {
    try {
        check(service::controlMediaKeyCode(Json::text("play")) == 207 &&
                  service::controlMediaKeyCode(Json::text("pause")) == 119 &&
                  service::controlMediaKeyCode(Json::text("stop")) == 128 &&
                  service::controlMediaKeyCode(Json::text("fastForward")) == 208 &&
                  service::controlMediaKeyCode(Json::text("fastforward")) == 208 &&
                  service::controlMediaKeyCode(Json::text("rewind")) == 168,
              "media action names map to the legacy remote key codes");
        check(service::controlMediaKeyCode(Json::text("playPause"), "playing", "MSE") == 119 &&
                  service::controlMediaKeyCode(Json::text("playpause"), "playing",
                                               "external input") == 207 &&
                  service::controlMediaKeyCode(Json::text("playPause"), "paused", "MSE") == 207,
              "play/pause toggling preserves built-in and external-input behavior");
        check(service::controlMediaKeyCode(Json::text("Pause")) == 0 &&
                  service::controlMediaKeyCode(Json::text("playPause ")) == 0 &&
                  service::controlMediaKeyCode(Json::numeric(207)) == 0,
              "unknown media key values keep the legacy false response");

        const std::string root = temporaryDirectory();
        const std::string procRoot = root + "/proc";
        const std::string systemRoot = root + "/system";
        makeDirectory(procRoot);
        makeDirectory(procRoot + "/bus");
        makeDirectory(procRoot + "/bus/input");
        makeDirectory(systemRoot);
        makeDirectory(systemRoot + "/dev");
        makeDirectory(systemRoot + "/dev/input");
        {
            std::ofstream devices((procRoot + "/bus/input/devices").c_str());
            devices << "I: Bus=0003 Vendor=0000 Product=0000 Version=0000\n"
                    << "N: Name=\"LGE RCU\"\n"
                    << "H: Handlers=kbd event7\n";
            std::ofstream event((systemRoot + "/dev/input/event7").c_str(),
                                std::ios::binary);
            check(event.good(), "could not create fake RCU input device");
        }
        const std::string eventPath = systemRoot + "/dev/input/event7";
        check(service::injectRemoteMediaKey(procRoot, systemRoot, 119),
              "detected RCU input device should accept a supported media key");
        const std::vector<unsigned char> data = bytes(eventPath);
        check(data.size() == 64, "one key press must emit press, sync, release and sync events");
        checkEvent(data, 0, 1, 119, 1);
        checkEvent(data, 16, 0, 0, 0);
        checkEvent(data, 32, 1, 119, 0);
        checkEvent(data, 48, 0, 0, 0);
        {
            std::ofstream reset(eventPath.c_str(), std::ios::binary | std::ios::trunc);
        }
        check(service::injectRemoteHomeKey(procRoot, systemRoot),
              "Home fallback should write KEY_LEFTMETA to the detected remote device");
        const std::vector<unsigned char> homeData = bytes(eventPath);
        check(homeData.size() == 64,
              "Home fallback emits the same press, sync, release and sync sequence");
        checkEvent(homeData, 0, 1, 125, 1);
        checkEvent(homeData, 16, 0, 0, 0);
        checkEvent(homeData, 32, 1, 125, 0);
        checkEvent(homeData, 48, 0, 0, 0);
        check(!service::injectRemoteMediaKey(procRoot, systemRoot, 139),
              "unsupported key codes must not be written to the remote device");
        check(bytes(eventPath).size() == 64,
              "unsupported keys leave the input event stream unchanged");

        {
            std::ofstream devices((procRoot + "/bus/input/devices").c_str());
            devices << "I: Bus=0003 Vendor=0000 Product=0000 Version=0000\n"
                    << "N: Name=\"LGE RCU\"\n"
                    << "H: Handlers=kbd event9\n";
            std::ofstream fallback((systemRoot + "/dev/input/event1").c_str(),
                                  std::ios::binary);
            check(fallback.good(), "could not create fallback input device");
        }
        check(service::injectRemoteMediaKey(procRoot, systemRoot, 207),
              "missing detected event nodes should use event1 as Node does");
        const std::vector<unsigned char> fallback =
            bytes(systemRoot + "/dev/input/event1");
        check(fallback.size() == 64, "fallback RCU node should receive all four events");
        checkEvent(fallback, 0, 1, 207, 1);
        {
            std::ofstream reset((systemRoot + "/dev/input/event1").c_str(),
                                std::ios::binary | std::ios::trunc);
        }
        check(service::injectRemoteHomeKey(procRoot, systemRoot),
              "Home fallback should use event1 when the discovered device cannot open");
        const std::vector<unsigned char> fallbackHome =
            bytes(systemRoot + "/dev/input/event1");
        check(fallbackHome.size() == 64,
              "Home fallback writes its complete event sequence to event1");
        checkEvent(fallbackHome, 0, 1, 125, 1);
        checkEvent(fallbackHome, 32, 1, 125, 0);

        std::remove((systemRoot + "/dev/input/event1").c_str());
        std::remove(eventPath.c_str());
        std::remove((procRoot + "/bus/input/devices").c_str());
        rmdir((systemRoot + "/dev/input").c_str());
        rmdir((systemRoot + "/dev").c_str());
        rmdir(systemRoot.c_str());
        rmdir((procRoot + "/bus/input").c_str());
        rmdir((procRoot + "/bus").c_str());
        rmdir(procRoot.c_str());
        rmdir(root.c_str());
        std::cout << "remote media key mapping and Linux input event checks passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "remote input checks failed: " << error.what() << '\n';
        return 1;
    }
}
