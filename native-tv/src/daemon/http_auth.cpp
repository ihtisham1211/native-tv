#include "daemon/http_auth.hpp"
#include <algorithm>

namespace nativetv {
namespace daemon {
namespace {
std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
    });
    return value;
}

int hexValue(char value) {
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    if (value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    return -1;
}

bool decodeQueryComponent(const std::string &input, std::string &output) {
    output.clear();
    for (size_t index = 0; index < input.size(); ++index) {
        if (input[index] == '+') {
            output.push_back(' ');
        } else if (input[index] == '%') {
            if (index + 2 >= input.size())
                return false;
            const int high = hexValue(input[index + 1]);
            const int low = hexValue(input[index + 2]);
            if (high < 0 || low < 0)
                return false;
            output.push_back(static_cast<char>((high << 4) | low));
            index += 2;
        } else {
            output.push_back(input[index]);
        }
    }
    return true;
}

bool queryToken(const std::string &query, std::string &token) {
    bool found = false;
    size_t start = 0;
    while (start <= query.size()) {
        const size_t end = query.find('&', start);
        const size_t pairEnd = end == std::string::npos ? query.size() : end;
        const size_t equals = query.find('=', start);
        const size_t keyEnd = equals == std::string::npos || equals > pairEnd ? pairEnd : equals;
        std::string key;
        if (!decodeQueryComponent(query.substr(start, keyEnd - start), key))
            return false;
        if (key == "k") {
            if (found)
                return false;
            found = true;
            const size_t valueStart = keyEnd < pairEnd ? keyEnd + 1 : pairEnd;
            if (!decodeQueryComponent(query.substr(valueStart, pairEnd - valueStart), token))
                return false;
        }
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return found;
}

std::string trim(const std::string &value) {
    size_t first = 0;
    size_t last = value.size();
    while (first < last && (value[first] == ' ' || value[first] == '\t'))
        ++first;
    while (last > first && (value[last - 1] == ' ' || value[last - 1] == '\t'))
        --last;
    return value.substr(first, last - first);
}

bool bearerToken(const HttpRequest &request, std::string &token) {
    const std::string authorization = trim(request.header("authorization"));
    const size_t separator = authorization.find_first_of(" \t");
    if (separator == std::string::npos || lower(authorization.substr(0, separator)) != "bearer")
        return false;
    size_t tokenStart = separator;
    while (tokenStart < authorization.size() &&
           (authorization[tokenStart] == ' ' || authorization[tokenStart] == '\t'))
        ++tokenStart;
    if (tokenStart == authorization.size())
        return false;
    token = authorization.substr(tokenStart);
    return token.find_first_of(" \t\r\n") == std::string::npos;
}
} // namespace

bool isLoopbackPeer(const std::string &address) {
    return address == "127.0.0.1" || address == "::1" ||
           address == "::ffff:127.0.0.1" || address == "0:0:0:0:0:0:0:1";
}

bool tokenMatchesConstantTime(const std::string &given, const std::string &expected) {
    unsigned int difference = static_cast<unsigned int>(given.size() ^ expected.size());
    for (size_t index = 0; index < expected.size(); ++index) {
        const unsigned char candidate = index < given.size()
                                            ? static_cast<unsigned char>(given[index])
                                            : 0;
        difference |= candidate ^ static_cast<unsigned char>(expected[index]);
    }
    return difference == 0;
}

bool requestHasValidToken(const HttpRequest &request, const std::string &expected) {
    std::string presented;
    if (bearerToken(request, presented) && tokenMatchesConstantTime(presented, expected))
        return true;
    if (queryToken(request.query, presented) && tokenMatchesConstantTime(presented, expected))
        return true;
    return false;
}

bool requestIsAuthorized(const HttpRequest &request, const std::string &token) {
    return token.empty() || isLoopbackPeer(request.remoteAddress) ||
           requestHasValidToken(request, token);
}
} // namespace daemon
} // namespace nativetv
