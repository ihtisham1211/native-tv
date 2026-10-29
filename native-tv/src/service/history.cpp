#include "service/history.hpp"
#include <fstream>
#include <string>
#include <time.h>

namespace nativetv {
namespace service {
namespace {
const size_t kMaxMonthBytes = 2 * 1024 * 1024;
const size_t kMaxTotalBytes = 16 * 1024 * 1024;

std::string hourKey(int64_t milliseconds) {
    time_t seconds = static_cast<time_t>(milliseconds / 1000);
    struct tm utc;
    if (!gmtime_r(&seconds, &utc))
        return std::string();
    char value[14] = {};
    if (::strftime(value, sizeof(value), "%Y-%m-%dT%H", &utc) == 0)
        return std::string();
    return value;
}

std::string monthKey(const std::string &hour) {
    return hour.size() >= 7 ? hour.substr(0, 7) : std::string();
}

std::string nextMonth(const std::string &month) {
    if (month.size() != 7)
        return std::string();
    if (month[4] != '-')
        return std::string();
    for (size_t index = 0; index < month.size(); ++index) {
        if (index != 4 && (month[index] < '0' || month[index] > '9'))
            return std::string();
    }
    int year = (month[0] - '0') * 1000 + (month[1] - '0') * 100 +
               (month[2] - '0') * 10 + (month[3] - '0');
    int number = (month[5] - '0') * 10 + (month[6] - '0');
    if (year < 1970 || number < 1 || number > 12)
        return std::string();
    if (++number == 13) {
        number = 1;
        ++year;
    }
    std::string yearText = std::to_string(year);
    while (yearText.size() < 4)
        yearText.insert(yearText.begin(), '0');
    std::string monthText = std::to_string(number);
    if (monthText.size() < 2)
        monthText.insert(monthText.begin(), '0');
    return yearText + "-" + monthText;
}

bool mergeHours(const Json &source, const std::string &from, const std::string &to,
                Json &hours) {
    if (!source.isObject())
        return false;
    for (const std::string &hour : source.keys()) {
        if (hour.size() != 13 || hour < from || hour > to)
            continue;
        const Json values = source[hour];
        if (!values.isObject())
            continue;
        Json combined = hours[hour];
        if (!combined.isObject())
            combined = Json::object();
        for (const std::string &key : values.keys()) {
            const Json seconds = values[key];
            if (seconds.isNumber() && seconds.number() >= 0)
                combined.set(key, seconds);
        }
        hours.set(hour, combined);
    }
    return true;
}
} // namespace

bool queryHistory(const std::string &directory, int64_t fromMs, int64_t toMs,
                  const Json &overlay, Json &result, std::string &error) {
    result = Json();
    error.clear();
    if (directory.empty() || fromMs > toMs || !overlay.isObject() ||
        !overlay["hours"].isObject() || !overlay["titles"].isObject() ||
        !overlay["enabled"].isBoolean() || !overlay["clockSet"].isBoolean() ||
        !overlay["readPersisted"].isBoolean() ||
        !(overlay["current"].isString() || !overlay["current"].valid())) {
        error = "invalid history query";
        return false;
    }

    const std::string from = hourKey(fromMs);
    const std::string to = hourKey(toMs);
    if (from.empty() || to.empty() || from > to) {
        error = "invalid history time range";
        return false;
    }

    Json hours = Json::object();
    Json storedTitles = Json::object();
    size_t totalBytes = 0;
    std::string month = overlay["readPersisted"].boolean() ? monthKey(from) : std::string();
    const std::string lastMonth = monthKey(to);
    while (!month.empty() && month <= lastMonth) {
        const std::string path = directory + "/" + month + ".json";
        std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
        if (input) {
            input.seekg(0, std::ios::end);
            const std::streamoff length = input.tellg();
            if (length < 0 || static_cast<size_t>(length) > kMaxMonthBytes ||
                totalBytes + static_cast<size_t>(length) > kMaxTotalBytes) {
                error = "history data exceeds the read limit";
                return false;
            }
            input.seekg(0, std::ios::beg);
            std::string bytes(static_cast<size_t>(length), '\0');
            if (length > 0) {
                input.read(&bytes[0], length);
                if (!input) {
                    error = "could not read history data";
                    return false;
                }
            }
            totalBytes += static_cast<size_t>(length);
            const Json file = Json::parse(bytes);
            if (file.isObject()) {
                if (file["hours"].isObject() &&
                    !mergeHours(file["hours"], from, to, hours)) {
                    error = "invalid history hours";
                    return false;
                }
                const Json titles = file["titles"];
                if (titles.isObject()) {
                    for (const std::string &key : titles.keys()) {
                        if (titles[key].isString())
                            storedTitles.set(key, titles[key]);
                    }
                }
            }
        }
        if (month == lastMonth)
            break;
        month = nextMonth(month);
    }

    if (!mergeHours(overlay["hours"], from, to, hours)) {
        error = "invalid live history overlay";
        return false;
    }

    Json used = Json::object();
    for (const std::string &hour : hours.keys()) {
        const Json values = hours[hour];
        for (const std::string &key : values.keys())
            used.set(key, Json::flag(true));
    }
    Json titles = Json::object();
    const Json freshTitles = overlay["titles"];
    for (const std::string &key : used.keys()) {
        const Json title = freshTitles[key].isString() ? freshTitles[key] : storedTitles[key];
        titles.set(key, title.isString() ? title : Json());
    }

    result = Json::object();
    result.set("enabled", overlay["enabled"]);
    result.set("hours", hours);
    result.set("titles", titles);
    result.set("current", overlay["current"]);
    result.set("clockSet", overlay["clockSet"]);
    result.set("ok", Json::flag(true));
    return true;
}

} // namespace service
} // namespace nativetv
