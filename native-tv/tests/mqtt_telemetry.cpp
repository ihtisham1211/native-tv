#include "daemon/mqtt_telemetry_publisher.hpp"
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace nativetv;
using namespace nativetv::daemon;

struct Published {
    std::string topic;
    Json payload;
    bool retained;
    std::chrono::steady_clock::time_point at = std::chrono::steady_clock::now();
};

size_t telemetryCount(const std::vector<Published> &messages) {
    size_t count = 0;
    for (const Published &message : messages)
        if (message.topic == "test/tv/telemetry")
            ++count;
    return count;
}
} // namespace

int main() {
    EventLoop loop(1, 8, 32);
    state::Cache cache;
    std::mutex mutex;
    std::condition_variable condition;
    std::vector<Published> messages;
    MqttTelemetryPublisher publisher(loop, cache, MqttTopics("test/tv"),
        [&](const std::string &topic, const std::string &payload, bool retained) {
            std::lock_guard<std::mutex> lock(mutex);
            messages.push_back({topic, Json::parse(payload), retained});
            condition.notify_all();
            return true;
        });
    Json base = Json::object();
    base.set("tvwebVersion", Json::text("test-version"));
    Json device = Json::object();
    device.set("id", Json::text("test-tv"));
    device.set("name", Json::text("Test TV"));
    base.set("device", device);
    publisher.setBase(base);
    Json system = Json::object();
    system.set("ok", Json::flag(true));
    system.set("temp", Json::numeric(59));
    system.set("tempMillidegrees", Json::numeric(59000));
    system.set("uptime", Json::numeric(3600));
    system.set("mem", Json::parse(R"json({"total":2048,"avail":768})json"));
    system.set("swap", Json::parse(R"json({"total":512,"free":256})json"));
    system.set("wifi", Json::parse(R"json({"link":70,"level":-52})json"));
    system.set("remote", Json::parse(R"json({"battery":74,"paired":true})json"));
    system.set("emmc", Json::parse(R"json({"health":"Healthy"})json"));
    system.set("gpuMhz", Json::numeric(300));
    system.set("gpuHz", Json::numeric(300000000));
    system.set("cpuHz", Json::numeric(1400000000));
    system.set("mhz", Json::numeric(1400));
    system.set("appStorage", Json::parse(R"json({"freeMb":4096})json"));
    system.set("loadavg", Json::parse(R"json(["1.0","0.5","0.2"])json"));
    system.set("net", Json::parse(
        R"json({"iface":"eth0","rx":1000,"tx":500,"mac":"aa:bb:cc:dd:ee:ff"})json"));
    system.set("cpu", Json::parse(
        R"json({"coresTotal":2,"currentLoad":19,"coreLoads":[18,21],"times":{"btime":1790000000,"cpus":{}}})json"));
    system.set("pictureEngine", Json::parse(
        R"json({"colorimetry":"BT.2020","hdr_mode":"hdr10"})json"));
    publisher.setSystemSnapshot(system);
    assert(publisher.start());
    std::thread loopThread([&] { loop.run(); });

    std::int64_t time = 1;
    const auto update = [&](const std::string &group, const std::string &key,
                            const Json &value) {
        assert(cache.update({group, key, value, time++, false, true}));
    };
    update("application", "app_id", Json::text("netflix"));
    update("application", "app", Json::text("netflix"));
    update("application", "display_title", Json::text("Netflix"));
    update("power", "state", Json::text("Active"));
    update("picture", "pictureMode", Json::text("hdrGame"));
    update("picture", "backlight", Json::text("75"));
    update("audio", "volume", Json::numeric(22));
    update("audio", "muted", Json::flag(false));
    update("audio", "output", Json::text("TV Speaker"));
    Json media = Json::object();
    media.set("state", Json::text("playing"));
    media.set("playerType", Json::text("MSE"));
    media.set("fullScreen", Json::flag(true));
    update("media", "snapshot", Json::text(media.dump()));

    {
        std::unique_lock<std::mutex> lock(mutex);
        const bool published = condition.wait_for(lock, std::chrono::seconds(2), [&] {
            for (const Published &message : messages) {
                if (message.topic == "test/tv/telemetry" &&
                    message.payload["app_id"].string() == "netflix" &&
                    message.payload["picture"]["backlight"].number(-1) == 75 &&
                    message.payload["media"]["state"].string() == "playing")
                    return true;
            }
            return false;
        });
        assert(published);
        assert(telemetryCount(messages) == 1);
        const Json payload = messages.back().payload;
        assert(messages.back().topic == "test/tv/telemetry");
        assert(messages.back().retained);
        assert(payload["tvwebVersion"].string() == "test-version");
        assert(payload["device"]["id"].string() == "test-tv");
        assert(payload["device"]["name"].string() == "Test TV");
        assert(payload["time"].isNumber());
        assert(payload["temp"].number() == 59);
        assert(payload["tempMillidegrees"].number() == 59000);
        assert(payload["mem"]["avail"].number() == 768);
        assert(payload["swap"]["free"].number() == 256);
        assert(payload["uptime"].number() == 3600);
        assert(payload["load"].number() == 19);
        assert(payload["loadPeak"].number() == 21);
        assert(payload["cores"].size() == 2);
        assert(payload["coresTotal"].number() == 2);
        assert(payload["bootTime"].string() == "2026-09-21T14:13:20Z");
        assert(payload["netTotal"]["rx"].number() == 1000);
        assert(payload["mac"].string() == "aa:bb:cc:dd:ee:ff");
        assert(payload["remote"]["battery"].number() == 74);
        assert(payload["gpuMhz"].number() == 300);
        assert(payload["appStorage"]["freeMb"].number() == 4096);
        assert(payload["picture_engine"]["hdr_mode"].string() == "hdr10");
        assert(payload["colorimetry"].string() == "BT.2020");
        assert(payload["app"].string() == "netflix");
        assert(payload["display_title"].string() == "Netflix");
        assert(payload["powerState"]["raw"].string() == "Active");
        assert(payload["powerState"]["label"].string() == "On");
        assert(payload["tvOff"].isBoolean() && !payload["tvOff"].boolean());
        assert(payload["picture"]["mode_raw"].string() == "hdrGame");
        assert(payload["picture"]["mode"].string() == "HDR Game");
        assert(payload["volume"].number() == 22);
        assert(payload["muted"].isBoolean() && !payload["muted"].boolean());
        assert(payload["audio_output"].string() == "TV Speaker");
        assert(payload["sound"]["output"].string() == "TV Speaker");
        assert(payload["media"]["playerType"].string() == "MSE");
    }

    update("power", "systemOn", Json::flag(false));
    {
        std::unique_lock<std::mutex> lock(mutex);
        const bool published = condition.wait_for(lock, std::chrono::seconds(1), [&] {
            return telemetryCount(messages) == 2 && messages.back().payload["tvOff"].boolean();
        });
        assert(published);
        assert(messages.back().retained);
    }

    // test-state-push.js: a change soon after a publish goes out no sooner than the
    // gap after it (1.5 s), and changes close together go out as one.
    std::chrono::steady_clock::time_point secondAt;
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (const Published &message : messages)
            if (message.topic == "test/tv/telemetry") secondAt = message.at;
    }
    update("audio", "volume", Json::numeric(30));
    update("audio", "volume", Json::numeric(31));
    update("audio", "volume", Json::numeric(32));
    {
        std::unique_lock<std::mutex> lock(mutex);
        const bool published = condition.wait_for(lock, std::chrono::seconds(4), [&] {
            return telemetryCount(messages) >= 3;
        });
        assert(published);
        assert(telemetryCount(messages) == 3);  // three changes, one publish
        assert(messages.back().payload["volume"].number() == 32);
        assert(messages.back().at - secondAt >= std::chrono::milliseconds(1300));
    }

    publisher.stop();
    loop.stop();
    loopThread.join();
    return 0;
}
