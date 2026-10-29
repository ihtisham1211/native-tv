#include "daemon/launcher_sync.hpp"
#include "core/json.hpp"
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <unistd.h>

namespace nativetv {
namespace daemon {
namespace {

// The Homebrew Channel app is listed under the id it had before the project was
// renamed too: an install made then keeps its pages until the new package replaces it.
const char *const kLauncherIds[] = {"com.tvweb.dashboard", "com.tvweb.home",
                                    "io.github.ihtisham1211.nativetv", "io.github.rorygallagher2024.lg-webos-dashboard"};

bool readFile(const std::string &path, std::string &text) {
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in) return false;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    text = buffer.str();
    return true;
}

// Written beside and renamed over, so a page being opened never reads half of it.
bool writeFile(const std::string &path, const std::string &text) {
    const std::string temporary = path + ".tmp";
    {
        std::ofstream out(temporary.c_str(), std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << text;
        if (!out) return false;
    }
    if (std::rename(temporary.c_str(), path.c_str()) != 0) {
        unlink(temporary.c_str());
        return false;
    }
    return true;
}

// The first `var PORT = <digits>;<space>// tvweb:port`, as the server's regular
// expression takes it. Returns the digits' offset and length, or npos.
size_t findPortDigits(const std::string &html, size_t &length) {
    const std::string lead = "var PORT = ";
    const std::string tail = "// tvweb:port";
    for (size_t at = html.find(lead); at != std::string::npos; at = html.find(lead, at + 1)) {
        size_t digits = at + lead.size();
        size_t end = digits;
        while (end < html.size() && std::isdigit(static_cast<unsigned char>(html[end]))) ++end;
        if (end == digits || end >= html.size() || html[end] != ';') continue;
        size_t after = end + 1;
        while (after < html.size() && std::isspace(static_cast<unsigned char>(html[after]))) ++after;
        if (html.compare(after, tail.size(), tail) != 0) continue;
        length = end - digits;
        return digits;
    }
    return std::string::npos;
}

} // namespace

std::vector<std::string> syncLauncherAppinfo(const std::string &root) {
    std::vector<std::string> changed;
    for (const char *id : kLauncherIds) {
        const std::string file = root + "/" + id + "/appinfo.json";
        std::string raw;
        if (!readFile(file, raw)) continue;
        Json info = Json::parse(raw);
        if (!info.isObject()) continue;
        if (info["resolution"].isString() && info["resolution"].string() == "1920x1080") continue;
        info.set("resolution", Json::text("1920x1080"));
        // ponytail: compact, where the server indented; webOS reads either.
        if (writeFile(file, info.dump(false) + "\n")) changed.push_back(id);
    }
    return changed;
}

std::vector<std::string> syncLauncherPort(int port, const std::string &root) {
    syncLauncherAppinfo(root);
    std::vector<std::string> changed;
    if (!(port >= 1 && port <= 65535)) return changed;
    const std::string digits = std::to_string(port);
    for (const char *id : kLauncherIds) {
        const std::string file = root + "/" + id + "/index.html";
        std::string html;
        if (!readFile(file, html)) continue;
        size_t length = 0;
        const size_t at = findPortDigits(html, length);
        if (at == std::string::npos || html.compare(at, length, digits) == 0) continue;
        html.replace(at, length, digits);
        if (writeFile(file, html)) changed.push_back(id);
    }
    return changed;
}

} // namespace daemon
} // namespace nativetv
