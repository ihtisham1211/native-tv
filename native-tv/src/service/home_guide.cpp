#include "service/home_guide.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace nativetv {
namespace service {
namespace {
struct CodePoint {
    unsigned long value;
    size_t width;
};

CodePoint decode(const std::string &text, size_t offset) {
    const unsigned char first = static_cast<unsigned char>(text[offset]);
    if (first < 0x80) return {first, 1};
    if ((first & 0xe0) == 0xc0 && offset + 1 < text.size()) {
        const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
        if ((second & 0xc0) == 0x80)
            return {static_cast<unsigned long>(((first & 0x1f) << 6) | (second & 0x3f)), 2};
    }
    if ((first & 0xf0) == 0xe0 && offset + 2 < text.size()) {
        const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
        const unsigned char third = static_cast<unsigned char>(text[offset + 2]);
        if ((second & 0xc0) == 0x80 && (third & 0xc0) == 0x80)
            return {static_cast<unsigned long>(((first & 0x0f) << 12) |
                                               ((second & 0x3f) << 6) |
                                               (third & 0x3f)), 3};
    }
    if ((first & 0xf8) == 0xf0 && offset + 3 < text.size()) {
        const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
        const unsigned char third = static_cast<unsigned char>(text[offset + 2]);
        const unsigned char fourth = static_cast<unsigned char>(text[offset + 3]);
        if ((second & 0xc0) == 0x80 && (third & 0xc0) == 0x80 &&
            (fourth & 0xc0) == 0x80)
            return {static_cast<unsigned long>(((first & 0x07) << 18) |
                                               ((second & 0x3f) << 12) |
                                               ((third & 0x3f) << 6) |
                                               (fourth & 0x3f)), 4};
    }
    return {first, 1};
}

bool javascriptWhitespace(unsigned long value) {
    return (value >= 0x0009 && value <= 0x000d) || value == 0x0020 ||
           value == 0x00a0 || value == 0x1680 ||
           (value >= 0x2000 && value <= 0x200a) || value == 0x2028 ||
           value == 0x2029 || value == 0x202f || value == 0x205f ||
           value == 0x3000 || value == 0xfeff;
}

std::string trim(const std::string &text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end) {
        const CodePoint point = decode(text, begin);
        if (!javascriptWhitespace(point.value)) break;
        begin += point.width;
    }
    size_t lastNonWhitespace = begin;
    for (size_t offset = begin; offset < end;) {
        const CodePoint point = decode(text, offset);
        offset += point.width;
        if (!javascriptWhitespace(point.value)) lastNonWhitespace = offset;
    }
    return text.substr(begin, lastNonWhitespace - begin);
}

bool truthy(const Json &value) {
    if (!value.valid()) return false;
    if (value.isBoolean()) return value.boolean();
    if (value.isNumber()) return value.number() != 0;
    if (value.isString()) return !value.string().empty();
    return value.isObject() || value.isArray();
}

std::string jsString(const Json &value) {
    if (!value.valid()) return "";
    if (value.isString()) return value.string();
    if (value.isBoolean()) return value.boolean() ? "true" : "false";
    if (value.isNumber()) {
        std::ostringstream out;
        out << std::setprecision(std::numeric_limits<double>::digits10 + 1)
            << value.number();
        return out.str();
    }
    if (value.isObject()) return "[object Object]";
    std::string result;
    const std::vector<Json> values = value.items();
    for (size_t index = 0; index < values.size(); ++index) {
        if (index) result.push_back(',');
        if (values[index].valid()) result += jsString(values[index]);
    }
    return result;
}

bool numberPart(const std::string &text, double &value) {
    if (text.empty() || std::all_of(text.begin(), text.end(), [](unsigned char character) {
            return std::isspace(character) != 0;
        })) {
        value = 0;
        return true;
    }
    char *end = nullptr;
    value = std::strtod(text.c_str(), &end);
    return end && end != text.c_str() && *end == '\0' && std::isfinite(value);
}

int64_t floorDivide(int64_t value, int64_t divisor) {
    int64_t quotient = value / divisor;
    if (value % divisor < 0) --quotient;
    return quotient;
}

int64_t daysFromCivil(int64_t year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int64_t era = floorDivide(year, 400);
    const unsigned yearOfEra = static_cast<unsigned>(year - era * 400);
    const unsigned adjustedMonth = month > 2 ? month - 3 : month + 9;
    const unsigned dayOfYear = (153 * adjustedMonth + 2) / 5 + day - 1;
    const unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    return era * 146097 + static_cast<int64_t>(dayOfEra) - 719468;
}

bool dateUtc(const Json &value, int64_t &result) {
    if (!value.isString()) return false;
    std::vector<std::string> parts;
    const std::string text = value.string();
    size_t begin = 0;
    while (begin <= text.size()) {
        const size_t comma = text.find(',', begin);
        parts.push_back(text.substr(begin, comma == std::string::npos
            ? std::string::npos : comma - begin));
        if (comma == std::string::npos) break;
        begin = comma + 1;
    }
    if (parts.size() < 5) return false;

    double component[6] = {};
    for (size_t index = 0; index < 6 && index < parts.size(); ++index)
        if (!numberPart(parts[index], component[index])) return false;
    for (double valuePart : component)
        if (std::fabs(valuePart) > 1000000) return false;

    int64_t year = static_cast<int64_t>(std::trunc(component[0]));
    if (year >= 0 && year <= 99) year += 1900;
    const int64_t monthIndex = static_cast<int64_t>(std::trunc(component[1])) - 1;
    year += floorDivide(monthIndex, 12);
    const unsigned month = static_cast<unsigned>(monthIndex - floorDivide(monthIndex, 12) * 12 + 1);
    const int64_t day = static_cast<int64_t>(std::trunc(component[2]));
    const int64_t hour = static_cast<int64_t>(std::trunc(component[3]));
    const int64_t minute = static_cast<int64_t>(std::trunc(component[4]));
    const int64_t second = static_cast<int64_t>(std::trunc(component[5]));
    const int64_t days = daysFromCivil(year, month, 1) + day - 1;
    const int64_t seconds = hour * 3600 + minute * 60 + second;
    const int64_t millis = (days * 86400 + seconds) * 1000;
    if (millis < -8640000000000000LL || millis > 8640000000000000LL)
        return false;
    result = millis;
    return true;
}
} // namespace

Json homeCurrentProgramme(const Json &reply, int64_t nowMs) {
    if (!reply.isObject() || (reply["returnValue"].isBoolean() &&
                              !reply["returnValue"].boolean()))
        return Json();
    const Json titleValue = reply["programName"];
    const std::string title = truthy(titleValue) ? trim(jsString(titleValue)) : "";
    int64_t start = 0;
    int64_t end = 0;
    if (title.empty() || !dateUtc(reply["startTime"], start) ||
        !dateUtc(reply["endTime"], end) || start > nowMs || end <= nowMs)
        return Json();
    Json result = Json::object();
    result.set("title", Json::text(title));
    result.set("start", Json::numeric(static_cast<double>(start)));
    result.set("end", Json::numeric(static_cast<double>(end)));
    return result;
}

} // namespace service
} // namespace nativetv
