#include "ui/keyboard.hpp"
#include <algorithm>
#include <cctype>

namespace nativetv {
namespace {
size_t previous(const std::string &value, size_t cursor) {
    if (!cursor)
        return 0;
    --cursor;
    while (cursor && (static_cast<unsigned char>(value[cursor]) & 0xc0) == 0x80)
        --cursor;
    return cursor;
}
size_t next(const std::string &value, size_t cursor) {
    if (cursor >= value.size())
        return value.size();
    ++cursor;
    while (cursor < value.size() && (static_cast<unsigned char>(value[cursor]) & 0xc0) == 0x80)
        ++cursor;
    return cursor;
}
size_t length(const std::string &value) {
    size_t count = 0;
    for (unsigned char c : value)
        if ((c & 0xc0) != 0x80)
            ++count;
    return count;
}
std::vector<std::string> split(const std::string &value) {
    std::vector<std::string> keys;
    for (char c : value)
        keys.push_back(std::string(1, c));
    return keys;
}
} // namespace
void Keyboard::reset(const std::string &value, size_t maximum, bool address) {
    value_ = value;
    maximum_ = maximum;
    address_ = address;
    cursor_ = value_.size();
    upper_ = symbols_ = false;
}
bool Keyboard::insert(const std::string &text) {
    if (text.empty() || length(value_) + length(text) > maximum_ ||
        text.find_first_of("\r\n\t") != std::string::npos)
        return false;
    value_.insert(cursor_, text);
    cursor_ += text.size();
    return true;
}
bool Keyboard::erase() {
    if (!cursor_)
        return false;
    size_t start = previous(value_, cursor_);
    value_.erase(start, cursor_ - start);
    cursor_ = start;
    return true;
}
bool Keyboard::press(const std::string &key) {
    if (key == "delete")
        return erase();
    if (key == "shift") {
        upper_ = !upper_;
        return false;
    }
    if (key == "symbols") {
        symbols_ = !symbols_;
        return false;
    }
    if (key == "left") {
        cursor_ = previous(value_, cursor_);
        return false;
    }
    if (key == "right") {
        cursor_ = next(value_, cursor_);
        return false;
    }
    if (key == "done")
        return false;
    return insert(key == "space" ? " " : label(key));
}
void Keyboard::clear() {
    value_.clear();
    cursor_ = 0;
}
std::vector<std::vector<std::string>> Keyboard::rows() const {
    auto fourth = split(symbols_ ? "'\"<>\\|~," : "zxcvbnm,");
    fourth.insert(fourth.begin(), "shift");
    fourth.push_back("delete");
    return {split("1234567890"),
            split(symbols_ ? "!@#$%&*()?" : "qwertyuiop"),
            split(symbols_ ? "[]{}+=/:;." : "asdfghjkl."),
            fourth,
            {"symbols", "left", "right", "space", "-", "_", "done"}};
}
std::vector<std::vector<std::string>> Keyboard::padRows() const {
    std::vector<std::string> fourth = split(symbols_ ? "-_,\"'<>~" : "zxcvbnm");
    if (!symbols_)
        fourth.insert(fourth.begin(), "shift");
    fourth.push_back("delete");
    return {split("1234567890"),
            split(symbols_ ? "!@#$%&*()?" : "qwertyuiop"),
            split(symbols_ ? "[]{}+=/:;." : address_ ? "asdfghjkl." : "asdfghjkl'"),
            fourth,
            {"symbols", "left", "right", "space", "done"}};
}
std::string Keyboard::label(const std::string &key) const {
    if (key == "shift")
        return "Shift";
    if (key == "delete")
        return "Delete";
    if (key == "symbols")
        return symbols_ ? "ABC" : "123?";
    if (key == "space")
        return "Space";
    if (key == "left")
        return "<";
    if (key == "right")
        return ">";
    if (key == "done")
        return "Done";
    std::string value = key;
    if (upper_)
        for (char &c : value)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return value;
}
} // namespace nativetv
