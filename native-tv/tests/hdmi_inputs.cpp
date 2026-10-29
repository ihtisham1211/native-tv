#include "service/hdmi_inputs.hpp"
#include "service/system_metrics.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

// Fixture folders are made and removed with the shell; a failure shows in the checks that read them.
static void runShell(const std::string &command) {
    if (std::system(command.c_str()) != 0) {
    }
}


using namespace nativetv;
using namespace nativetv::service;

namespace {
unsigned checks = 0;

void check(bool value, const char *message) {
    ++checks;
    if (!value)
        throw std::runtime_error(message);
}

Json fact(bool connected, unsigned width, unsigned height, unsigned refresh,
          double pixelClock, const std::string &depth) {
    Json timing = Json::object();
    timing.set("connected", Json::flag(connected));
    timing.set("width", connected ? Json::numeric(width) : Json());
    timing.set("height", connected ? Json::numeric(height) : Json());
    timing.set("refreshHz", connected ? Json::numeric(refresh) : Json());
    timing.set("pixelClockKhz", connected ? Json::numeric(pixelClock) : Json());
    Json result = Json::object();
    result.set("timing", timing);
    result.set("colorDepth", connected ? Json::text(depth) : Json());
    result.set("interlaced", Json::flag(false));
    return result;
}

Json device(const std::string &id, unsigned port, const std::string &label,
            bool active, bool plugged) {
    Json result = Json::object();
    result.set("id", Json::text(id));
    result.set("port", Json::numeric(port));
    result.set("label", Json::text(label));
    result.set("activate", Json::flag(active));
    result.set("hdmiPlugIn", Json::flag(plugged));
    return result;
}
// A root whose receivers' status files are a real TV's (tests/fixtures/tv/hdmi20-<tv>).
std::string rootWith(const char *tv) {
    char pattern[] = "/tmp/nativetv-hdmi-map-XXXXXX";
    const std::string root = mkdtemp(pattern);
    for (int port = 0; port < 4; ++port) {
        const std::string folder = root + "/proc/lg/hdmi20/port" + std::to_string(port);
        runShell(("mkdir -p '" + folder + "'"));
        std::ifstream in(std::string(NATIVETV_REPO_ROOT) + "/native-tv/tests/fixtures/tv/hdmi20-" + tv + "/port" +
                         std::to_string(port) + ".status");
        std::ostringstream text;
        text << in.rdbuf();
        std::ofstream(folder + "/status") << text.str();
    }
    return root;
}

Json configdMap(const std::string &assignment) {
    return Json::parse("{\"returnValue\":true,\"configs\":{\"inputMap.videoInputMapIndexInfo0\":["
                       "{\"assignment\":" + assignment + "}]}}");
}

Json deviceList() {
    Json devices = Json::array();
    devices.add(device("HDMI_1", 1, "Streamer", true, true));
    devices.add(device("HDMI_2", 2, "HDMI 2", false, false));
    devices.add(device("HDMI_3", 3, "HDMI 3", false, false));
    devices.add(device("HDMI_4", 4, "PC", true, true));
    return devices;
}

} // namespace

