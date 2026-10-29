#include "service/oled_care.hpp"
#include "domain/names.hpp"
#include "service/kernel_files.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <regex>
#include <sys/stat.h>

namespace nativetv {
namespace service {
namespace {
const char *kPanelTypeMarker = "/var/luna/preferences/paneltype_oled";
const char *kPanelRecords[] = {
    "/mnt/lg/cmn_data/pnwash/autoOffRsLastTime",
    "/mnt/lg/cmn_data/pnwash/autoOffRsTime"};

std::string rootedPath(const std::string &root, const std::string &path) {
    if (root.empty() || root == "/")
        return path;
    size_t end = root.size();
    while (end > 1 && root[end - 1] == '/')
        --end;
    return root.substr(0, end) + path;
}

bool exists(const std::string &path) {
    struct stat status;
    return ::stat(path.c_str(), &status) == 0;
}

std::string trim(const std::string &value) {
    size_t first = 0;
    size_t last = value.size();
    while (first < last && std::isspace(static_cast<unsigned char>(value[first])))
        ++first;
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1])))
        --last;
    return value.substr(first, last - first);
}

std::string readTrimmed(const std::string &root, const std::string &path) {
    std::string value;
    if (!readKernelFile(rootedPath(root, path), value))
        return std::string();
    return trim(value);
}

bool parseIntPrefix(const Json &value, double &number) {
    std::string text;
    if (value.isString()) {
        text = value.string();
    } else if (value.isNumber()) {
        if (value.number() == 0)
            return false;
        text = value.dump();
    } else {
        return false;
    }
    const char *start = text.c_str();
    while (*start && std::isspace(static_cast<unsigned char>(*start)))
        ++start;
    char *end = nullptr;
    const double parsed = std::strtod(start, &end);
    if (end == start || !std::isfinite(parsed))
        return false;
    number = std::trunc(parsed);
    return true;
}

bool parseFloatPrefix(const std::string &text, double &number) {
    const char *start = text.c_str();
    while (*start && std::isspace(static_cast<unsigned char>(*start)))
        ++start;
    char *end = nullptr;
    const double parsed = std::strtod(start, &end);
    if (end == start || !std::isfinite(parsed))
        return false;
    number = parsed;
    return true;
}

double roundOneDecimal(double value) {
    return std::floor(value * 10.0 + 0.5) / 10.0;
}

Json protectionValue(const Json &live, const char *key, bool fallbackAvailable,
                     bool fallbackValue) {
    if (live.isObject()) {
        const Json value = live[key];
        return value.isBoolean() ? value : Json();
    }
    return fallbackAvailable ? Json::flag(fallbackValue) : Json();
}

void setUnavailableFields(Json &response) {
    response.set("gsr", Json());
    response.set("tpc", Json());
    response.set("gsrStressCount", Json());
    response.set("screenShift", Json());
    response.set("logoDimming", Json());
    response.set("panelHours", Json());
    response.set("hoursUntilComp", Json());
    response.set("compInterval", Json::numeric(4));
    response.set("hoursUntilRefresher", Json());
    response.set("refresherInterval", Json::numeric(2000));
    response.set("compStatus", Json());
    response.set("refresherStatus", Json());
    response.set("compCycles", Json());
    response.set("refresherCycles", Json());
    response.set("failureAlerts", Json());
}
} // namespace

bool oledModel(const std::string &model) {
    static const std::regex oled("oled", std::regex::icase);
    static const std::regex flex("^[0-9]{2}LX[0-9]", std::regex::icase);
    static const std::regex art("^[0-9]{2}ART9[0-9]", std::regex::icase);
    return std::regex_search(model, oled) || std::regex_search(model, flex) ||
           std::regex_search(model, art);
}

bool hasOledPanelTypeMarker(const std::string &systemRoot) {
    return exists(rootedPath(systemRoot, kPanelTypeMarker));
}

bool hasOledPanelRecords(const std::string &systemRoot) {
    for (const char *record : kPanelRecords)
        if (exists(rootedPath(systemRoot, record)))
            return true;
    return false;
}

