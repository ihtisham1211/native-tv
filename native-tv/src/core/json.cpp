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
Json Json::parse(const std::string &text) {
    if (text.empty() || text.size() > 8 * 1024 * 1024)
        return Json();
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
