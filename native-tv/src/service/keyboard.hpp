#pragma once

#include "core/json.hpp"
#include <string>

namespace nativetv {
namespace service {

// A call to LG's text-input service (com.webos.service.ime), which types into the
// text field the TV has open.
struct KeyboardCall {
    std::string action; // "text", "delete" or "enter"
    std::string uri;
    Json payload;
};

// Turns a request into the call. {"action":"text","text":"...","replace":false} types
// the text (at most 256 characters, no control characters; replace swaps the field's
// text for it), {"action":"delete","count":1} deletes 1 to 100 characters, and
// {"action":"enter"} presses Enter. On a request that is none of those, false and
// the reason.
bool keyboardCall(const Json &request, KeyboardCall &call, std::string &error);

} // namespace service
} // namespace nativetv
