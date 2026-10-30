#pragma once
#include <string>
#include <vector>

namespace nativetv {
class Keyboard {
    std::string value_;
    size_t cursor_ = 0, maximum_ = 80;
    bool upper_ = false, symbols_ = false, address_ = false, digits_ = false;

  public:
    // `address`: a host name or IP is being typed, so the letters layer offers "." where a name
    // offers an apostrophe. `digits`: a number is being typed (a PIN), so only the digits go in and
    // the pad is a row of them over delete and Done.
    void reset(const std::string &value, size_t maximum, bool address = false, bool digits = false);
    bool insert(const std::string &text);
    bool erase();
    bool press(const std::string &key);
    void clear();
    std::string value() const {
        return value_;
    }
    size_t cursor() const {
        return cursor_;
    }
    bool upper() const {
        return upper_;
    }
    bool symbols() const {
        return symbols_;
    }
    // The rows the editors draw today.
    std::vector<std::vector<std::string>> rows() const;
    // The designed pad (spec 3.6): digits, letters, shift and delete around the last letters,
    // then symbols, caret keys, space and Done. Key ids are "shift", "delete", "symbols", "left",
    // "right", "space", "done" and the characters themselves.
    std::vector<std::vector<std::string>> padRows() const;
    std::string label(const std::string &key) const;
};
} // namespace nativetv