Json oledCareResponse(const OledCareSources &sources) {
    Json response = Json::object();
    response.set("ok", Json::flag(true));
    response.set("isOled", Json::flag(sources.isOled));
    response.set("available", Json::flag(sources.isOled));
    response.set("serviceControls", Json::flag(sources.serviceControls));
    response.set("writable", Json::flag(sources.writable));
    if (!sources.isOled) {
        setUnavailableFields(response);
        return response;
    }

    const std::string root = sources.systemRoot.empty() ? "/" : sources.systemRoot;
    double usageUnits = 0;
    bool hasUsageUnits = parseIntPrefix(sources.systemProperties["panelUsageTime"],
                                        usageUnits);
    double lastCompUnits = 0;
    bool hasLastCompUnits = parseIntPrefix(
        sources.systemProperties["lastCompensationTimestamp"], lastCompUnits);
    if (!hasUsageUnits) {
        double fallbackUsage = 0;
        if (parseIntPrefix(sources.panelController["panelUsageTime"], fallbackUsage)) {
            usageUnits = fallbackUsage;
            hasUsageUnits = true;
        }
    }

    const std::string refresherLastRaw =
        readTrimmed(root, "/mnt/lg/cmn_data/pnwash/autoPnwashTime").empty()
            ? readTrimmed(root, "/mnt/lg/cmn_data/pnwash/autoJbLastTime")
            : readTrimmed(root, "/mnt/lg/cmn_data/pnwash/autoPnwashTime");
    double lastRefresher = 0;
    if (!refresherLastRaw.empty()) {
        double parsed = 0;
        if (parseFloatPrefix(refresherLastRaw, parsed))
            lastRefresher = std::trunc(parsed);
    }

    const std::string lastCompRaw =
        readTrimmed(root, "/mnt/lg/cmn_data/pnwash/autoOffRsTime").empty()
            ? readTrimmed(root, "/mnt/lg/cmn_data/pnwash/autoOffRsLastTime")
            : readTrimmed(root, "/mnt/lg/cmn_data/pnwash/autoOffRsTime");
    double fsLastCompHours = 0;
    const bool hasFileCompHours = !lastCompRaw.empty() &&
        parseFloatPrefix(lastCompRaw, fsLastCompHours);
    if (hasFileCompHours)
        fsLastCompHours = std::trunc(fsLastCompHours);
    if (!hasUsageUnits && hasFileCompHours) {
        usageUnits = fsLastCompHours * 6.0;
        hasUsageUnits = true;
    }

    const std::string intervalUnitsRaw = readTrimmed(
        root, "/mnt/lg/cmn_data/pnwash/autoOffRsIntervalHomeMode");
    double compInterval = 4;
    if (!intervalUnitsRaw.empty()) {
        double units = 0;
        if (parseFloatPrefix(intervalUnitsRaw, units)) {
            units = std::trunc(units);
            if (units <= 0)
                units = 24;
            compInterval = roundOneDecimal(units * 10.0 / 60.0);
        }
    } else {
        const std::string intervalHoursRaw = readTrimmed(
            root, "/mnt/lg/cmn_data/pnwash/autoOffRsInterval");
        double hours = 4;
        if (!intervalHoursRaw.empty() && !parseFloatPrefix(intervalHoursRaw, hours))
            hours = 4;
        if (hours <= 0)
            hours = 4;
        compInterval = hours;
    }
    if (compInterval < 0.5 || compInterval > 24)
        compInterval = 4;

    const std::string refresherIntervalRaw = readTrimmed(
        root, "/mnt/lg/cmn_data/pnwash/autoJbInterval");
    double refresherInterval = 2000;
    if (!refresherIntervalRaw.empty() &&
        parseFloatPrefix(refresherIntervalRaw, refresherInterval))
        refresherInterval = std::trunc(refresherInterval);
    if (refresherInterval <= 0)
        refresherInterval = 2000;

    const double panelHours = hasUsageUnits ? std::floor(usageUnits / 6.0) : 0;
    const double panelHoursExact = hasUsageUnits
        ? roundOneDecimal(usageUnits / 6.0) : 0;
    double lastCompHours = 0;
    double hoursSinceComp = 0;
    if (hasLastCompUnits) {
        lastCompHours = roundOneDecimal(lastCompUnits / 6.0);
        hoursSinceComp = hasUsageUnits
            ? roundOneDecimal((usageUnits - lastCompUnits) / 6.0) : 0;
    } else if (hasFileCompHours) {
        lastCompHours = fsLastCompHours;
        hoursSinceComp = panelHoursExact && lastCompHours
            ? std::max(0.0, roundOneDecimal(panelHoursExact - lastCompHours)) : 0;
    }
    const double hoursUntilComp = std::max(
        0.0, roundOneDecimal(compInterval - hoursSinceComp));
    const double hoursSinceRefresher = panelHours && lastRefresher
        ? std::max(0.0, panelHours - lastRefresher) : 0;
    const double hoursUntilRefresher = std::max(
        0.0, refresherInterval - hoursSinceRefresher);

    const std::string offRsCountRaw = readTrimmed(
        root, "/mnt/lg/cmn_data/pnwash/completedOffRsCount");
    const std::string jbCountRaw = readTrimmed(
        root, "/mnt/lg/cmn_data/pnwash/completedJbCount");
    const std::string failCountRaw = readTrimmed(
        root, "/mnt/lg/cmn_data/pnwash/failAlertCount");
    const bool tpcOff = exists(rootedPath(root, "/mnt/lg/cmn_data/pnwash/tpcOff"));
    const bool gsrOff = exists(rootedPath(root, "/mnt/lg/cmn_data/pnwash/gsrOff"));
    const std::string socTpc = readTrimmed(
        root, "/mnt/lg/cmn_data/pnwash/socTpcStatus");
    const bool hasTpcMonitoring = tpcOff || !socTpc.empty() || exists(rootedPath(
        root, "/mnt/lg/cmn_data/pnwash/socTpcStatus")) || exists(
        rootedPath(root, "/mnt/lg/cmn_data/pnwash/autoOffRsInterval"));
    const bool hasAsblStatus = hasTpcMonitoring;
    const bool asblActive = !tpcOff;
    const bool hasGsrStatus = hasTpcMonitoring;
    const bool gsrActive = !gsrOff;

    const std::string state = readTrimmed(root, "/mnt/lg/cmn_data/pnwash/state");
    const std::string jobScope = readTrimmed(root, "/mnt/lg/cmn_data/pnwash/jobScope");
    const bool running = (!state.empty() && state[0] == '2') || jobScope == "1";
    const Json comp = names::compensationStatus(running);
    const std::string displayStatus = sources.displayStatus["status"].isString()
        ? sources.displayStatus["status"].string() : "schedule";
    const Json live = sources.liveProtections;

    response.set("gsr", protectionValue(live, "gsr", hasGsrStatus, gsrActive));
    response.set("tpc", protectionValue(live, "tpc", hasAsblStatus, asblActive));
    response.set("gsrStressCount", live.isObject() &&
                    live["gsrStressCount"].isNumber()
                        ? live["gsrStressCount"] : Json());
    const Json screenShift = sources.pictureSettings["screenShift"];
    const Json logoDimming = sources.pictureSettings["logoLuminanceAdjust"];
    response.set("screenShift", screenShift.isString() && !screenShift.string().empty()
        ? screenShift : Json::text("off"));
    response.set("logoDimming", logoDimming.isString() && !logoDimming.string().empty()
        ? logoDimming : Json::text("off"));
    response.set("panelHours", Json::numeric(panelHours));
    response.set("hoursUntilComp", Json::numeric(hoursUntilComp));
    response.set("compInterval", Json::numeric(compInterval));
    response.set("hoursUntilRefresher", Json::numeric(hoursUntilRefresher));
    response.set("refresherInterval", Json::numeric(refresherInterval));
    response.set("compStatus", Json::text(comp["display"].string()));
    response.set("refresherStatus", Json::text(names::refresherStatus(displayStatus)));

    double cycles = 0;
    response.set("compCycles", parseFloatPrefix(offRsCountRaw, cycles)
        ? Json::numeric(std::trunc(cycles)) : Json());
    cycles = 0;
    response.set("refresherCycles", parseFloatPrefix(jbCountRaw, cycles)
        ? Json::numeric(std::trunc(cycles)) : Json());
    cycles = 0;
    response.set("failureAlerts", parseFloatPrefix(failCountRaw, cycles)
        ? Json::numeric(std::trunc(cycles)) : Json());
    return response;
}

