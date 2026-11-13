#include "core/json.hpp"
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace nativetv {
namespace {
// json-c counts references with plain integers unless it was built with threading
// (the usual build is not), and keeps scratch buffers inside its objects. An answer
// cached for the next request is copied, read and dropped by several request
// threads at once, which corrupted the heap on a TV ("double free detected"). Every
// access to a json-c object takes this one lock; recursive, as the accessors call each other.
// Never destroyed: a static Json (the HA discovery templates) is released at exit after a
// function-local mutex first used later would be gone, and locking it then aborts on macOS.
std::recursive_mutex &objectLock() {
    static std::recursive_mutex *const lock = new std::recursive_mutex;
    return *lock;
}
// json-c 0.18 compares each byte of a string, as a signed char, with 0x1f in strict mode, so every
// byte from 0x80 up reads as a control character and any non-ASCII text is refused (fixed in 0.19;
// Ubuntu 26.04 ships 0.18). Inside a string, each valid UTF-8 sequence is rewritten as the \u escape
// of the same character, which parses to the same text. Anything else, an invalid sequence included,
// is left as it is.
bool hasHighByte(const std::string &text) {
    for (unsigned char c : text)
        if (c >= 0x80)
            return true;
    return false;
}
std::string escapeNonAscii(const std::string &text) {
    std::string out;
    out.reserve(text.size() + text.size() / 4);
    bool inString = false, afterBackslash = false;
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (inString && c >= 0xC2 && c < 0xF5) {
            const size_t length = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
            unsigned long point = length == 4 ? c & 7 : length == 3 ? c & 15 : c & 31;
            bool valid = i + length <= text.size();
            for (size_t k = 1; valid && k < length; ++k) {
                const unsigned char next = static_cast<unsigned char>(text[i + k]);
                valid = (next & 0xC0) == 0x80;
                point = (point << 6) | (next & 0x3F);
            }
            valid = valid && point >= (length == 2 ? 0x80ul : length == 3 ? 0x800ul : 0x10000ul) &&
                    point <= 0x10FFFF && !(point >= 0xD800 && point <= 0xDFFF);
            if (valid) {
                char escape[32];
                if (point < 0x10000) {
                    std::snprintf(escape, sizeof escape, "\\u%04x", static_cast<unsigned>(point));
                } else {
                    point -= 0x10000;
                    std::snprintf(escape, sizeof escape, "\\u%04x\\u%04x",
                                  static_cast<unsigned>(0xD800 + (point >> 10)),
                                  static_cast<unsigned>(0xDC00 + (point & 0x3FF)));
                }
                out += escape;
                i += length;
                continue;
            }
        }
        out += static_cast<char>(c);
        if (inString) {
            if (afterBackslash)
                afterBackslash = false;
            else if (c == '\\')
                afterBackslash = true;
            else if (c == '"')
                inString = false;
        } else if (c == '"') {
            inString = true;
        }
        ++i;
    }
    return out;
}
} // namespace

