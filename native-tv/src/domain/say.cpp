#include "domain/say.hpp"
#include "domain/say_translation.hpp"
#include <atomic>
#include <dirent.h>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace nativetv {
namespace say {
namespace {
bool wordCharacter(unsigned char value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || value == '_';
}

// text with each {name} replaced by what lookup gives for it; a name it does
// not know stays as written.
template <typename Lookup> std::string fillWith(const std::string &text, Lookup lookup) {
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
        std::string value;
        if (lookup(text.substr(index + 1, end - index - 1), value))
            result += value;
        else
            result.append(text, index, end - index + 1);
        index = end + 1;
    }
    return result;
}

std::string fill(const std::string &text, const Json &variables) {
    if (!variables.isObject())
        return text;
    return fillWith(text, [&variables](const std::string &name, std::string &value) {
        const Json item = variables[name];
        if (!item.valid())
            return false;
        value = item.string();
        return true;
    });
}

// Where a rendered message came from, to translate it by.
struct Origin {
    std::string key;
    std::string english;
    std::map<std::string, std::string> values;
};
struct Entry {
    std::string text;
    std::string from;
};
using Dictionary = std::unordered_map<std::string, Entry>;
using Origins = std::unordered_map<std::string, Origin>;

// A fixed message, such as a screen saver's name, is made from the same few
// hundred templates; one with values makes a new entry for each value, so its
// memory is bounded harder. Either map starts again when it fills, which at
// worst shows English for a message a cached response made long ago.
const size_t kMaximumFixed = 4096;
const size_t kMaximumFilled = 1000;
// Lets msg() skip the bookkeeping when there is no dictionary: the TV client
// shares this code and makes messages on every frame.
std::atomic<bool> translating(false);
std::mutex originsMutex; // msg() runs on every HTTP worker
Origins fixedOrigins;
Origins filledOrigins;
std::map<std::string, Dictionary> dictionaries; // changed only by loadTranslations, before serving

void remember(const std::string &text, const std::string &key, const std::string &english,
              const Json &variables) {
    Origin origin;
    origin.key = key;
    origin.english = english;
    const bool withValues = variables.isObject();
    if (withValues)
        for (const std::string &name : variables.keys())
            origin.values[name] = variables[name].string();
    std::lock_guard<std::mutex> lock(originsMutex);
    Origins &origins = withValues ? filledOrigins : fixedOrigins;
    if (!origins.count(text) && origins.size() >= (withValues ? kMaximumFilled : kMaximumFixed))
        origins.clear();
    origins[text] = std::move(origin);
}

bool lookupOrigin(const std::string &text, Origin &origin) {
    std::lock_guard<std::mutex> lock(originsMutex);
    for (const Origins *origins : {&fixedOrigins, &filledOrigins}) {
        const Origins::const_iterator found = origins->find(text);
        if (found != origins->end()) {
            origin = found->second;
            return true;
        }
    }
    return false;
}

bool translateText(const std::string &text, const Dictionary &dictionary, std::string &translated,
                   int depth = 0);

// A value can be a message itself, such as a service's name, or a few joined
// by list() ("A, B").
bool translateValue(const std::string &value, const Dictionary &dictionary, std::string &translated,
                    int depth) {
    if (depth > 3)
        return false;
    if (translateText(value, dictionary, translated, depth))
        return true;
    if (value.find(", ") == std::string::npos)
        return false;
    bool any = false;
    translated.clear();
    for (size_t start = 0; start <= value.size();) {
        size_t end = value.find(", ", start);
        const std::string piece = value.substr(start, end == std::string::npos ? end : end - start);
        std::string one;
        any = translateText(piece, dictionary, one, depth) || any;
        translated += (start ? ", " : "") + (one.empty() ? piece : one);
        if (end == std::string::npos)
            break;
        start = end + 2;
    }
    return any;
}

bool translateText(const std::string &text, const Dictionary &dictionary, std::string &translated,
                   int depth) {
    Origin origin;
    if (!lookupOrigin(text, origin))
        return false;
    const Dictionary::const_iterator entry = dictionary.find(origin.key);
    // Made from other English than the code has now: show the English until it is redone.
    if (entry == dictionary.end() || entry->second.from != origin.english)
        return false;
    translated = fillWith(entry->second.text, [&](const std::string &name, std::string &value) {
        const std::map<std::string, std::string>::const_iterator found = origin.values.find(name);
        if (found == origin.values.end())
            return false;
        std::string inner;
        value = translateValue(found->second, dictionary, inner, depth + 1) ? inner : found->second;
        return true;
    });
    return true;
}

// The fields a page shows, as opposed to raw values that happen to read the
// same as a message (a TV state such as "Running").
bool displayField(const std::string &field) {
    static const char *const names[] = {
        "error", "note", "label", "name", "detail", "desc", "description", "title", "badge",
        "message", "hint", "reason", "limitTrackingLabel", "limitTrackingDetail",
        // Lists of the names above, which a page joins into a sentence.
        "sharesWith", "agreements", "kept", "cautions",
        // [id, title, note] rows: only the strings msg() made are changed, never an id.
        "consentGroups"};
    for (const char *name : names)
        if (field == name)
            return true;
    return false;
}

// Translates node in place where it is an object; a string or an array is
// handed back as `replacement` for its owner to put in. `changed` records any
// change at all, so a body nothing in is touched is sent as it was.
bool translateNode(const Json &node, const std::string &field, const Dictionary &dictionary,
                   bool &changed, Json &replacement) {
    if (node.isString()) {
        std::string translated;
        if (!displayField(field) || !translateText(node.string(), dictionary, translated))
            return false;
        replacement = Json::text(translated);
        return changed = true;
    }
    if (node.isArray()) {
        std::vector<Json> items = node.items();
        bool any = false;
        for (Json &item : items) {
            Json translated;
            if (translateNode(item, field, dictionary, changed, translated)) {
                item = translated;
                any = true;
            }
        }
        if (!any)
            return false;
        replacement = Json::array();
        for (const Json &item : items)
            replacement.add(item);
        return true;
    }
    if (node.isObject()) {
        Json object = node; // the same object: set() below changes it in place
        for (const std::string &name : node.keys()) {
            Json translated;
            if (translateNode(node[name], name, dictionary, changed, translated))
                object.set(name, translated);
        }
    }
    return false;
}

bool readFile(const std::string &path, std::string &contents) {
    std::ifstream file(path.c_str(), std::ios::binary);
    if (!file)
        return false;
    std::ostringstream buffer;
    buffer << file.rdbuf();
    contents = buffer.str();
    return true;
}

bool languageCode(const std::string &value) {
    return value.size() == 2 && value[0] >= 'a' && value[0] <= 'z' && value[1] >= 'a' &&
           value[1] <= 'z';
}
} // namespace

