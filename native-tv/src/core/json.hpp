#pragma once
#include <json-c/json.h>
#include <string>
#include <vector>

namespace nativetv {
class Json {
    json_object *value_;

  public:
    Json();
    explicit Json(json_object *value, bool retain = true);
    Json(const Json &other);
    Json(Json &&other) noexcept;
    Json &operator=(Json other);
    ~Json();
    static Json parse(const std::string &text);
    static Json object();
    static Json array();
    static Json text(const std::string &value);
    static Json numeric(double value);
    static Json flag(bool value);
    Json operator[](const std::string &key) const;
    Json operator[](size_t index) const;
    bool has(const std::string &key) const;
    bool valid() const;
    bool isArray() const;
    bool isObject() const;
    bool isBoolean() const;
    bool isNumber() const;
    bool isString() const;
    bool boolean(bool fallback = false) const;
    double number(double fallback = 0) const;
    std::string string(const std::string &fallback = "") const;
    std::string dump(bool escapeSlashes = true) const;
    size_t size() const;
    std::vector<std::string> keys() const;
    std::vector<Json> items() const;
    void set(const std::string &key, const Json &value);
    void add(const Json &value);
};
std::string encode(const std::string &text);
std::string compactNumber(double number, int precision = 0);
} // namespace nativetv