Json::Json() : value_(nullptr) {}
Json::Json(json_object *value, bool retain) : value_(value) {
    if (retain && value_) {
        std::lock_guard<std::recursive_mutex> lock(objectLock());
        json_object_get(value_);
    }
}
Json::Json(const Json &other) : Json(other.value_) {}
Json::Json(Json &&other) noexcept : value_(other.value_) {
    other.value_ = nullptr;
}
Json &Json::operator=(Json other) {
    std::swap(value_, other.value_);
    return *this;
}
Json::~Json() {
    if (value_) {
        std::lock_guard<std::recursive_mutex> lock(objectLock());
        json_object_put(value_);
    }
}
Json Json::parse(const std::string &original) {
    if (original.empty() || original.size() > 8 * 1024 * 1024)
        return Json();
    const std::string escaped = hasHighByte(original) ? escapeNonAscii(original) : std::string();
    const std::string &text = escaped.empty() ? original : escaped;
    json_tokener *tokener = json_tokener_new_ex(48);
    if (!tokener)
        return Json();
    json_tokener_set_flags(tokener, JSON_TOKENER_STRICT);
    json_object *object =
        json_tokener_parse_ex(tokener, text.c_str(), static_cast<int>(text.size()) + 1);
    size_t parsed = json_tokener_get_parse_end(tokener);
    while (parsed < text.size() &&
           (text[parsed] == ' ' || text[parsed] == '\t' ||
            text[parsed] == '\n' || text[parsed] == '\r'))
        ++parsed;
    bool ok = json_tokener_get_error(tokener) == json_tokener_success && parsed == text.size();
    json_tokener_free(tokener);
    if (!ok) {
        if (object)
            json_object_put(object);
        return Json();
    }
    return Json(object, false);
}
Json Json::object() {
    return Json(json_object_new_object(), false);
}
Json Json::array() {
    return Json(json_object_new_array(), false);
}
Json Json::text(const std::string &value) {
    return Json(json_object_new_string_len(value.data(), static_cast<int>(value.size())), false);
}
Json Json::numeric(double value) {
    return Json(json_object_new_double(value), false);
}
Json Json::flag(bool value) {
    return Json(json_object_new_boolean(value), false);
}
Json Json::operator[](const std::string &key) const {
    std::lock_guard<std::recursive_mutex> lock(objectLock());
    if (!isObject())
        return Json();
    json_object *result = nullptr;
    return json_object_object_get_ex(value_, key.c_str(), &result) ? Json(result) : Json();
}
Json Json::operator[](size_t index) const {
    std::lock_guard<std::recursive_mutex> lock(objectLock());
    return isArray() && index < size() ? Json(json_object_array_get_idx(value_, index)) : Json();
}
bool Json::has(const std::string &key) const {
    std::lock_guard<std::recursive_mutex> lock(objectLock());
    if (!isObject())
        return false;
    json_object *ignored = nullptr;
    return json_object_object_get_ex(value_, key.c_str(), &ignored);
}
bool Json::valid() const {
    std::lock_guard<std::recursive_mutex> lock(objectLock());
    return value_ != nullptr;
}
bool Json::isArray() const {
    std::lock_guard<std::recursive_mutex> lock(objectLock());
    return value_ && json_object_get_type(value_) == json_type_array;
}
bool Json::isObject() const {
    std::lock_guard<std::recursive_mutex> lock(objectLock());
    return value_ && json_object_get_type(value_) == json_type_object;
}
bool Json::isBoolean() const {
    std::lock_guard<std::recursive_mutex> lock(objectLock());
    return value_ && json_object_get_type(value_) == json_type_boolean;
}
bool Json::isNumber() const {
    std::lock_guard<std::recursive_mutex> lock(objectLock());
    if (!value_)
        return false;
    json_type type = json_object_get_type(value_);
    return type == json_type_int || type == json_type_double;
}
bool Json::isString() const {
    std::lock_guard<std::recursive_mutex> lock(objectLock());
    return value_ && json_object_get_type(value_) == json_type_string;
}
bool Json::boolean(bool fallback) const {
    std::lock_guard<std::recursive_mutex> lock(objectLock());
    return value_ ? json_object_get_boolean(value_) != 0 : fallback;
}
double Json::number(double fallback) const {
    std::lock_guard<std::recursive_mutex> lock(objectLock());
    return value_ ? json_object_get_double(value_) : fallback;
}
std::string Json::string(const std::string &fallback) const {
    std::lock_guard<std::recursive_mutex> lock(objectLock());
    return value_ ? json_object_get_string(value_) : fallback;
}
std::string Json::dump(bool escapeSlashes) const {
    // json-c writes the text into a buffer kept inside the object: dumps take turns.
    std::lock_guard<std::recursive_mutex> lock(objectLock());
    int flags = JSON_C_TO_STRING_PLAIN;
    if (!escapeSlashes)
        flags |= JSON_C_TO_STRING_NOSLASHESCAPE;
    return value_ ? json_object_to_json_string_ext(value_, flags) : "null";
}
size_t Json::size() const {
    std::lock_guard<std::recursive_mutex> lock(objectLock());
    return isArray() ? json_object_array_length(value_) : 0;
}
std::vector<std::string> Json::keys() const {
    std::lock_guard<std::recursive_mutex> lock(objectLock());
    std::vector<std::string> result;
    if (!isObject())
        return result;
    json_object_object_foreach(value_, key, item) {
        (void)item;
        result.push_back(key);
    }
    return result;
}
std::vector<Json> Json::items() const {
    std::vector<Json> result;
    for (size_t i = 0; i < size(); ++i)
        result.push_back((*this)[i]);
    return result;
}
void Json::set(const std::string &key, const Json &value) {
    std::lock_guard<std::recursive_mutex> lock(objectLock());
    if (isObject())
        json_object_object_add(value_, key.c_str(),
                               value.value_ ? json_object_get(value.value_) : nullptr);
}
void Json::add(const Json &value) {
    std::lock_guard<std::recursive_mutex> lock(objectLock());
    if (isArray())
        json_object_array_add(value_, value.value_ ? json_object_get(value.value_) : nullptr);
}
std::string encode(const std::string &text) {
    const char hex[] = "0123456789ABCDEF";
    std::string result;
    for (unsigned char c : text) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~')
            result += c;
        else {
            result += '%';
            result += hex[c >> 4];
            result += hex[c & 15];
        }
    }
    return result;
}
std::string compactNumber(double number, int precision) {
    char text[64];
    std::snprintf(text, sizeof(text), "%.*f", precision, number);
    return text;
}
} // namespace nativetv
