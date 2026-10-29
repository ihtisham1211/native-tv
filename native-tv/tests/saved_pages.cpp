#include "service/saved_pages.hpp"
#include <cassert>
#include <iostream>
#include <string>

using namespace nativetv::service;

int main() {
    SavedPageTarget target;
    assert(savedPageTarget("  example.com/watch?q=1  ", target));
    assert(target.url == "https://example.com/watch?q=1");
    assert(target.host == "example.com");

    assert(savedPageTarget("HTTP://www.Example.com:8080/path", target));
    assert(target.url == "HTTP://www.Example.com:8080/path");
    assert(target.host == "Example.com");
    assert(savedPageTarget("http://localhost:99999", target));
    assert(target.host == "localhost");
    assert(savedPageTarget("https://192.168.1.20/", target));
    assert(!savedPageTarget("javascript:alert(1)", target));
    assert(!savedPageTarget("http://user@example.com", target));
    assert(!savedPageTarget("http://256.1.1.1", target));
    assert(!savedPageTarget("https://example.com:123456", target));
    assert(!savedPageTarget("https://example.com/a b", target));

    assert(normalizeSavedPageTitle(" \tA\n  saved\u00a0page \t") == "A saved page");
    assert(normalizeSavedPageTitle("  ").empty());
    assert(savedPageTitleLength(std::string(60, 'x')) == 60);
    assert(savedPageTitleLength(std::string(61, 'x')) == 61);
    assert(savedPageTitleLength("TV \xF0\x9F\x8E\xAC") == 5);
    assert(savedPageBookmark("com.webos.app.browser", "bookmark",
                             "com.webos.app.browser"));
    assert(!savedPageBookmark("com.webos.app.browser", "app",
                              "com.webos.app.browser"));
    assert(!savedPageBookmark("com.webos.app.settings", "bookmark",
                              "com.webos.app.browser"));

    std::cout << "Saved page validation passed.\n";
    return 0;
}
