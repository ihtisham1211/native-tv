#include "adapters/http_services.hpp"
#include "adapters/readiness_retry.hpp"
#include "domain/models.hpp"
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>

using namespace nativetv;
namespace {
int checks = 0;
void check(bool value, const char *message) {
    ++checks;
    if (!value)
        throw std::runtime_error(message);
}
Response wait(HttpServices &services, const std::string &channel) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (std::chrono::steady_clock::now() < deadline) {
        for (auto &reply : services.drain())
            if (reply.channel == channel)
                return reply;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    throw std::runtime_error("Reply did not arrive: " + channel);
}
} // namespace
int main(int argc, char **argv) {
    try {
        if (argc != 2)
            throw std::runtime_error("Fixture origin required");
        bool rejected = false;
        try {
            HttpServices invalid("https://invalid");
        } catch (const std::exception &) {
            rejected = true;
        }
        check(rejected, "The adapter rejects unsupported origins before allocating workers");
        using Retry = ReadinessRetry;
        check(Retry::delay(0) == std::chrono::milliseconds(100) &&
                  Retry::delay(1) == std::chrono::milliseconds(250) &&
                  Retry::delay(2) == std::chrono::milliseconds(500) &&
                  Retry::delay(3) == std::chrono::milliseconds(800) &&
                  Retry::delay(4) == std::chrono::milliseconds(0),
              "Readiness retries use increasing bounded backoff");
        check(Retry::shouldRetry(0, Retry::retryable(false, 503), std::chrono::milliseconds(0)) &&
                  !Retry::shouldRetry(0, Retry::retryable(false, 404),
                                      std::chrono::milliseconds(0)) &&
                  !Retry::shouldRetry(4, true, std::chrono::milliseconds(0)),
              "Only transient failures retry, up to the fixed retry limit");
        check(Retry::attemptTimeoutMs(std::chrono::milliseconds(0)) == 1000 &&
                  Retry::attemptTimeoutMs(std::chrono::milliseconds(2000)) == 500 &&
                  Retry::attemptTimeoutMs(Retry::budget()) == 0 &&
                  !Retry::shouldRetry(0, true, Retry::budget()),
              "Readiness attempts stop at the overall timeout budget");
        HttpServices services(argv[1]);
        check(services.write("none", Operation::None, Json::object()) == 0 &&
                  services.read("none", Operation::None) == 0,
              "An operation with no route does not start, and does not throw into the screen");
        services.read("caps", Operation::Capabilities);
        auto response = wait(services, "caps");
        check(response.ok() && response.json()["ok"].boolean(),
              "A transient readiness response is retried until the server answers");
        services.read("caps-exhausted", Operation::Capabilities);
        auto readinessStarted = std::chrono::steady_clock::now();
        response = wait(services, "caps-exhausted");
        auto readinessElapsed = std::chrono::steady_clock::now() - readinessStarted;
        check(!response.ok() && response.status == 503 &&
                  readinessElapsed >= std::chrono::milliseconds(1400) &&
                  readinessElapsed < std::chrono::seconds(3),
              "Exhausted readiness retries return the final server error within the time budget");
        services.read("search", Operation::SearchTitles, fields({{"query", Json::text("é & /?")}}));
        response = wait(services, "search");
        check(response.ok() && response.json()["query"].string() == "é & /?",
              "Query encoding survives real HTTP transport");
        services.write("action", Operation::Control,
                       fields({{"action", Json::text("mute")}, {"value", Json::flag(true)}}));
        response = wait(services, "action");
        check(response.ok() && response.json()["body"]["value"].boolean(),
              "JSON POST preserves booleans and content type");
        services.write("setup-done", Operation::Setup, fields({{"action", Json::text("done")}}));
        response = wait(services, "setup-done");
        check(response.ok() && response.json()["path"].string() == "/api/setup/done" &&
                  response.json()["body"]["action"].string() == "done",
              "Setup completion uses the native route and preserves its action body");
        services.write("setup-network", Operation::Setup,
                       fields({{"action", Json::text("network")},
                               {"open", Json::flag(true)}}));
        response = wait(services, "setup-network");
        check(response.ok() && response.json()["path"].string() == "/api/setup/network" &&
                  response.json()["body"]["open"].boolean(),
              "Network setup uses its native route and preserves the selected value");
        services.write("setup-always-ready", Operation::Setup,
                       fields({{"action", Json::text("alwaysReady")}, {"on", Json::flag(true)}}));
        response = wait(services, "setup-always-ready");
        check(response.ok() && response.json()["path"].string() == "/api/setup/always-ready" &&
                  response.json()["body"]["on"].boolean(),
              "Always Ready setup uses the native route and preserves its setting");
        services.write("setup-handoff", Operation::Setup,
                       fields({{"action", Json::text("handoff")}}));
        response = wait(services, "setup-handoff");
        check(response.ok() && response.json()["path"].string() == "/api/setup/handoff" &&
                  response.json()["body"]["action"].string() == "handoff",
              "Handoff start uses the native route and preserves its action body");
        services.write("setup-handoff-stop", Operation::Setup,
                       fields({{"action", Json::text("handoffStop")}}));
        response = wait(services, "setup-handoff-stop");
        check(response.ok() && response.json()["path"].string() == "/api/setup/handoff-stop" &&
                  response.json()["body"]["action"].string() == "handoffStop",
              "Handoff stop uses the native route and preserves its action body");
        services.read("search", Operation::SearchTitles, fields({{"query", Json::text("slow")}}));
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        services.read("search", Operation::SearchTitles, fields({{"query", Json::text("latest")}}));
        response = wait(services, "search");
        check(response.json()["query"].string() == "latest",
              "A delayed obsolete reply cannot replace the latest result");
        services.read("error", Operation::Privacy);
        response = wait(services, "error");
        check(!response.ok() && response.status == 503 &&
                  response.json()["error"].string() == "Unavailable",
              "Service errors retain status and message");
        services.image("app:test.app");
        response = wait(services, "image:app:test.app");
        check(response.ok() && response.path == "app:test.app" && response.bytes == "image",
              "Artwork keeps its opaque native reference across the adapter");
        services.read("now", Operation::HomeNow);
        response = wait(services, "now");
        check(response.ok() && response.json()["port"].number() == 4,
              "Current-source reads use the Home source endpoint");
        services.read("picture", Operation::HomePicture,
                      fields({{"kind", Json::text("app")}, {"app", Json::text("app & name")}}));
        response = wait(services, "picture");
        check(response.ok() &&
                  response.json()["query"]["app"][static_cast<size_t>(0)].string() == "app & name",
              "Picture reads preserve the encoded source selector");
        services.image("recent:7:app.one");
        response = wait(services, "image:recent:7:app.one");
        check(response.ok() &&
                  response.json()["query"]["id"][static_cast<size_t>(0)].string() == "app.one" &&
                  response.json()["query"]["v"][static_cast<size_t>(0)].string() == "7",
              "Snapshot revisions refresh the cache without changing the app ID");
        services.image("wallpaper:morning@revision-1");
        response = wait(services, "image:wallpaper:morning@revision-1");
        check(response.ok() &&
                  response.json()["query"]["slot"][static_cast<size_t>(0)].string() == "morning" &&
                  response.json()["query"]["v"][static_cast<size_t>(0)].string() == "revision-1",
              "Wallpaper requests preserve the slot and refresh changed uploads by revision");
        services.write("beat", Operation::HomeBeat, Json::object());
        response = wait(services, "beat");
        check(response.ok() && response.json()["path"].string() == "/api/home/beat",
              "Home's heartbeat posts to its route");
        for (int i = 0; i < 200 && !services.idle(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        check(services.idle(), "With everything answered and drained the adapter reports idle");
        services.read("big", Operation::Apps);
        response = wait(services, "big");
        check(!response.ok() && response.bytes.size() <= 8 * 1024 * 1024,
              "Oversized responses abort before growing beyond the byte budget");
        auto started = std::chrono::steady_clock::now();
        {
            HttpServices closing(argv[1]);
            closing.read("slow", Operation::SearchTitles, fields({{"query", Json::text("hang")}}));
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        check(std::chrono::steady_clock::now() - started < std::chrono::seconds(3),
              "Shutdown cancels an in-flight connection");
        std::printf("%d native HTTP checks passed\n", checks);
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
