#pragma once

#include <cstddef>
#include <string>

namespace nativetv {
namespace say {
// Translating the messages msg() makes, for a dashboard in another language.
// msg() still returns English, so everything that compares, joins or logs it
// goes on as before. Once a dictionary is loaded it also remembers which key
// and English template each rendered message came from, and translateJson()
// swaps those messages for their translations in the fields a page shows.

// Lists <directory>/<xx>.json (a two-letter code, or one with a region as in
// pt-br.json, for a language whose regions have files of their own), each
// { "key": { "text": "...", "from": "the English it was made from" } }, and
// starts remembering messages. A language is read the first time a request
// asks for it, not here, so a TV holds only the dictionaries that dashboards
// use. Returns the number of files listed; none leaves msg() as it was.
// Call it before serving, once: it replaces any listed before.
size_t loadTranslations(const std::string &directory);

// The language a request asks for in its language header, when a dictionary is
// loaded for it. English, anything unknown and no header are all "".
std::string requestedLanguage(const std::string &headerValue);

// A JSON body with the messages in its display fields (error, label, title and
// the like) replaced by their translations. Raw values, the strings of fields
// a page does not show, and anything that is not JSON come back as they were.
std::string translateJson(const std::string &body, const std::string &language);
} // namespace say
} // namespace nativetv
