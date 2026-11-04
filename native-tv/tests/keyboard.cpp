#include "service/keyboard.hpp"
#include <iostream>
#include <string>

using namespace nativetv;

namespace {
int failures = 0;

void check(bool condition, const std::string &message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

bool refused(const std::string &request, const std::string &reason) {
    service::KeyboardCall call;
    std::string error;
    return !service::keyboardCall(Json::parse(request), call, error) && error == reason &&
           call.uri.empty();
}
} // namespace

int main() {
    service::KeyboardCall call;
    std::string error;

    check(service::keyboardCall(Json::parse("{\"action\":\"text\",\"text\":\"hello world\"}"),
                                call, error) &&
              call.uri == "com.webos.service.ime/insertText" &&
              call.payload["text"].string() == "hello world" &&
              call.payload["replace"].isBoolean() && !call.payload["replace"].boolean() &&
              error.empty(),
          "text is typed at the cursor by default");
    check(service::keyboardCall(
              Json::parse("{\"action\":\"text\",\"text\":\"x\",\"replace\":true}"), call, error) &&
              call.payload["replace"].boolean(),
          "replace swaps the field's text");
    check(service::keyboardCall(
              Json::parse("{\"action\":\"text\",\"text\":\"caf\\u00e9 \\ud83d\\ude00 \\u65e5\\u672c\"}"),
              call, error) &&
              call.payload["text"].string() == "caf\xc3\xa9 \xf0\x9f\x98\x80 \xe6\x97\xa5\xe6\x9c\xac",
          "text with accents, emoji and other scripts is passed on whole");

    // The limit is in characters, not bytes.
    std::string accented;
    for (int index = 0; index < 256; ++index) accented += "\\u00e9";
    check(service::keyboardCall(
              Json::parse("{\"action\":\"text\",\"text\":\"" + accented + "\"}"), call, error),
          "256 characters is accepted even where that is 512 bytes");
    check(refused("{\"action\":\"text\",\"text\":\"" + accented + "a\"}", "invalid text"),
          "257 characters is refused");

    for (const char *text : {"\"\"", "5", "null", "[\"a\"]", "\"line\\nbreak\"", "\"tab\\there\"",
                             "\"bell\\u0007\"", "\"del\\u007f\""})
        check(refused(std::string("{\"action\":\"text\",\"text\":") + text + "}", "invalid text"),
              std::string("text ") + text + " is refused");
    check(refused("{\"action\":\"text\"}", "invalid text"), "no text is refused");
    check(refused("{\"action\":\"text\",\"text\":\"a\",\"replace\":1}", "invalid replace"),
          "a replace that is not a boolean is refused");

    check(service::keyboardCall(Json::parse("{\"action\":\"delete\"}"), call, error) &&
              call.uri == "com.webos.service.ime/deleteCharacters" &&
              call.payload["count"].number() == 1,
          "delete takes one character by default");
    check(service::keyboardCall(Json::parse("{\"action\":\"delete\",\"count\":100}"), call, error) &&
              call.payload["count"].number() == 100,
          "delete takes up to 100");
    for (const char *count : {"0", "101", "-1", "1.5", "\"3\"", "null", "true"})
        check(refused(std::string("{\"action\":\"delete\",\"count\":") + count + "}",
                      "invalid count"),
              std::string("delete count ") + count + " is refused");

    check(service::keyboardCall(Json::parse("{\"action\":\"enter\"}"), call, error) &&
              call.uri == "com.webos.service.ime/sendEnterKey" && call.payload.isObject() &&
              call.payload.keys().empty(),
          "enter sends no parameters");
    check(refused("{\"action\":\"type\"}", "unknown action: type") &&
              refused("{}", "no action") && refused("[]", "no request") && refused("\"text\"", "no request"),
          "an unknown or missing action, or a request that is not an object, is refused");

    if (failures) {
        std::cerr << failures << " keyboard check(s) failed\n";
        return 1;
    }
    std::cout << "keyboard checks passed\n";
    return 0;
}