Json oledStatsObject(const OledCareSources &sources) {
    const std::string root = sources.systemRoot.empty() ? "/" : sources.systemRoot;
    const auto number = [](double value) { return Json::numeric(value); };

    double usageUnits = 0;
    bool hasUsage = parseIntPrefix(sources.systemProperties["panelUsageTime"], usageUnits);
    double lastCompUnits = 0;
    const bool hasLastComp = parseIntPrefix(sources.systemProperties["lastCompensationTimestamp"], lastCompUnits);
    if (!hasUsage) {
        double fallback = 0;
        if (parseIntPrefix(sources.panelController["panelUsageTime"], fallback)) {
            usageUnits = fallback;
            hasUsage = true;
        }
    }
    const auto readFirst = [&root](const char *a, const char *b) {
        const std::string first = readTrimmed(root, a);
        return first.empty() ? readTrimmed(root, b) : first;
    };
    double lastRefresher = 0;
    {
        const std::string raw = readFirst("/mnt/lg/cmn_data/pnwash/autoPnwashTime",
                                          "/mnt/lg/cmn_data/pnwash/autoJbLastTime");
        double parsed = 0;
        if (!raw.empty() && parseFloatPrefix(raw, parsed)) lastRefresher = std::trunc(parsed);
    }
    double fsLastCompHours = 0;
    bool hasFsComp = false;
    {
        const std::string raw = readFirst("/mnt/lg/cmn_data/pnwash/autoOffRsTime",
                                          "/mnt/lg/cmn_data/pnwash/autoOffRsLastTime");
        double parsed = 0;
        if (!raw.empty() && parseFloatPrefix(raw, parsed)) {
            fsLastCompHours = std::trunc(parsed);
            hasFsComp = true;
        }
    }
    double compInterval = 4;
    double compIntervalUnits = 24;
    {
        const std::string unitsRaw = readTrimmed(root, "/mnt/lg/cmn_data/pnwash/autoOffRsIntervalHomeMode");
        double parsed = 0;
        if (!unitsRaw.empty()) {
            double units = parseFloatPrefix(unitsRaw, parsed) ? std::trunc(parsed) : 0;
            if (units <= 0) units = 24;
            compIntervalUnits = units;
            compInterval = roundOneDecimal(units * 10.0 / 60.0);
        } else {
            const std::string hoursRaw = readTrimmed(root, "/mnt/lg/cmn_data/pnwash/autoOffRsInterval");
            double hours = 4;
            if (!hoursRaw.empty() && parseFloatPrefix(hoursRaw, parsed)) hours = parsed;
            if (!(hours > 0)) hours = 4;
            compInterval = hours;
            compIntervalUnits = std::floor(hours * 6.0 + 0.5);
        }
        if (compInterval < 0.5 || compInterval > 24) compInterval = 4;
    }
    double refresherInterval = 2000;
    {
        double parsed = 0;
        const std::string raw = readTrimmed(root, "/mnt/lg/cmn_data/pnwash/autoJbInterval");
        if (!raw.empty() && parseFloatPrefix(raw, parsed)) refresherInterval = std::trunc(parsed);
        if (!(refresherInterval > 0)) refresherInterval = 2000;
    }

    if (!hasUsage && hasFsComp) {
        usageUnits = fsLastCompHours * 6.0;
        hasUsage = true;
    }
    const double panelHours = hasUsage ? std::floor(usageUnits / 6.0) : 0;
    const double panelHoursExact = hasUsage ? roundOneDecimal(usageUnits / 6.0) : 0;
    double lastCompHours = 0;
    double hoursSinceComp = 0;
    if (hasLastComp) {
        lastCompHours = roundOneDecimal(lastCompUnits / 6.0);
        hoursSinceComp = hasUsage ? roundOneDecimal((usageUnits - lastCompUnits) / 6.0) : 0;
    } else if (hasFsComp) {
        lastCompHours = fsLastCompHours;
        hoursSinceComp = panelHoursExact && lastCompHours
            ? std::max(0.0, roundOneDecimal(panelHoursExact - lastCompHours)) : 0;
    }
    const double hoursUntilComp = std::max(0.0, roundOneDecimal(compInterval - hoursSinceComp));
    const double hoursSinceRefresher = panelHours && lastRefresher ? std::max(0.0, panelHours - lastRefresher) : 0;
    const double hoursUntilRefresher = std::max(0.0, refresherInterval - hoursSinceRefresher);

    const auto count = [&root](const char *path) {
        double parsed = 0;
        const std::string raw = readTrimmed(root, path);
        return !raw.empty() && parseFloatPrefix(raw, parsed) ? Json::numeric(std::trunc(parsed)) : Json();
    };
    const bool tpcOff = exists(rootedPath(root, "/mnt/lg/cmn_data/pnwash/tpcOff"));
    const bool gsrOff = exists(rootedPath(root, "/mnt/lg/cmn_data/pnwash/gsrOff"));
    const bool hasTpcMonitoring = tpcOff || exists(rootedPath(root, "/mnt/lg/cmn_data/pnwash/socTpcStatus")) ||
                                  exists(rootedPath(root, "/mnt/lg/cmn_data/pnwash/autoOffRsInterval"));
    const Json asbl = hasTpcMonitoring ? Json::text(tpcOff ? "Disabled" : "Active") : Json();
    const Json gsr = hasTpcMonitoring ? Json::text(gsrOff ? "Disabled" : "Active") : Json();
    const std::string state = readTrimmed(root, "/mnt/lg/cmn_data/pnwash/state");
    const std::string jobScope = readTrimmed(root, "/mnt/lg/cmn_data/pnwash/jobScope");
    const bool running = (!state.empty() && state[0] == '2') || jobScope == "1";
    const Json comp = names::compensationStatus(running);
    const std::string rawStatus = sources.displayStatus["status"].isString() &&
                                  !sources.displayStatus["status"].string().empty()
        ? sources.displayStatus["status"].string() : "schedule";
    const Json live = sources.liveProtections;

    Json out = Json::object();
    out.set("panel_usage_units", hasUsage ? number(usageUnits) : Json());
    out.set("last_compensation_units", hasLastComp ? number(lastCompUnits)
        : (hasFsComp ? number(fsLastCompHours * 6.0) : Json()));
    out.set("panel_hours", number(panelHours));
    out.set("panel_hours_exact", number(panelHoursExact));
    out.set("last_compensation_hours", number(lastCompHours));
    out.set("hours_since_comp", number(hoursSinceComp));
    out.set("hours_until_comp", number(hoursUntilComp));
    out.set("comp_interval_hours", number(compInterval));
    out.set("comp_interval_units", number(compIntervalUnits));
    out.set("comp_cycles", count("/mnt/lg/cmn_data/pnwash/completedOffRsCount"));
    out.set("comp_status", comp["display"]);
    out.set("comp_status_label", comp["detail"]);
    out.set("refresher_interval_hours", number(refresherInterval));
    out.set("last_refresher_hours", number(lastRefresher));
    out.set("hours_since_refresher", number(hoursSinceRefresher));
    out.set("hours_until_refresher", number(hoursUntilRefresher));
    out.set("refresher_cycles", count("/mnt/lg/cmn_data/pnwash/completedJbCount"));
    out.set("refresher_status", Json::text(names::refresherStatus(rawStatus)));
    out.set("refresher_status_raw", Json::text(rawStatus));
    out.set("failure_alerts", count("/mnt/lg/cmn_data/pnwash/failAlertCount"));
    out.set("asbl_protection", asbl);
    out.set("gsr_protection", gsr);
    // The live service where the TV has one, the marker files otherwise, and
    // null where neither reports. TPC is the service's name for what the
    // markers call ASBL.
    out.set("gsr_enabled", live.isObject() ? live["gsr"]
        : (gsr.valid() ? Json::flag(gsr.string() == "Active") : Json()));
    out.set("tpc_enabled", live.isObject() ? live["tpc"]
        : (asbl.valid() ? Json::flag(asbl.string() == "Active") : Json()));
    out.set("gsr_stress_count", live.isObject() ? live["gsrStressCount"] : Json());
    const Json shift = sources.pictureSettings["screenShift"];
    const Json dimming = sources.pictureSettings["logoLuminanceAdjust"];
    out.set("screen_shift", shift.isString() && !shift.string().empty() ? shift : Json::text("off"));
    out.set("logo_dimming", dimming.isString() && !dimming.string().empty() ? dimming : Json::text("off"));
    return out;
}

} // namespace service
} // namespace nativetv
