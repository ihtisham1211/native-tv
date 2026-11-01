// The phone dashboard is assembled once, on the first request for it, and the TV
// pages never need it (test/test-lazy-ui.js).
#include "daemon/http_server.hpp"
#include "daemon/http_static_assets.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

// Fixture folders are made and removed with the shell; a failure shows in the checks that read them.
static void runShell(const std::string &command) {
    if (std::system(command.c_str()) != 0) {
    }
}


using namespace nativetv::daemon;

namespace {
int failures = 0;
void check(bool condition, const char *what) {
    std::printf("%s %s\n", condition ? "  ok" : "FAIL", what);
    if (!condition) ++failures;
}
void put(const std::string &path, const std::string &text) {
    const size_t slash = path.rfind('/');
    runShell(("mkdir -p '" + path.substr(0, slash) + "'"));
    std::ofstream(path.c_str(), std::ios::binary | std::ios::trunc) << text;
}
bool has(const std::string &text, const std::string &part) { return text.find(part) != std::string::npos; }

struct Answer {
    int status = 0;
    std::string body;
    bool handled = false;
};
Answer get(const HttpStaticAssets &assets, const std::string &uri, bool gzip) {
    HttpRequest request;
    request.method = "GET";
    request.uri = uri;
    request.version = "1.1";
    request.remoteAddress = "127.0.0.1";
    if (gzip) request.headers.push_back({"Accept-Encoding", "gzip"});
    HttpExchange exchange(request, std::string());
    Answer answer;
    answer.handled = assets.handle(exchange);
    answer.status = exchange.responseStatus();
    answer.body = exchange.responseBody();
    return answer;
}
bool gzipped(const std::string &body) {
    return body.size() > 2 && static_cast<unsigned char>(body[0]) == 0x1f && static_cast<unsigned char>(body[1]) == 0x8b;
}
} // namespace

int main() {
    char pattern[] = "/tmp/nativetv-lazy-ui-XXXXXX";
    const std::string root = mkdtemp(pattern);
    put(root + "/google-tv/pages/dashboard/index.html", "<html>TV dashboard</html>");
    put(root + "/google-tv/pages/setup/index.html", "<html>TV setup</html>");
    const HttpStaticAssets assets(root);

    check(get(assets, "/assets/google-tv/pages/dashboard/index.html", false).status == 200 &&
              get(assets, "/assets/google-tv/pages/setup/index.html", false).status == 200,
          "the TV pages are served");
    // ui.html appears only now: had a TV page request tried to load the phone page, it
    // would have found it missing and kept it so.
    put(root + "/ui.html",
        "<!doctype html><html><head><link rel=\"stylesheet\" href=\"/assets/ui/ui.css\"></head><body>"
        "<script src=\"/assets/ui/core.js\"></script>\n<script src=\"/assets/ui/tabs.js\"></script>\n</body></html>");
    put(root + "/ui/ui.css", "body{margin:0}");
    put(root + "/ui/core.js", "var core=1;");
    put(root + "/ui/tabs.js", "var tabs=1;");

    const Answer first = get(assets, "/", true);
    check(first.status == 200 && gzipped(first.body), "the first request for the phone page loads it, compressed");
    const Answer plain = get(assets, "/index.html", false);
    check(plain.status == 200 && !gzipped(plain.body) && has(plain.body, "<style>") && has(plain.body, "body{margin:0}") &&
              has(plain.body, "var core=1;") && has(plain.body, "var tabs=1;") &&
              !has(plain.body, "<script src=\"/assets/ui/"),
          "styles and scripts are inlined, and a client that takes no gzip gets the page plain");
    // The assembled page is reused: changing the parts changes nothing until a restart.
    put(root + "/ui/core.js", "var core=2;");
    check(has(get(assets, "/", false).body, "var core=1;"), "the assembled page is reused");

    // Missing assets are reported, and not looked for again on every request.
    const std::string broken = root + "/broken";
    put(broken + "/ui.html", "<html><head><link rel=\"stylesheet\" href=\"/assets/ui/ui.css\"></head></html>");
    const HttpStaticAssets missing(broken);
    const Answer failed = get(missing, "/", false);
    check(failed.status == 503 && has(failed.body, "ui/ui.css"), "a page whose parts are missing is a 503 naming one");
    put(broken + "/ui/ui.css", "body{}");
    check(get(missing, "/", false).status == 503, "missing assets are not retried for every request");

    runShell(("rm -rf '" + root + "'"));
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    return 0;
}
