#include "daemon/mqtt_state_publisher.hpp"
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace nativetv;
using namespace nativetv::daemon;

struct Published {
    std::string topic;
    std::string payload;
    bool retained;
};

Json text(const std::string &input) {
    Json value = Json::parse(input);
    if (!value.valid())
        throw std::runtime_error("invalid test JSON");
    return value;
}

bool contains(const std::vector<Published> &messages, const std::string &topic,
              const std::string &payload, size_t *index = nullptr) {
    for (size_t i = 0; i < messages.size(); ++i) {
        if (messages[i].topic == topic && messages[i].payload == payload &&
            messages[i].retained) {
            if (index)
                *index = i;
            return true;
        }
    }
    return false;
}
} // namespace

int main() {
    const MqttTopics topics("test/tv");
    assert(topics.status() == "test/tv/status");
    assert(topics.telemetry() == "test/tv/telemetry");
    assert(topics.commandPrefix() == "test/tv/command/");
    assert(topics.command("volume") == "test/tv/command/volume");
    assert(topics.state("power/screenOn") == "test/tv/state/power/screenOn");
    assert(topics.eventsButton() == "test/tv/events/button");
    assert(MqttTopics().status() == "lgtv/status");

    EventLoop loop(1, 8, 32);
    state::Cache cache;
    std::mutex mutex;
    std::condition_variable condition;
    std::vector<Published> messages;
    MqttStatePublisher publisher(loop, cache, topics,
        [&](const std::string &topic, const std::string &payload, bool retained) {
            std::lock_guard<std::mutex> lock(mutex);
            messages.push_back({topic, payload, retained});
            condition.notify_all();
            return true;
        });
    assert(publisher.start());
    std::thread loopThread([&] { loop.run(); });

    const auto update = [&](const std::string &group, const std::string &key,
                            const std::string &value, std::int64_t time) {
        assert(cache.update({group, key, text(value), time, false, true}));
    };
    const auto waitFor = [&](const std::string &topic, const std::string &payload) {
        std::unique_lock<std::mutex> lock(mutex);
        return condition.wait_for(lock, std::chrono::seconds(2), [&] {
            return contains(messages, topic, payload);
        });
    };

    update("application", "app_id", "\"netflix\"", 1);
    update("power", "systemOn", "true", 2);
    update("power", "screenOn", "true", 3);
    update("surface", "control_app_id", "\"com.webos.app.youtube\"", 4);
    update("focus", "control_app_id", "\"com.palm.app.settings\"", 5);
    {
        std::lock_guard<std::mutex> lock(mutex);
        assert(contains(messages, "test/tv/state/application/app_id", "netflix"));
        assert(contains(messages, "test/tv/state/power/systemOn", "true"));
        assert(contains(messages, "test/tv/state/power/screenOn", "true"));
        assert(contains(messages, "test/tv/state/screen", "ON"));
        assert(contains(messages, "test/tv/state/application/remote",
                        "{\"app_id\":\"netflix\",\"control_app_id\":\"com.palm.app.settings\"}"));
        for (const Published &message : messages)
            assert(message.retained);
    }

    update("focus", "control_app_id", "\"\"", 6);
    {
        std::lock_guard<std::mutex> lock(mutex);
        assert(!contains(messages, "test/tv/state/application/remote",
                         "{\"app_id\":\"netflix\",\"control_app_id\":\"\"}"));
    }
    assert(waitFor("test/tv/state/application/remote",
                   "{\"app_id\":\"netflix\",\"control_app_id\":\"\"}"));

    update("power", "systemOn", "false", 7);
    update("power", "screenOn", "false", 8);
    assert(waitFor("test/tv/state/screen", "OFF"));
    // test-power-state.js: a transient screenOn while the system is off stays OFF.
    update("power", "screenOn", "true", 9);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    {
        std::lock_guard<std::mutex> lock(mutex);
        std::string last;
        for (const Published &message : messages)
            if (message.topic == "test/tv/state/screen") last = message.payload;
        assert(last == "OFF");
    }
    publisher.publishSnapshot();
    {
        std::lock_guard<std::mutex> lock(mutex);
        size_t before = 0;
        assert(contains(messages, "test/tv/state/application/remote",
                        "{\"app_id\":\"netflix\",\"control_app_id\":\"\"}", &before));
        size_t after = 0;
        for (size_t i = before + 1; i < messages.size(); ++i) {
            if (messages[i].topic == "test/tv/state/application/remote")
                after = i;
        }
        assert(after > before);
    }

    // The daemon's player snapshot is its own state, not a topic the server
    // publishes (a B3 showed it on the user's broker), and a snapshot clears the
    // retained release state that servers before this one left at <prefix>/update.
    update("media", "snapshot", "\"{\\\"state\\\":\\\"idle\\\"}\"", 10);
    publisher.publishSnapshot();
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (const Published &message : messages)
            assert(message.topic.find("/state/media/") == std::string::npos);
        assert(contains(messages, "test/tv/update", ""));
    }

    // test-window-state.js: window transitions settle without hiding a lasting
    // loss of focus. A fresh cache and publisher on the same running loop.
    {
        state::Cache windows;
        std::mutex windowMutex;
        std::vector<Published> remote;
        MqttStatePublisher windowPublisher(loop, windows, topics,
            [&](const std::string &topic, const std::string &payload, bool retained) {
                std::lock_guard<std::mutex> lock(windowMutex);
                if (topic == "test/tv/state/application/remote") {
                    assert(retained);
                    remote.push_back({topic, payload, retained});
                }
                return true;
            });
        assert(windowPublisher.start());
        std::int64_t sourceTime = 100;
        const auto set = [&](const std::string &group, const std::string &key,
                             const std::string &value) {
            assert(windows.update({group, key, text(value), ++sourceTime, false, true}));
        };
        const auto focus = [&](const std::string &id) {
            set("focus", "control_app_id", "\"" + id + "\"");
        };
        const auto sleep = [](int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); };
        const auto count = [&] {
            std::lock_guard<std::mutex> lock(windowMutex);
            return remote.size();
        };
        const auto last = [&] {
            std::lock_guard<std::mutex> lock(windowMutex);
            return remote.empty() ? std::string() : remote.back().payload;
        };
        const auto lastIs = [&](const std::string &app, const std::string &control) {
            return last() == "{\"app_id\":\"" + app + "\",\"control_app_id\":\"" + control + "\"}";
        };
        const auto waitForLast = [&](const std::string &app, const std::string &control) {
            for (int i = 0; i < 100 && !lastIs(app, control); ++i)
                sleep(10);
            return lastIs(app, control);
        };

        set("application", "app_id", "\"com.webos.app.hdmi3\"");
        set("power", "systemOn", "true");
        focus("com.palm.app.settings");
        assert(lastIs("com.webos.app.hdmi3", "com.palm.app.settings"));

        // Closing a window reports no focus for about 100 ms before HDMI regains
        // it: that must not reach the remote, even when another window event
        // arrives meanwhile.
        size_t before = count();
        focus("");
        sleep(100);
        set("surface", "control_app_id", "\"com.webos.app.hdmi3\"");
        assert(count() == before);
        focus("com.webos.app.hdmi3");
        assert(count() == before + 1);
        assert(lastIs("com.webos.app.hdmi3", "com.webos.app.hdmi3"));
        sleep(350);
        assert(count() == before + 1);
        set("surface", "control_app_id", "\"com.webos.app.home\"");
        assert(count() == before + 1);

        // An empty focus that lasts releases the remote, with the current app.
        focus("");
        sleep(100);
        assert(lastIs("com.webos.app.hdmi3", "com.webos.app.hdmi3"));
        set("application", "app_id", "\"netflix\"");
        assert(waitForLast("netflix", ""));

        // A reconnect republishes even an unchanged empty state, and a focused
        // window at once.
        size_t republished = count();
        windowPublisher.publishSnapshot();
        for (int i = 0; i < 100 && count() == republished; ++i)
            sleep(10);
        assert(count() == republished + 1);
        focus("netflix");
        republished = count();
        windowPublisher.publishSnapshot();
        assert(count() == republished + 1);

        // Standby releases the remote at once, cancels the pending empty
        // publication, and a late window event cannot restore it.
        focus("");
        sleep(50);
        set("power", "systemOn", "false");
        assert(lastIs("netflix", ""));
        republished = count();
        sleep(350);
        assert(count() == republished);
        focus("com.webos.app.home");
        assert(count() == republished);
        set("power", "systemOn", "true");
        assert(lastIs("netflix", "com.webos.app.home"));

        // A reconnect during an empty transition still republishes the settled state.
        focus("");
        windowPublisher.publishSnapshot();
        republished = count();
        focus("com.webos.app.home");
        assert(count() == republished + 1);
        sleep(350);
        assert(count() == republished + 1);
        windowPublisher.stop();
    }

    // test-state-push.js: a TV without SurfaceManager's window reports leaves
    // control_app_id out, on or off.
    {
        state::Cache older;
        std::mutex olderMutex;
        std::vector<std::string> remote;
        MqttStatePublisher olderPublisher(loop, older, topics,
            [&](const std::string &topic, const std::string &payload, bool) {
                std::lock_guard<std::mutex> lock(olderMutex);
                if (topic == "test/tv/state/application/remote")
                    remote.push_back(payload);
                return true;
            });
        assert(olderPublisher.start());
        assert(older.update({"application", "app_id", text("\"netflix\""), 1, false, true}));
        assert(older.update({"power", "systemOn", text("false"), 2, false, true}));
        olderPublisher.stop();
        std::lock_guard<std::mutex> lock(olderMutex);
        assert(!remote.empty() && remote.front() == "{\"app_id\":\"netflix\"}");
        assert(remote.back() == "{\"app_id\":\"netflix\"}");
    }

    publisher.stop();
    loop.stop();
    loopThread.join();
    return 0;
}
