#include "service/oled_care.hpp"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>
#include <ftw.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace nativetv;
using namespace nativetv::service;

namespace {
void check(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}

std::string makeRoot() {
    char pattern[] = "/tmp/nativetv-oled-care-XXXXXX";
    char *path = ::mkdtemp(pattern);
    if (!path)
        throw std::runtime_error("could not create OLED care test directory");
    return path;
}

void makeDirectory(const std::string &path) {
    size_t offset = 1;
    while (offset <= path.size()) {
        const size_t end = path.find('/', offset);
        const std::string current = path.substr(0, end == std::string::npos
                                                       ? path.size() : end);
        if (!current.empty() && ::mkdir(current.c_str(), 0700) != 0 && errno != EEXIST)
            throw std::runtime_error("could not create " + current);
        if (end == std::string::npos)
            break;
        offset = end + 1;
    }
}

void write(const std::string &root, const std::string &path,
           const std::string &contents) {
    const std::string fullPath = root + path;
    const size_t slash = fullPath.find_last_of('/');
    makeDirectory(fullPath.substr(0, slash));
    std::ofstream file(fullPath.c_str());
    if (!file)
        throw std::runtime_error("could not write " + fullPath);
    file << contents;
}

int removePath(const char *path, const struct stat *, int, struct FTW *) {
    return ::remove(path);
}

void removeTree(const std::string &root) {
    ::nftw(root.c_str(), removePath, 16, FTW_DEPTH | FTW_PHYS);
}

void testPanelDetection() {
    check(oledModel("OLED65C2PUA"), "OLED models are recognized");
    check(oledModel("42LX3Q6LA"), "Flex models are recognized");
    check(oledModel("65ART90E6QA"), "ART9 models are recognized");
    check(!oledModel("65ART10"), "ART10 StanbyME remains classified as LCD");
    check(!oledModel("50UP81006LR"), "LCD model is not inferred from usage data");

    const std::string root = makeRoot();
    try {
        check(!hasOledPanelTypeMarker(root), "panel marker is absent initially");
        write(root, "/var/luna/preferences/paneltype_oled", "1\n");
        check(hasOledPanelTypeMarker(root), "panel marker is detected under test root");
        check(!hasOledPanelRecords(root), "panel records are absent initially");
        write(root, "/mnt/lg/cmn_data/pnwash/autoOffRsLastTime", "1\n");
        check(hasOledPanelRecords(root), "legacy panel record proves OLED presence");
    } catch (...) {
        removeTree(root);
        throw;
    }
    removeTree(root);
}

void testOledCareProjection() {
    const std::string root = makeRoot();
    try {
        write(root, "/mnt/lg/cmn_data/pnwash/autoPnwashTime", "800\n");
        write(root, "/mnt/lg/cmn_data/pnwash/autoOffRsIntervalHomeMode", "24\n");
        write(root, "/mnt/lg/cmn_data/pnwash/autoJbInterval", "2000\n");
        write(root, "/mnt/lg/cmn_data/pnwash/completedOffRsCount", "23\n");
        write(root, "/mnt/lg/cmn_data/pnwash/completedJbCount", "9\n");
        write(root, "/mnt/lg/cmn_data/pnwash/failAlertCount", "2\n");
        write(root, "/mnt/lg/cmn_data/pnwash/socTpcStatus", "0\n");
        write(root, "/mnt/lg/cmn_data/pnwash/state", "2\n");

        OledCareSources sources;
        sources.systemRoot = root;
        sources.isOled = true;
        sources.serviceControls = true;
        sources.writable = true;
        sources.systemProperties = Json::parse(
            R"({"panelUsageTime":"6000","lastCompensationTimestamp":"5994"})");
        sources.displayStatus = Json::parse(R"({"status":"processing"})");
        sources.liveProtections = Json::parse(
            R"({"gsr":true,"tpc":false,"gsrStressCount":17})");
        sources.pictureSettings = Json::parse(
            R"({"screenShift":"on","logoLuminanceAdjust":"strong"})");

        const Json response = oledCareResponse(sources);
        check(response["ok"].boolean() && response["isOled"].boolean() &&
                  response["available"].boolean() && response["serviceControls"].boolean() &&
                  response["writable"].boolean(),
              "OLED availability and writable fields are preserved");
        check(response["gsr"].boolean() && !response["tpc"].boolean() &&
                  response["gsrStressCount"].number() == 17,
              "live protection state takes precedence over marker state");
        check(response["screenShift"].string() == "on" &&
                  response["logoDimming"].string() == "strong",
              "picture settings are projected into OLED care values");
        check(response["panelHours"].number() == 1000 &&
                  response["hoursUntilComp"].number() == 3 &&
                  response["compInterval"].number() == 4,
              "panel and compensation counters retain hour conversion");
        check(response["hoursUntilRefresher"].number() == 1800 &&
                  response["refresherInterval"].number() == 2000,
              "refresher interval and remaining hours preserve the legacy values");
        check(response["compStatus"].string() == "Running" &&
                  response["refresherStatus"].string() == "Running" &&
                  response["compCycles"].number() == 23 &&
                  response["refresherCycles"].number() == 9 &&
                  response["failureAlerts"].number() == 2,
              "maintenance states and cycle counts are projected");

        OledCareSources fallback = sources;
        fallback.systemProperties = Json::parse(R"({"panelUsageTime":0})");
        fallback.panelController = Json::parse(R"({"panelUsageTime":"600"})");
        const Json fallbackResponse = oledCareResponse(fallback);
        check(fallbackResponse["panelHours"].number() == 100,
              "numeric zero falls back to the panel-controller usage reading");
    } catch (...) {
        removeTree(root);
        throw;
    }
    removeTree(root);
}

void testUnavailablePanelResponse() {
    OledCareSources sources;
    sources.writable = true;
    const Json response = oledCareResponse(sources);
    check(response["ok"].boolean() && !response["isOled"].boolean() &&
              !response["available"].boolean() && response["writable"].boolean(),
          "non-OLED response keeps the legacy availability shape");
    check(!response["gsr"].valid() && !response["tpc"].valid() &&
              response["compInterval"].number() == 4 &&
              response["refresherInterval"].number() == 2000,
          "non-OLED readings are null with the legacy interval defaults");
}
// test/test-oled-protections.js, through the object /api/stats carries under `oled`.
void testOledStatsProtections() {
    const std::string pnwash = "/mnt/lg/cmn_data/pnwash/";
    {
        // The live service wins over a marker that says otherwise.
        const std::string root = makeRoot();
        write(root, pnwash + "autoOffRsInterval", "4");
        write(root, pnwash + "gsrOff", "");
        OledCareSources sources;
        sources.systemRoot = root;
        sources.liveProtections = Json::parse(R"({"gsr":true,"tpc":false,"gsrStressCount":7})");
        const Json stats = oledStatsObject(sources);
        check(stats["gsr_enabled"].boolean() && !stats["tpc_enabled"].boolean() &&
                  stats["gsr_stress_count"].number() == 7,
              "the live service is read into the stats");
        check(stats["gsr_protection"].string() == "Disabled",
              "the marker is still reported as read");
        removeTree(root);
    }
    {
        // Without the service, the marker files decide.
        const std::string root = makeRoot();
        write(root, pnwash + "autoOffRsInterval", "4");
        write(root, pnwash + "tpcOff", "");
        OledCareSources sources;
        sources.systemRoot = root;
        const Json stats = oledStatsObject(sources);
        check(stats["gsr_enabled"].boolean() && !stats["tpc_enabled"].boolean(),
              "without the service the markers decide");
        check(!stats["gsr_stress_count"].valid(), "a TV without the service has no stress count");
        check(stats["asbl_protection"].string() == "Disabled", "tpcOff turns ASBL off");
        removeTree(root);
    }
    {
        // socTpcStatus is the dimming state, not the setting: 0 between checks.
        const std::string root = makeRoot();
        write(root, pnwash + "socTpcStatus", "0");
        OledCareSources sources;
        sources.systemRoot = root;
        Json stats = oledStatsObject(sources);
        check(stats["asbl_protection"].string() == "Active" && stats["tpc_enabled"].boolean(),
              "socTpcStatus 0 is not the setting being off");
        write(root, pnwash + "tpcOff", "");
        stats = oledStatsObject(sources);
        check(stats["asbl_protection"].string() == "Disabled" && !stats["tpc_enabled"].boolean(),
              "tpcOff alone turns ASBL off");
        removeTree(root);
    }
    {
        // A B8 writes no marker, which is not the protections being off.
        const std::string root = makeRoot();
        OledCareSources sources;
        sources.systemRoot = root;
        const Json stats = oledStatsObject(sources);
        check(!stats["gsr_enabled"].valid() && !stats["tpc_enabled"].valid() &&
                  !stats["gsr_stress_count"].valid() && !stats["asbl_protection"].valid(),
              "with neither the service nor a marker the protections are not reported");
        check(stats["screen_shift"].string() == "off" && stats["logo_dimming"].string() == "off",
              "an unread picture setting is off");
        removeTree(root);
    }
    {
        // The wear figures: the TV's 10-minute units, and the hours they round to.
        const std::string root = makeRoot();
        write(root, pnwash + "autoOffRsIntervalHomeMode", "24");
        write(root, pnwash + "autoPnwashTime", "800");
        OledCareSources sources;
        sources.systemRoot = root;
        sources.systemProperties = Json::parse(R"({"panelUsageTime":"6000","lastCompensationTimestamp":"5994"})");
        sources.pictureSettings = Json::parse(R"({"screenShift":"on","logoLuminanceAdjust":"strong"})");
        const Json stats = oledStatsObject(sources);
        check(stats["panel_usage_units"].number() == 6000 && stats["panel_hours"].number() == 1000 &&
                  stats["last_compensation_units"].number() == 5994 &&
                  stats["last_compensation_hours"].number() == 999 &&
                  stats["hours_since_comp"].number() == 1 && stats["comp_interval_hours"].number() == 4 &&
                  stats["comp_interval_units"].number() == 24 &&
                  stats["hours_until_comp"].number() == 3,
              "the compensation figures follow the 10-minute units");
        check(stats["last_refresher_hours"].number() == 800 &&
                  stats["hours_since_refresher"].number() == 200 &&
                  stats["hours_until_refresher"].number() == 1800 &&
                  stats["screen_shift"].string() == "on" && stats["logo_dimming"].string() == "strong",
              "the refresher figures and the picture settings");
        removeTree(root);
    }
}
} // namespace

int main() {
    try {
        testOledStatsProtections();
        testPanelDetection();
        testOledCareProjection();
        testUnavailablePanelResponse();
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "OLED care service test failed: %s\n", error.what());
        return 1;
    }
}
