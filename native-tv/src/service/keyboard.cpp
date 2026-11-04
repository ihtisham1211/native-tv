#include "service/keyboard.hpp"
#include <cmath>

namespace nativetv {
namespace service {
namespace {
const size_t kMaximumCharacters = 256;
const int kMaximumDelete = 100;

// The characters of a UTF-8 text, or -1 when it is not valid UTF-8 or holds a
// control character (a newline would press Enter in some fields, which is what
// the enter action is for).
long characterCount(const std::string &text) {
    long count = 0;
    for (size_t index = 0; index < text.size();) {
        const unsigned char first = static_cast<unsigned char>(text[index]);
        size_t length = 1;
        if (first < 0x20 || first == 0x7f)
            return -1;
        if (first >= 0xf8 || (first >= 0x80 && first < 0xc2)) return -1;
        if (first >= 0xf0) length = 4;
        else if (first >= 0xe0) length = 3;
        else if (first >= 0xc0) length = 2;
        if (index + length > text.size())
            return -1;
        for (size_t next = 1; next < length; ++next)
            if ((static_cast<unsigned char>(text[index + next]) & 0xc0) != 0x80)
                return -1;
        index += length;
        ++count;
    }
    return count;
}
} // namespace

bool keyboardCall(const Json &request, KeyboardCall &call, std::string &error) {
    call = KeyboardCall();
    error.clear();
    if (!request.isObject()) {
        error = "no request";
        return false;
    }
    const std::string action = request["action"].string();
    call.action = action;
    call.payload = Json::object();
    if (action == "text") {
        const Json text = request["text"];
        if (!text.isString() || text.string().empty()) {
            error = "invalid text";
            return false;
        }
        const long characters = characterCount(text.string());
        if (characters < 1 || characters > static_cast<long>(kMaximumCharacters)) {
            error = "invalid text";
            return false;
        }
        const Json replace = request["replace"];
        if (request.has("replace") && !replace.isBoolean()) {
            error = "invalid replace";
            return false;
        }
        call.uri = "com.webos.service.ime/insertText";
        call.payload.set("text", text);
        call.payload.set("replace", Json::flag(replace.boolean()));
        return true;
    }
    if (action == "delete") {
        double count = 1;
        if (request.has("count")) {
            const Json value = request["count"];
            if (!value.isNumber() || !std::isfinite(value.number()) ||
                std::floor(value.number()) != value.number() || value.number() < 1 ||
                value.number() > kMaximumDelete) {
                error = "invalid count";
                return false;
            }
            count = value.number();
        }
        call.uri = "com.webos.service.ime/deleteCharacters";
        call.payload.set("count", Json::numeric(count));
        return true;
    }
    if (action == "enter") {
        call.uri = "com.webos.service.ime/sendEnterKey";
        return true;
    }
    error = action.empty() ? "no action" : "unknown action: " + action;
    return false;
}

} // namespace service
} // namespace nativetv
