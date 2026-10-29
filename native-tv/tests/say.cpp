#include "domain/say.hpp"
#include <cstdio>
#include <stdexcept>

using namespace nativetv;

namespace {
unsigned checks = 0;

void check(bool value, const char *message) {
    ++checks;
    if (!value)
        throw std::runtime_error(message);
}
} // namespace

int main() {
    try {
        check(say::msg("srv.power.on", "On") == "On", "English fallback");
        check(say::msg("srv.update.failed", "The update failed: {error}",
                       Json::parse("{\"error\":\"disk full\"}")) ==
                  "The update failed: disk full",
              "Message placeholder substitution");
        check(say::msg("srv.update.failed", "The update failed: {error}", Json::object()) ==
                  "The update failed: {error}",
              "Missing placeholder remains intact");
        check(say::msg("srv.privacy.running", "{name} is running",
                       Json::parse("{\"name\":\"Diagnostics uploader\"}")) ==
                  "Diagnostics uploader is running",
              "Nested message text substitution");
        check(say::list({}) == "" && say::list({"Only"}) == "Only" &&
                  say::list({"Terms", "Privacy", "Ads"}) == "Terms, Privacy and Ads",
              "Natural-language lists");
        std::puts("5 native message checks passed");
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
