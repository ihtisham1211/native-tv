#include "service/home_channels.hpp"
#include <cctype>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace nativetv {
namespace service {
namespace {
bool truthy(const Json &value) {
    if (!value.valid()) return false;
    if (value.isBoolean()) return value.boolean();
    if (value.isNumber()) return value.number() != 0;
    if (value.isString()) return !value.string().empty();
    return value.isArray() || value.isObject();
}

std::string jsString(const Json &value) {
    if (!value.valid()) return "undefined";
    if (value.isString()) return value.string();
    if (value.isBoolean()) return value.boolean() ? "true" : "false";
    if (value.isNumber()) {
        std::ostringstream out;
        out << std::setprecision(std::numeric_limits<double>::digits10 + 1)
            << value.number();
        std::string rendered = out.str();
        if (rendered.size() > 2 && rendered.compare(rendered.size() - 2, 2, ".0") == 0)
            rendered.erase(rendered.size() - 2);
        return rendered;
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

std::string channelString(const Json &value) {
    return truthy(value) ? jsString(value) : "";
}
} // namespace

HomeChannels homeChannels(const Json &reply) {
    HomeChannels result;
    result.channels = Json::parse("null");
    if (!reply.isObject() ||
        (reply["returnValue"].isBoolean() && !reply["returnValue"].boolean()) ||
        !reply["channelList"].isArray())
        return result;

    result.channels = Json::array();
    result.tunerAvailable = true;
    for (const Json &channel : reply["channelList"].items()) {
        if (!channel.isObject() || !channel["channelId"].isString() ||
            (channel["skipped"].isBoolean() && channel["skipped"].boolean()) ||
            (channel["Invisible"].isBoolean() && channel["Invisible"].boolean()))
            continue;
        Json item = Json::object();
        item.set("id", channel["channelId"]);
        item.set("number", Json::text(channelString(channel["channelNumber"])));
        item.set("name", Json::text(trim(channelString(channel["channelName"]))));
        item.set("radio", Json::flag(channel["Radio"].isBoolean() &&
                                      channel["Radio"].boolean()));
        result.channels.add(item);
    }
    return result;
}

} // namespace service
} // namespace nativetv