int main() {
    try {
        const Json config = Json::parse(
            "{\"returnValue\":true,\"configs\":{\"inputMap.videoInputMapIndexInfo0\":["
            "{\"assignment\":{\"hdmi1\":\"3\",\"hdmi2\":\"2\","
            "\"hdmi3\":\"1\",\"hdmi4\":\"0\"}}]}}");
        const Json map = hdmiInputMapFromConfig(config);
        check(map["1"].number() == 3 && map["4"].number() == 0,
              "Configd receiver map");
        check(hdmiInputMapFromConfig(Json::parse("{\"returnValue\":false}"))["2"].number() == 2,
              "Base map fallback on configd refusal");
        const Json absentMap = hdmiInputMapFromConfig(Json::parse(
            "{\"returnValue\":true,\"configs\":{\"inputMap.videoInputMapIndexInfo0\":["
            "{\"assignment\":{\"hdmi1\":\"none\",\"hdmi2\":\"none\","
            "\"hdmi3\":\"none\",\"hdmi4\":\"none\"}}]}}"));
        check(absentMap["1"].number() == 3, "Base map fallback when no input is assigned");

        Json snapshot = Json::object(), rawPorts = Json::array(), facts = Json::array();
        rawPorts.add(Json::text("receiver 0"));
        rawPorts.add(Json());
        rawPorts.add(Json());
        rawPorts.add(Json::text("receiver 3"));
        facts.add(fact(true, 3840, 2160, 120, 1188000, "10BIT"));
        facts.add(Json());
        facts.add(Json());
        facts.add(fact(true, 1920, 1080, 60, 148500, "8BIT"));
        snapshot.set("hdmiPorts", rawPorts);
        snapshot.set("hdmiPortFacts", facts);

        Json devices = Json::array();
        devices.add(device("HDMI_1", 1, "Streamer", true, true));
        devices.add(device("HDMI_2", 2, "HDMI 2", false, false));
        devices.add(device("HDMI_4", 4, "PC", true, true));
        Json noDevice = device("HDMI_3", 3, "HDMI 3", false, false);
        noDevice.set("lastUniqueId", Json::numeric(255));
        devices.add(noDevice);

        Json foreground = Json::object();
        foreground.set("appId", Json::text("com.webos.app.hdmi1"));
        const Json result = hdmiInputs(snapshot, devices, foreground, map);
        const Json inputs = result["inputs"];
        const Json ports = result["ports"];
        check(result["ok"].boolean() && inputs.size() == 4 && ports.size() == 2,
              "HDMI response shape includes present ports and HDMI inputs");
        check(inputs[size_t(0)]["active"].boolean() &&
                  inputs[size_t(0)]["deviceSeen"].boolean() &&
                  inputs[size_t(0)]["appId"].string() == "com.webos.app.hdmi1",
              "Input state and synthesized app ID");
        check(inputs[size_t(0)]["signal"]["port"].number() == 3 &&
                  inputs[size_t(0)]["signal"]["resolution"].string() == "1920x1080" &&
                  inputs[size_t(0)]["signal"]["pixelClockMhz"].number() == 148.5,
              "Signal follows the receiver assigned to the input");
        check(!inputs[size_t(1)]["signal"].valid() &&
                  !inputs[size_t(1)]["deviceSeen"].boolean(),
              "Idle input does not inherit another receiver's signal");
        check(!inputs[size_t(2)]["active"].boolean() &&
                  inputs[size_t(2)]["signal"]["port"].number() == 0 &&
                  inputs[size_t(2)]["signal"]["resolution"].string() == "3840x2160",
              "Non-foreground input retains its own connected signal");
        check(!inputs[size_t(3)]["deviceSeen"].boolean() &&
                  ports[size_t(0)]["pixelClockMhz"].number() == 1188,
              "No CEC device is not seen and port clocks use MHz");

        // test-hdmi-map.js: an input the map marks none has no receiver and no signal,
        // even with another receiver live; a TV without a map takes the base table.
        const Json partial = hdmiInputMapFromConfig(Json::parse(
            "{\"returnValue\":true,\"configs\":{\"inputMap.videoInputMapIndexInfo0\":["
            "{\"assignment\":{\"hdmi1\":\"3\",\"hdmi2\":\"2\","
            "\"hdmi3\":\"1\",\"hdmi4\":\"none\"}}]}}"));
        check(partial["1"].number() == 3 && partial["3"].number() == 1 && !partial["4"].valid(),
              "An input the map marks none has no receiver");
        const Json partialInputs = hdmiInputs(snapshot, devices, foreground, partial)["inputs"];
        check(!partialInputs[size_t(3)]["signal"].valid() &&
                  partialInputs[size_t(0)]["signal"]["port"].number() == 3,
              "An input without a receiver takes no other input's signal");
        const Json bare = hdmiInputMapFromConfig(Json::parse(
            "{\"returnValue\":true,\"configs\":{},"
            "\"missingConfigs\":[\"inputMap.videoInputMapIndexInfo0\"]}"));
        check(bare["1"].number() == 3 && bare["2"].number() == 2 && bare["3"].number() == 1 &&
                  bare["4"].number() == 0,
              "A TV whose configd has no map takes the base table");
        check(hdmiInputMapFromConfig(Json::parse("{\"returnValue\":false,\"errorText\":\"Unknown method\"}"))["4"].number() == 0 &&
                  hdmiInputMapFromConfig(Json())["1"].number() == 3,
              "A refusal or no answer takes the base table");

        // Each input's signal comes from its own receiver, by configd's map (test-hdmi-map.js).
        const Json noForeground = Json::object();
        {
            const std::string root = rootWith("c4");
            const Json system = collectSystem(root, root);
            const Json c4 = hdmiInputMapFromConfig(configdMap(
                "{\"hdmi1\":\"3\",\"hdmi2\":\"2\",\"hdmi3\":\"1\",\"hdmi4\":\"0\",\"av1\":\"2\",\"comp1\":\"1\"}"));
            check(c4["1"].number() == 3 && c4["2"].number() == 2 && c4["3"].number() == 1 && c4["4"].number() == 0,
                  "a C4's input map puts HDMI 1 to 4 on receivers 3, 2, 1 and 0");
            const Json r = hdmiInputs(system, deviceList(), noForeground, c4);
            const Json in = r["inputs"];
            check(in[size_t(0)]["signal"]["port"].number() == 3 && in[size_t(0)]["signal"]["resolution"].string() == "1920x1080",
                  "HDMI 1 is the streamer on receiver 3");
            check(!in[size_t(1)]["signal"].valid() && !in[size_t(2)]["signal"].valid(),
                  "HDMI 2 and 3 have nothing and take no other input's signal");
            check(in[size_t(3)]["signal"]["port"].number() == 0 && in[size_t(3)]["signal"]["refreshHz"].number() == 120 &&
                      in[size_t(3)]["signal"]["resolution"].string() == "3840x2160",
                  "HDMI 4 is the PC on receiver 0 at 4K 120 Hz");
            check(in[size_t(0)]["signal"]["pixelClockMhz"].number() == 148.5 &&
                      in[size_t(3)]["signal"]["pixelClockMhz"].number() == 1188,
                  "the pixel clock is the Pixel Clk field on TMDS and from the totals on FRL");

            // Without a map the base table stands: HDMI 2 takes nothing from receiver 0.
            const Json base = hdmiBaseInputMap();
            check(base["1"].number() == 3 && base["2"].number() == 2 && base["3"].number() == 1 && base["4"].number() == 0,
                  "the base table is HDMI 1 to 4 on receivers 3, 2, 1 and 0");
            check(!hdmiInputs(system, deviceList(), noForeground, base)["inputs"][size_t(1)]["signal"].valid(),
                  "without a map, HDMI 2 takes nothing from receiver 0");
            check(hdmiInputMapFromConfig(Json::parse("{\"returnValue\":true,\"configs\":{},"
                                                     "\"missingConfigs\":[\"inputMap.videoInputMapIndexInfo0\"]}"))["2"].number() == 2,
                  "an answer without a map is the base table");

            // An input the map marks none has no receiver.
            const Json none = hdmiInputMapFromConfig(configdMap(
                "{\"hdmi1\":\"3\",\"hdmi2\":\"2\",\"hdmi3\":\"1\",\"hdmi4\":\"none\"}"));
            check(!none["4"].isNumber() && none["1"].number() == 3,
                  "an input the map marks none has no receiver");
            check(!hdmiInputs(system, deviceList(), noForeground, none)["inputs"][size_t(3)]["signal"].valid(),
                  "and no signal");
            runShell(("rm -rf '" + root + "'"));
        }
        {
            // A C9's HDMI 2.0 driver, no map: the Apple TV on HDMI 4 is receiver 0, the soundbar on HDMI 2 receiver 2.
            const std::string root = rootWith("c9");
            const Json system = collectSystem(root, root);
            const Json in = hdmiInputs(system, deviceList(), noForeground, hdmiBaseInputMap())["inputs"];
            check(!in[size_t(0)]["signal"].valid() && !in[size_t(2)]["signal"].valid(),
                  "a C9's HDMI 1 and 3 have no signal");
            check(in[size_t(1)]["signal"]["port"].number() == 2 && in[size_t(1)]["signal"]["resolution"].string() == "1920x1080",
                  "a C9's HDMI 2 is read from receiver 2");
            check(in[size_t(3)]["signal"]["port"].number() == 0 && in[size_t(3)]["signal"]["refreshHz"].number() == 50 &&
                      in[size_t(3)]["signal"]["resolution"].string() == "3840x2160",
                  "a C9's HDMI 4 is read from receiver 0 at 4K 50 Hz");
            runShell(("rm -rf '" + root + "'"));
        }

        std::printf("%u native HDMI input checks passed\n", checks);
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