std::string msg(const std::string &key, const std::string &english, const Json &variables) {
    const std::string text = fill(english, variables);
    if (translating.load(std::memory_order_acquire))
        remember(text, key, english, variables);
    return text;
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

size_t loadTranslations(const std::string &directory) {
    std::map<std::string, Dictionary> loaded;
    if (DIR *folder = opendir(directory.c_str())) {
        while (const dirent *item = readdir(folder)) {
            const std::string name = item->d_name;
            std::string contents;
            if (name.size() != 7 || name.compare(2, 5, ".json") != 0 ||
                !languageCode(name.substr(0, 2)) || !readFile(directory + "/" + name, contents))
                continue;
            const Json document = Json::parse(contents);
            Dictionary dictionary;
            for (const std::string &key : document.keys()) {
                const Json entry = document[key];
                if (entry["text"].isString() && entry["from"].isString() &&
                    !entry["text"].string().empty())
                    dictionary[key] = {entry["text"].string(), entry["from"].string()};
            }
            if (!dictionary.empty())
                loaded[name.substr(0, 2)] = std::move(dictionary);
        }
        closedir(folder);
    }
    translating.store(false, std::memory_order_release);
    dictionaries.swap(loaded);
    {
        std::lock_guard<std::mutex> lock(originsMutex);
        fixedOrigins.clear();
        filledOrigins.clear();
    }
    translating.store(!dictionaries.empty(), std::memory_order_release);
    return dictionaries.size();
}

std::string requestedLanguage(const std::string &headerValue) {
    std::string code = headerValue.substr(0, 2);
    for (char &letter : code)
        if (letter >= 'A' && letter <= 'Z')
            letter = static_cast<char>(letter + ('a' - 'A'));
    if (headerValue.size() != 2 || !languageCode(code) ||
        !translating.load(std::memory_order_acquire) || !dictionaries.count(code))
        return std::string();
    return code;
}

std::string translateJson(const std::string &body, const std::string &language) {
    if (!translating.load(std::memory_order_acquire))
        return body;
    const std::map<std::string, Dictionary>::const_iterator dictionary = dictionaries.find(language);
    if (dictionary == dictionaries.end())
        return body;
    Json document = Json::parse(body);
    if (!document.valid())
        return body;
    bool changed = false;
    Json replacement;
    if (translateNode(document, "", dictionary->second, changed, replacement))
        document = replacement;
    return changed ? document.dump() : body;
}
} // namespace say
} // namespace nativetv
