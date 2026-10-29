#include "service/home_recent_history.hpp"
#include <algorithm>
#include <map>
#include <utility>
#include <vector>

namespace nativetv {
namespace service {
namespace {
const char *kRows[] = {"app", "dev", "ch"};
const size_t kMaximumIds = 100;
const size_t kMaximumIdUnits = 200;

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
            return {static_cast<uint32_t>(((first & 0x1f) << 6) |
                                          (second & 0x3f)), 2};
    }
    if ((first & 0xf0) == 0xe0 && offset + 2 < text.size()) {
        const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
        const unsigned char third = static_cast<unsigned char>(text[offset + 2]);
        if ((second & 0xc0) == 0x80 && (third & 0xc0) == 0x80)
            return {static_cast<uint32_t>(((first & 0x0f) << 12) |
                                          ((second & 0x3f) << 6) |
                                          (third & 0x3f)), 3};
    }
    if ((first & 0xf8) == 0xf0 && offset + 3 < text.size()) {
        const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
        const unsigned char third = static_cast<unsigned char>(text[offset + 2]);
        const unsigned char fourth = static_cast<unsigned char>(text[offset + 3]);
        if ((second & 0xc0) == 0x80 && (third & 0xc0) == 0x80 &&
            (fourth & 0xc0) == 0x80)
            return {static_cast<uint32_t>(((first & 0x07) << 18) |
                                          ((second & 0x3f) << 12) |
                                          ((third & 0x3f) << 6) |
                                          (fourth & 0x3f)), 4};
    }
    return {first, 1};
}

bool validId(const std::string &id) {
    if (id.empty()) return false;
    size_t units = 0;
    for (size_t offset = 0; offset < id.size();) {
        const CodePoint point = decode(id, offset);
        units += point.value > 0xffff ? 2 : 1;
        if (units > kMaximumIdUnits) return false;
        offset += point.width;
    }
    return true;
}

bool validRow(const std::string &row) {
    for (const char *candidate : kRows)
        if (row == candidate) return true;
    return false;
}

bool replacementRequested(const Json &rows, const std::string &row) {
    for (const Json &candidate : rows.items())
        if (candidate.isString() && candidate.string() == row) return true;
    return false;
}

Json cleanRow(const Json &value) {
    std::map<std::string, double> entries;
    if (value.isObject()) {
        for (const std::string &id : value.keys())
            if (validId(id) && value[id].isNumber())
                entries[id] = value[id].number();
    }
    if (entries.size() > kMaximumIds) {
        std::vector<std::pair<std::string, double>> oldest(entries.begin(), entries.end());
        std::sort(oldest.begin(), oldest.end(), [](const auto &left, const auto &right) {
            if (left.second != right.second) return left.second < right.second;
            return left.first < right.first;
        });
        for (size_t index = 0; index < oldest.size() - kMaximumIds; ++index)
            entries.erase(oldest[index].first);
    }
    Json result = Json::object();
    for (const auto &entry : entries)
        result.set(entry.first, Json::numeric(entry.second));
    return result;
}
} // namespace

Json mergeHomeRecentHistory(const Json &current, const Json &incoming,
                            const Json &replaceRows) {
    Json result = Json::object();
    for (const char *rowName : kRows) {
        const std::string row(rowName);
        Json values = Json::object();
        if (!replacementRequested(replaceRows, row) && current[row].isObject()) {
            for (const std::string &id : current[row].keys())
                if (validId(id) && current[row][id].isNumber())
                    values.set(id, current[row][id]);
        }
        if (incoming[row].isObject()) {
            for (const std::string &id : incoming[row].keys()) {
                if (!validId(id) || !incoming[row][id].isNumber()) continue;
                if (!values[id].isNumber() ||
                    incoming[row][id].number() > values[id].number())
                    values.set(id, incoming[row][id]);
            }
        }
        result.set(row, cleanRow(values));
    }
    return result;
}

Json stampHomeRecentHistory(const Json &current, const std::string &row,
                            const std::string &id, int64_t timestamp) {
    if (!validRow(row) || !validId(id)) return mergeHomeRecentHistory(current, Json::object(), Json());
    Json incoming = Json::object();
    for (const char *rowName : kRows) incoming.set(rowName, Json::object());
    Json values = Json::object();
    values.set(id, Json::numeric(static_cast<double>(timestamp)));
    incoming.set(row, values);
    return mergeHomeRecentHistory(current, incoming, Json());
}

} // namespace service
} // namespace nativetv
