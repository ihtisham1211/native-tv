#include "service/saved_pages.hpp"
#include <algorithm>
#include <cctype>
#include <cstdint>

namespace nativetv {
namespace service {
namespace {
struct CodePoint {
    uint32_t value;
    size_t width;
};

CodePoint decode(const std::string &text, size_t offset) {
    const unsigned char first = static_cast<unsigned char>(text[offset]);
    if (first < 0x80) return {first, 1};
    if ((first & 0xe0) == 0xc0 && offset + 1 < text.size()) {
        const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
        if ((second & 0xc0) == 0x80)
            return {static_cast<uint32_t>(((first & 0x1f) << 6) | (second & 0x3f)), 2};
    }
    if ((first & 0xf0) == 0xe0 && offset + 2 < text.size()) {
        const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
        const unsigned char third = static_cast<unsigned char>(text[offset + 2]);
        if ((second & 0xc0) == 0x80 && (third & 0xc0) == 0x80)
            return {static_cast<uint32_t>(((first & 0x0f) << 12) |
                                          ((second & 0x3f) << 6) | (third & 0x3f)), 3};
    }
    if ((first & 0xf8) == 0xf0 && offset + 3 < text.size()) {
        const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
        const unsigned char third = static_cast<unsigned char>(text[offset + 2]);
        const unsigned char fourth = static_cast<unsigned char>(text[offset + 3]);
        if ((second & 0xc0) == 0x80 && (third & 0xc0) == 0x80 &&
            (fourth & 0xc0) == 0x80)
            return {static_cast<uint32_t>(((first & 0x07) << 18) |
                                          ((second & 0x3f) << 12) |
                                          ((third & 0x3f) << 6) | (fourth & 0x3f)), 4};
    }
    return {first, 1};
}

bool javascriptWhitespace(uint32_t value) {
    return (value >= 0x0009 && value <= 0x000d) || value == 0x0020 ||
           value == 0x00a0 || value == 0x1680 ||
           (value >= 0x2000 && value <= 0x200a) || value == 0x2028 ||
           value == 0x2029 || value == 0x202f || value == 0x205f ||
           value == 0x3000 || value == 0xfeff;
}

std::string trimJavascriptWhitespace(const std::string &text) {
    size_t start = 0;
    size_t end = text.size();
    while (start < end && javascriptWhitespace(decode(text, start).value))
        start += decode(text, start).width;
    size_t lastNonWhitespace = start;
    for (size_t offset = start; offset < end;) {
        const CodePoint point = decode(text, offset);
        offset += point.width;
        if (!javascriptWhitespace(point.value))
            lastNonWhitespace = offset;
    }
    return text.substr(start, lastNonWhitespace - start);
}

bool validIpv4(const std::string &host) {
    size_t offset = 0;
    unsigned parts = 0;
    while (offset <= host.size()) {
        const size_t dot = host.find('.', offset);
        const size_t end = dot == std::string::npos ? host.size() : dot;
        if (end == offset || end - offset > 3 || ++parts > 4)
            return false;
        unsigned value = 0;
        for (size_t i = offset; i < end; ++i) {
            if (!std::isdigit(static_cast<unsigned char>(host[i]))) return false;
            value = value * 10 + static_cast<unsigned>(host[i] - '0');
        }
        if (value > 255) return false;
        if (dot == std::string::npos) break;
        offset = dot + 1;
    }
    return parts == 4;
}

bool validDomain(const std::string &host) {
    const size_t lastDot = host.find_last_of('.');
    if (lastDot == std::string::npos || host.size() - lastDot - 1 < 2 ||
        host.size() - lastDot - 1 > 63)
        return false;
    for (size_t i = lastDot + 1; i < host.size(); ++i)
        if (!std::isalpha(static_cast<unsigned char>(host[i]))) return false;

    size_t labelStart = 0;
    while (labelStart < lastDot) {
        const size_t dot = host.find('.', labelStart);
        const size_t labelEnd = dot == std::string::npos || dot > lastDot ? lastDot : dot;
        const size_t length = labelEnd - labelStart;
        if (length == 0 || length > 63 ||
            !std::isalnum(static_cast<unsigned char>(host[labelStart])) ||
            !std::isalnum(static_cast<unsigned char>(host[labelEnd - 1])))
            return false;
        for (size_t i = labelStart; i < labelEnd; ++i) {
            const unsigned char character = static_cast<unsigned char>(host[i]);
            if (!std::isalnum(character) && character != '-') return false;
        }
        if (labelEnd == lastDot) break;
        labelStart = labelEnd + 1;
    }
    return true;
}

bool containsJavascriptWhitespace(const std::string &text) {
    for (size_t offset = 0; offset < text.size();) {
        const CodePoint point = decode(text, offset);
        if (javascriptWhitespace(point.value)) return true;
        offset += point.width;
    }
    return false;
}

bool validHost(const std::string &value) {
    std::string host = value;
    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    if (host == "localhost") return true;
    if (host.find_first_not_of("0123456789.") == std::string::npos)
        return validIpv4(host);
    return validDomain(host);
}

bool schemePrefix(const std::string &value) {
    const size_t colon = value.find(':');
    if (colon == std::string::npos || colon == 0 ||
        !std::isalpha(static_cast<unsigned char>(value[0])))
        return false;
    for (size_t i = 1; i < colon; ++i) {
        const unsigned char character = static_cast<unsigned char>(value[i]);
        if (!std::isalnum(character) && character != '+' && character != '.' &&
            character != '-')
            return false;
    }
    return true;
}
} // namespace

bool savedPageTarget(const std::string &address, SavedPageTarget &target) {
    target = SavedPageTarget{};
    std::string url = trimJavascriptWhitespace(address);
    if (!schemePrefix(url))
        url = "https://" + url;

    std::string lowerUrl = url;
    std::transform(lowerUrl.begin(), lowerUrl.end(), lowerUrl.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    size_t authorityStart = 0;
    if (lowerUrl.compare(0, 7, "http://") == 0)
        authorityStart = 7;
    else if (lowerUrl.compare(0, 8, "https://") == 0)
        authorityStart = 8;
    else
        return false;

    const size_t authorityEnd = url.find_first_of("/?#", authorityStart);
    const size_t end = authorityEnd == std::string::npos ? url.size() : authorityEnd;
    const std::string authority = url.substr(authorityStart, end - authorityStart);
    if (authority.empty()) return false;
    if (containsJavascriptWhitespace(url)) return false;

    const size_t colon = authority.find(':');
    const std::string host = authority.substr(0, colon);
    if (host.empty() || !validHost(host)) return false;
    if (colon != std::string::npos) {
        const std::string port = authority.substr(colon + 1);
        if (port.empty() || port.size() > 5 ||
            port.find_first_not_of("0123456789") != std::string::npos)
            return false;
    }

    target.url = url;
    target.host = host;
    if (target.host.compare(0, 4, "www.") == 0)
        target.host.erase(0, 4);
    return true;
}

bool savedPageAddressEmpty(const std::string &address) {
    return trimJavascriptWhitespace(address).empty();
}

std::string normalizeSavedPageTitle(const std::string &title) {
    std::string result;
    bool pendingSpace = false;
    for (size_t offset = 0; offset < title.size();) {
        const CodePoint point = decode(title, offset);
        if (javascriptWhitespace(point.value)) {
            pendingSpace = !result.empty();
        } else {
            if (pendingSpace) result.push_back(' ');
            result.append(title, offset, point.width);
            pendingSpace = false;
        }
        offset += point.width;
    }
    return result;
}

size_t savedPageTitleLength(const std::string &title) {
    size_t units = 0;
    for (size_t offset = 0; offset < title.size();) {
        const CodePoint point = decode(title, offset);
        units += point.value > 0xffff ? 2 : 1;
        offset += point.width;
    }
    return units;
}

bool savedPageBookmark(const std::string &appId, const std::string &launchPointType,
                       const std::string &browserId) {
    return appId == browserId && launchPointType == "bookmark";
}

} // namespace service
} // namespace nativetv
