#include "domain/say.hpp"

namespace nativetv {
namespace say {
namespace {
bool wordCharacter(unsigned char value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || value == '_';
}

std::string fill(const std::string &text, const Json &variables) {
    if (!variables.isObject())
        return text;
    std::string result;
    for (size_t index = 0; index < text.size();) {
        if (text[index] != '{') {
            result.push_back(text[index++]);
            continue;
        }
        size_t end = index + 1;
        while (end < text.size() && wordCharacter(static_cast<unsigned char>(text[end])))
            ++end;
        if (end == index + 1 || end >= text.size() || text[end] != '}') {
            result.push_back(text[index++]);
            continue;
        }
        const std::string name = text.substr(index + 1, end - index - 1);
        const Json value = variables[name];
        if (value.valid())
            result += value.string();
        else
            result.append(text, index, end - index + 1);
        index = end + 1;
    }
    return result;
}
} // namespace

std::string msg(const std::string &key, const std::string &english, const Json &variables) {
    (void)key;
    return fill(english, variables);
}

std::string list(const std::vector<std::string> &items) {
    if (items.size() < 2)
        return items.empty() ? "" : items.front();
    std::string preceding;
    for (size_t index = 0; index + 1 < items.size(); ++index) {
        if (index > 0)
            preceding += ", ";
        preceding += items[index];
    }
    Json variables = Json::object();
    variables.set("items", Json::text(preceding));
    variables.set("last", Json::text(items.back()));
    return msg("srv.list", "{items} and {last}", variables);
}
} // namespace say
} // namespace nativetv
