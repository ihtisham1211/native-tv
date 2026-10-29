#include "daemon/remote_buttons.hpp"
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {
using namespace nativetv::daemon;

std::vector<unsigned char> event(size_t size, unsigned type, unsigned code,
                                 std::int32_t value) {
    std::vector<unsigned char> bytes(size, 0);
    const size_t offset = size - 8;
    bytes[offset] = static_cast<unsigned char>(type & 0xff);
    bytes[offset + 1] = static_cast<unsigned char>((type >> 8) & 0xff);
    bytes[offset + 2] = static_cast<unsigned char>(code & 0xff);
    bytes[offset + 3] = static_cast<unsigned char>((code >> 8) & 0xff);
    const std::uint32_t raw = static_cast<std::uint32_t>(value);
    bytes[offset + 4] = static_cast<unsigned char>(raw & 0xff);
    bytes[offset + 5] = static_cast<unsigned char>((raw >> 8) & 0xff);
    bytes[offset + 6] = static_cast<unsigned char>((raw >> 16) & 0xff);
    bytes[offset + 7] = static_cast<unsigned char>((raw >> 24) & 0xff);
    return bytes;
}

void append(std::vector<unsigned char> &target, const std::vector<unsigned char> &value) {
    target.insert(target.end(), value.begin(), value.end());
}
} // namespace

int main() {
    {
        EventLoop loop(1, 8, 32);
        std::mutex holdMutex;
        std::condition_variable holdCondition;
        unsigned heldCount = 0;
        unsigned shortPressCount = 0;
        RemoteHomeHoldTimer holdTimer(loop, [&]() {
            std::lock_guard<std::mutex> lock(holdMutex);
            ++heldCount;
            holdCondition.notify_all();
        }, EventLoop::Milliseconds(40), [&]() {
            std::lock_guard<std::mutex> lock(holdMutex);
            ++shortPressCount;
            holdCondition.notify_all();
        });
        std::thread loopThread([&loop]() { loop.run(); });

        holdTimer.onKeyEvent(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        holdTimer.onKeyEvent(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        {
            std::lock_guard<std::mutex> lock(holdMutex);
            assert(heldCount == 0 && "a short Home press must cancel the hold action");
            assert(shortPressCount == 1 && "a released short Home press must be reported once");
        }

        holdTimer.onKeyEvent(true);
        {
            std::unique_lock<std::mutex> lock(holdMutex);
            assert(holdCondition.wait_for(lock, std::chrono::milliseconds(500), [&]() {
                return heldCount == 1;
            }));
        }
        holdTimer.onKeyEvent(false);
        holdTimer.stop();
        loop.stop();
        loopThread.join();
        std::lock_guard<std::mutex> lock(holdMutex);
        assert(heldCount == 1 && "one held Home press must emit one hold action");
        assert(shortPressCount == 1 && "a completed hold must not be reported as a short press");
    }

    RemoteButtonsParser parser32(16);
    const std::vector<unsigned char> red = event(16, 1, 398, 1);
    const std::vector<RemoteButtonEvent> partial = parser32.feed(red.data(), 9);
    assert(partial.empty());
    const std::vector<RemoteButtonEvent> completed = parser32.feed(red.data() + 9, 7);
    assert(completed.size() == 1);
    assert(completed[0].type == RemoteButtonEventType::ColorButton);
    assert(completed[0].button == "red" && completed[0].code == 398 &&
           completed[0].pressed);

    std::vector<unsigned char> records;
    append(records, event(16, 1, 773, 1));
    append(records, event(16, 1, 773, 0));
    append(records, event(16, 1, 28, 1));
    append(records, event(16, 1, 399, 2));
    const std::vector<RemoteButtonEvent> keys = parser32.feed(records.data(), records.size());
    assert(keys.size() == 3);
    assert(keys[0].type == RemoteButtonEventType::HomeKey && keys[0].pressed);
    assert(keys[1].type == RemoteButtonEventType::HomeKey && !keys[1].pressed);
    assert(keys[2].type == RemoteButtonEventType::Key && keys[2].code == 28);

    RemoteButtonsParser parser64(24);
    const std::vector<unsigned char> yellow = event(24, 1, 400, 1);
    const std::vector<RemoteButtonEvent> parsed64 =
        parser64.feed(yellow.data(), yellow.size());
    assert(parsed64.size() == 1 && parsed64[0].button == "yellow");

    const std::vector<RemoteButtonEvent> qt =
        parser64.parseQtLogLine("inputcommon: D_KEY_PRESS 18874388");
    assert(qt.size() == 1 && qt[0].button == "blue" && qt[0].code == 18874388);
    const std::vector<RemoteButtonEvent> irrelevant =
        parser64.parseQtLogLine("inputcommon: key released 18874388");
    assert(irrelevant.empty());
    const std::vector<RemoteButtonEvent> twoQt =
        parser64.parseQtLogLine("pressed 18874385 and 18874387");
    assert(twoQt.size() == 2 && twoQt[0].button == "red" &&
           twoQt[1].button == "yellow");

    char root[] = "/tmp/nativetv-remote-buttons-XXXXXX";
    assert(mkdtemp(root) != nullptr);
    const std::string inputDirectory = std::string(root) + "/input";
    assert(mkdir(inputDirectory.c_str(), 0700) == 0);
    const std::string devicePath = inputDirectory + "/event1";
    const std::vector<unsigned char> green =
        event(RemoteButtonsParser::platformEventSize(), 1, 399, 1);
    {
        std::ofstream device(devicePath, std::ios::binary);
        device.write(reinterpret_cast<const char *>(green.data()),
                     static_cast<std::streamsize>(green.size()));
        assert(device.good());
    }
    RemoteButtonsReaderOptions options;
    options.inputDirectory = inputDirectory;
    options.inputDevicesPath = std::string(root) + "/devices";
    options.inputLogPath.clear();
    options.scanIntervalMilliseconds = 10;
    options.pollTimeoutMilliseconds = 20;
    std::mutex mutex;
    std::condition_variable condition;
    std::string observedButton;
    RemoteButtonsReader reader([&](const RemoteButtonEvent &received) {
        std::lock_guard<std::mutex> lock(mutex);
        if (received.type == RemoteButtonEventType::ColorButton)
            observedButton = received.button;
        condition.notify_all();
    }, options);
    assert(RemoteButtonsReader::isSupported(options));
    assert(reader.start());
    {
        std::unique_lock<std::mutex> lock(mutex);
        assert(condition.wait_for(lock, std::chrono::seconds(2), [&] {
            return observedButton == "green";
        }));
    }
    reader.stop();
    assert(!reader.running());
    unlink(devicePath.c_str());
    rmdir(inputDirectory.c_str());
    rmdir(root);
    return 0;
}
