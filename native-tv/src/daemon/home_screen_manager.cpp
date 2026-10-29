#include "daemon/home_screen_manager.hpp"
#include "daemon/child_process.hpp"
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <csignal>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <iterator>
#include <regex>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace nativetv {
namespace daemon {
namespace {
const char *const kHomeId = "com.webos.app.home";
const char *const kLayoutMark = "<!-- nativetv -->";
const char *const kTakeoverMark = "<!-- nativetv-home-starting -->";
const char *const kBlankLayout =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<!-- nativetv-home-starting -->\n"
    "<home version=\"2.0\">\n"
    "    <layout type=\"ShelfView\" bgColor=\"#000000\">\n"
    "        <firstPage>\n"
    "        </firstPage>\n"
    "    </layout>\n"
    "</home>\n";
const char *const kWebos10Parts[] = {"sideIcons", "quickCards", "recommended"};
const char *const kWebos9Ids[] = {"banner", "quickCards", "apps", "deviceFunctions"};
const char *const kWebos9Types[] = {"Hero", "Qcard", "AppList", "AdShelf"};

std::string rooted(const std::string &root, const std::string &path) {
    if (root.empty() || root == "/") return path;
    size_t end = root.size();
    while (end > 1 && root[end - 1] == '/') --end;
    return root.substr(0, end) + path;
}

bool readFile(const std::string &path, std::string &contents) {
    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
    if (!input) return false;
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (!input.good() && !input.eof()) return false;
    contents = buffer.str();
    return true;
}

bool ensureDirectory(const std::string &path) {
    if (path.empty() || path == "/") return true;
    struct stat info;
    if (stat(path.c_str(), &info) == 0) return S_ISDIR(info.st_mode);
    const size_t slash = path.find_last_of('/');
    if (slash != std::string::npos && slash > 0 &&
        !ensureDirectory(path.substr(0, slash))) return false;
    if (mkdir(path.c_str(), 0755) == 0) return true;
    return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

bool writeFile(const std::string &path, const std::string &contents) {
    std::ofstream output(path.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    output.close();
    return output.good();
}

bool writeAtomic(const std::string &path, const std::string &contents) {
    const std::string temporary = path + ".tmp";
    if (!writeFile(temporary, contents)) return false;
    if (rename(temporary.c_str(), path.c_str()) == 0) return true;
    (void)unlink(temporary.c_str());
    return false;
}

bool removeTree(const std::string &path) {
    struct stat info;
    if (lstat(path.c_str(), &info) != 0) return errno == ENOENT;
    if (!S_ISDIR(info.st_mode)) return unlink(path.c_str()) == 0;
    DIR *directory = opendir(path.c_str());
    if (!directory) return false;
    bool ok = true;
    struct dirent *entry;
    while ((entry = readdir(directory)) != nullptr) {
        const std::string name(entry->d_name);
        if (name == "." || name == "..") continue;
        if (!removeTree(path + "/" + name)) ok = false;
    }
    closedir(directory);
    return rmdir(path.c_str()) == 0 && ok;
}

std::string webosVersion(const std::string &systemRoot) {
    std::string issue;
    if (!readFile(rooted(systemRoot, "/etc/issue"), issue))
        (void)readFile(rooted(systemRoot, "/etc/issue.net"), issue);
    std::smatch match;
    if (std::regex_search(issue, match,
                          std::regex("webOS(?: TV)?[[:space:]]+([0-9.]+)",
                                     std::regex_constants::icase)))
        return match[1].str();
    std::string starfish;
    if (readFile(rooted(systemRoot, "/etc/starfish-release"), starfish) &&
        std::regex_search(starfish, match,
                          std::regex("release[[:space:]]+([0-9.]+)",
                                     std::regex_constants::icase)))
        return match[1].str();
    return std::string();
}

bool startsWith(const std::string &value, const std::string &prefix) {
    return value.compare(0, prefix.size(), prefix) == 0;
}

Json failure(const std::string &error) {
    Json result = Json::object();
    result.set("ok", Json::flag(false));
    result.set("error", Json::text(error));
    return result;
}

Json success(bool changed) {
    Json result = Json::object();
    result.set("ok", Json::flag(true));
    result.set("changed", Json::flag(changed));
    return result;
}

bool contains(const std::vector<std::string> &items, const std::string &item) {
    return std::find(items.begin(), items.end(), item) != items.end();
}

bool validHomeInputId(const std::string &id) {
    return id == "hdmi1" || id == "hdmi2" || id == "hdmi3" || id == "hdmi4";
}

bool validLaunchPointId(const std::string &id) {
    if (id.empty() || id.size() > 256) return false;
    for (unsigned char character : id)
        if (std::iscntrl(character)) return false;
    return true;
}

std::vector<std::string> webos9ItemTypes(const std::string &xml) {
    std::vector<std::string> types;
    const std::regex item("<item[[:space:]][^>]*type=\"([^\"]+)\"");
    for (std::sregex_iterator it(xml.begin(), xml.end(), item), end; it != end; ++it)
        types.push_back((*it)[1].str());
    return types;
}

ChildResult runTool(ChildProcessRunner &children, const std::string &path,
                    const std::vector<std::string> &arguments,
                    std::chrono::milliseconds timeout = std::chrono::milliseconds(8000)) {
    ChildOptions options;
    options.arguments.push_back(path);
    options.arguments.insert(options.arguments.end(), arguments.begin(), arguments.end());
    options.timeout = timeout;
    options.maximumOutputBytes = 4096;
    return children.run(options);
}

std::vector<std::string> splitWords(const std::string &line) {
    std::istringstream input(line);
    std::vector<std::string> words;
    std::string word;
    while (input >> word) words.push_back(word);
    return words;
}
} // namespace

class HomeScreenState {
  public:
    HomeScreenState(ChildProcessRunner &children, const std::string &systemRoot,
                    const std::string &dataRoot, const std::string &procRoot,
                    bool allowed)
        : children(children), systemRoot(systemRoot), dataRoot(dataRoot),
          procRoot(procRoot), allowExperimental(allowed), version(webosVersion(systemRoot)) {}

    bool isWebos9() const {
        return startsWith(version, "9.");
    }

    bool supportsTakeover(bool allowExperimentalHome) const {
        return startsWith(version, "9.") || startsWith(version, "10.") ||
               (startsWith(version, "11.") && allowExperimentalHome);
    }

    Json state() const {
        Json result = Json::object();
        const bool on9 = supported9();
        const bool on10 = !on9 && supported10();
        result.set("supported", Json::flag(on9 || on10));
        result.set("experimental", on10 ? Json::flag(true) : Json());
        result.set("allowed", Json::flag(!on10 || allowExperimental));
        Json parts = Json::array();
        if (on9) {
            const std::vector<bool> shown = readParts9();
            for (size_t i = 0; i < 4; ++i) {
                Json part = Json::object();
                part.set("id", Json::text(kWebos9Ids[i]));
                part.set("shown", Json::flag(shown[i]));
                parts.add(part);
            }
        } else if (on10) {
            const std::vector<std::string> hidden = readHidden10();
            for (const char *id : kWebos10Parts) {
                Json part = Json::object();
                part.set("id", Json::text(id));
                part.set("shown", Json::flag(!contains(hidden, id)));
                parts.add(part);
            }
        }
        result.set("parts", parts);
        result.set("addedInputs", readAddedInputs());
        return result;
    }

    Json setPart(const std::string &id, bool shown) {
        if (startsWith(version, "9.") && nativeTvHomeOn9())
            return failure("Native Tv Home is in LG's Home's place. Switch it off on the Home tab to change LG's Home screen.");
        if (supported9()) return setPart9(id, shown);
        if (supported10()) return setPart10(id, shown);
        return failure("This TV's Home screen cannot be changed from here");
    }

    Json startLayout(bool blank) const {
        std::string error;
        if (blank) {
            if (!blankStartLayout9(error))
                return failure("could not prepare LG's Home's starting layout");
        } else if (!restoreStartLayout9(error)) {
            return failure("LG's Home layout could not be put back: " + error);
        }
        Json result = success(true);
        result.set("blank", Json::flag(blank));
        return result;
    }

    Json restore() {
        if (startsWith(version, "9.")) {
            bool had = false;
            if (!removeOwnedLayout9(had)) return failure("could not restore LG's Home layout");
            return success(had);
        }
        if (startsWith(version, "10.")) {
            const bool had = mounted10() || !readHidden10().empty();
            std::string error;
            if (!unmount10(&error)) return failure(error);
            if (unlink(statePath10().c_str()) != 0 && errno != ENOENT)
                return failure("could not remove the saved Home layout settings");
            return success(had);
        }
        return success(false);
    }

    Json setAllowed(bool allowed) {
        allowExperimental = allowed;
        if (allowed || !supported10()) return success(false);
        return restore();
    }

    Json reapply() {
        if (!supported10() || !allowExperimental || mounted10()) return success(false);
        const std::vector<std::string> hidden = readHidden10();
        if (hidden.empty()) return success(false);
        std::string error;
        if (!apply10(hidden, error)) return failure(error);
        return success(true);
    }

    bool stopHome(bool &stopped, std::string *error) {
        stopped = false;
        if (!clearHomeCheckpoints(error)) return false;
        const std::vector<pid_t> pids = homePids();
        stopped = !pids.empty();
        for (pid_t pid : pids) {
            if (!isHome(pid)) continue;
            if (kill(pid, SIGTERM) != 0 && errno != ESRCH) {
                if (error) *error = "could not restart the Home screen";
                return false;
            }
        }
        return true;
    }

    bool restartHome(std::string *error) {
        bool stopped = false;
        return stopHome(stopped, error);
    }

    Json setAddedInputs(const Json &inputs) {
        if (!inputs.isObject()) return failure("Home input state must be an object");
        Json clean = Json::object();
        for (const std::string &id : inputs.keys()) {
            const Json launchPoint = inputs[id];
            if (!validHomeInputId(id) || !launchPoint.isString() ||
                !validLaunchPointId(launchPoint.string()))
                return failure("Home input state contains an invalid launch point");
            clean.set(id, launchPoint);
        }
        if (!writeAddedInputs(clean)) return failure("could not save Home input state");
        return success(true);
    }

  private:
    ChildProcessRunner &children;
    std::string systemRoot;
    std::string dataRoot;
    std::string procRoot;
    bool allowExperimental;
    std::string version;

    std::string home9() const {
        return rooted(systemRoot, "/usr/palm/applications/com.webos.app.home/qml");
    }
    std::string layoutDir9() const { return rooted(systemRoot, "/mnt/lg/cmn_data/manifest"); }
    std::string builtIn9() const { return home9() + "/UserInterfaceLayer/home_layoutShelfView.xml"; }
    std::string mainView9() const { return home9() + "/UserInterfaceLayer/Containers/MainView_M.qml"; }
    std::vector<std::string> layoutFiles9() const {
        const std::string directory = layoutDir9();
        return {directory + "/home_layoutShelfView.xml", directory + "/home_layout.xml"};
    }
    std::string keptStartLayout9() const {
        return dataRoot + "/home-takeover-lg-layout";
    }
    std::string assetsDir10() const {
        return rooted(systemRoot,
                      "/usr/palm/applications/com.webos.app.home/data/flutter_assets/assets");
    }
    std::string overlayDir10() const { return rooted(systemRoot, "/tmp/nativetv-home"); }
    std::string mountsFile10() const { return rooted(systemRoot, "/proc/mounts"); }
    std::string filesystemsFile10() const { return rooted(systemRoot, "/proc/filesystems"); }
    std::string statePath10() const { return dataRoot + "/home-webos10.json"; }
    std::string lgCopy10() const { return dataRoot + "/home-webos10-lg.xml"; }
    std::string inputStatePath() const { return dataRoot + "/home-inputs.json"; }
    std::string homeXml10() const { return assetsDir10() + "/home.xml"; }

    Json readAddedInputs() const {
        std::string source;
        if (!readFile(inputStatePath(), source)) return Json::object();
        const Json stored = Json::parse(source);
        if (!stored.isObject()) return Json::object();
        Json result = Json::object();
        for (const std::string &id : stored.keys()) {
            const Json launchPoint = stored[id];
            if (validHomeInputId(id) && launchPoint.isString() &&
                validLaunchPointId(launchPoint.string()))
                result.set(id, launchPoint);
        }
        return result;
    }

    bool writeAddedInputs(const Json &inputs) const {
        if (inputs.keys().empty())
            return unlink(inputStatePath().c_str()) == 0 || errno == ENOENT;
        return ensureDirectory(dataRoot) &&
               writeAtomic(inputStatePath(), inputs.dump() + "\n");
    }

    bool supported9() const {
        if (!startsWith(version, "9.")) return false;
        std::string mainView, builtIn;
        if (!readFile(mainView9(), mainView) || !readFile(builtIn9(), builtIn) ||
            mainView.find("/mnt/lg/cmn_data/manifest/") == std::string::npos)
            return false;
        const std::vector<std::string> types = webos9ItemTypes(builtIn);
        for (const char *type : kWebos9Types)
            if (!contains(types, type)) return false;
        for (const std::string &path : layoutFiles9()) {
            std::string current;
            if (readFile(path, current) && current.find(kLayoutMark) == std::string::npos)
                return false;
        }
        return true;
    }

    bool mounted10() const {
        std::string mounts;
        if (!readFile(mountsFile10(), mounts)) return false;
        std::istringstream input(mounts);
        std::string line;
        while (std::getline(input, line)) {
            const std::vector<std::string> fields = splitWords(line);
            if (fields.size() >= 3 && fields[1] == assetsDir10() && fields[2] == "overlay")
                return true;
        }
        return false;
    }

    std::string lgLayout10() const {
        std::string result;
        return readFile(mounted10() ? lgCopy10() : homeXml10(), result) ? result : std::string();
    }

    bool recognised10(const std::string &xml) const {
        if (xml.empty()) return false;
        const std::vector<std::string> lines = linesOf(xml);
        for (const char *id : {"globalline", "qcardList", "recommendedShelf"}) {
            unsigned count = 0;
            for (const std::string &line : lines)
                if (line.find(std::string("id=\"") + id + "\"") != std::string::npos) ++count;
            if (count != 1) return false;
        }
        return std::regex_search(xml, std::regex("<item id=\"herobanner\"[^>]*/>")) &&
               xml.find("id=\"container\" hasChildren=\"true\"") != std::string::npos;
    }

    bool supported10() const {
        if (!startsWith(version, "10.")) return false;
        std::string filesystems;
        return readFile(filesystemsFile10(), filesystems) &&
               filesystems.find("overlay") != std::string::npos && recognised10(lgLayout10());
    }

    static std::vector<std::string> linesOf(const std::string &text) {
        std::vector<std::string> lines;
        std::istringstream input(text);
        std::string line;
        while (std::getline(input, line)) lines.push_back(line);
        return lines;
    }

    bool nativeTvHomeOn9() const {
        for (const std::string &path : layoutFiles9()) {
            std::string current;
            if (readFile(path, current) && current.find(kTakeoverMark) != std::string::npos)
                return true;
        }
        return false;
    }

    bool blankStartLayout9(std::string &error) const {
        error.clear();
        const std::string kept = keptStartLayout9();
        bool alreadyBlank = false;
        for (const std::string &path : layoutFiles9()) {
            std::string current;
            if (readFile(path, current) &&
                current.find(kTakeoverMark) != std::string::npos)
                alreadyBlank = true;
        }
        if (!alreadyBlank) {
            if (!ensureDirectory(kept)) {
                error = "could not create the saved Home layout directory";
                return false;
            }
            for (const std::string &path : layoutFiles9()) {
                const std::string name = path.substr(path.find_last_of('/') + 1);
                const std::string backup = kept + "/" + name;
                const std::string absent = backup + ".none";
                struct stat info = {};
                if (lstat(backup.c_str(), &info) == 0) {
                    if (!S_ISREG(info.st_mode)) {
                        error = "saved LG Home layout is not a regular file";
                        return false;
                    }
                    continue;
                }
                if (errno != ENOENT) {
                    error = "could not inspect the saved Home layout";
                    return false;
                }
                if (lstat(absent.c_str(), &info) == 0) {
                    if (!S_ISREG(info.st_mode)) {
                        error = "saved missing-layout marker is not a regular file";
                        return false;
                    }
                    continue;
                }
                if (errno != ENOENT) {
                    error = "could not inspect the missing-layout marker";
                    return false;
                }
                if (lstat(path.c_str(), &info) != 0) {
                    if (errno != ENOENT) {
                        error = "could not read LG's Home layout";
                        return false;
                    }
                    if (!writeAtomic(absent, std::string())) {
                        error = "could not save the missing Home layout marker";
                        return false;
                    }
                    continue;
                }
                if (!S_ISREG(info.st_mode)) {
                    error = "LG's Home layout is not a regular file";
                    return false;
                }
                std::string contents;
                if (!readFile(path, contents) || !writeAtomic(backup, contents)) {
                    error = "could not save LG's Home layout";
                    return false;
                }
            }
        }
        if (!ensureDirectory(layoutDir9())) {
            error = "could not create LG's Home layout directory";
            return false;
        }
        for (const std::string &path : layoutFiles9()) {
            if (!writeAtomic(path, kBlankLayout)) {
                error = "could not write LG's Home starting layout";
                return false;
            }
        }
        return true;
    }

    bool restoreStartLayout9(std::string &error) const {
        error.clear();
        const std::string kept = keptStartLayout9();
        struct stat keptInfo = {};
        const bool hasKept = lstat(kept.c_str(), &keptInfo) == 0;
        if (!hasKept && errno != ENOENT) {
            error = "could not inspect the saved Home layout";
            return false;
        }
        bool blankInPlace = false;
        for (const std::string &path : layoutFiles9()) {
            std::string current;
            if (readFile(path, current) && current.find(kTakeoverMark) != std::string::npos)
                blankInPlace = true;
        }
        if (!blankInPlace && !hasKept) return true;
        for (const std::string &path : layoutFiles9()) {
            const std::string name = path.substr(path.find_last_of('/') + 1);
            const std::string backup = kept + "/" + name;
            const std::string absent = backup + ".none";
            struct stat info = {};
            if (lstat(backup.c_str(), &info) == 0) {
                if (!S_ISREG(info.st_mode)) {
                    error = "saved LG Home layout is not a regular file";
                    return false;
                }
                std::string contents;
                if (!readFile(backup, contents) || !ensureDirectory(layoutDir9()) ||
                    !writeAtomic(path, contents)) {
                    error = "could not restore " + name;
                    return false;
                }
            } else if (errno != ENOENT) {
                error = "could not inspect saved " + name;
                return false;
            } else if (lstat(absent.c_str(), &info) == 0) {
                if (!S_ISREG(info.st_mode)) {
                    error = "saved missing-layout marker is not a regular file";
                    return false;
                }
                if (unlink(path.c_str()) != 0 && errno != ENOENT) {
                    error = "could not remove " + name;
                    return false;
                }
            } else if (errno != ENOENT) {
                error = "could not inspect saved " + name;
                return false;
            } else {
                std::string current;
                if (readFile(path, current) &&
                    current.find(kTakeoverMark) != std::string::npos) {
                    error = "no saved layout for " + name;
                    return false;
                }
            }
        }
        (void)rmdir(layoutDir9().c_str());
        for (const std::string &path : layoutFiles9()) {
            const std::string name = path.substr(path.find_last_of('/') + 1);
            (void)unlink((kept + "/" + name).c_str());
            (void)unlink((kept + "/" + name + ".none").c_str());
        }
        (void)rmdir(kept.c_str());
        (void)clearHomeCheckpoints(nullptr);
        return true;
    }

    bool clearHomeCheckpoints(std::string *error) const {
        const std::vector<std::string> checkpoints = {
            rooted(systemRoot, "/tmp/checkpoint"),
            rooted(systemRoot, "/mnt/lg/cmn_data/checkpoint")};
        for (const std::string &checkpoint : checkpoints) {
            if (!removeTree(checkpoint + "/" + kHomeId) ||
                !removeTree(checkpoint + "/hash_check_" + kHomeId)) {
                if (error) *error = "could not clear the Home screen checkpoint";
                return false;
            }
        }
        return true;
    }

    std::vector<bool> readParts9() const {
        std::string ours;
        const bool marked = readFile(layoutFiles9()[0], ours) &&
                            ours.find(kLayoutMark) != std::string::npos;
        const std::vector<std::string> types = marked ? webos9ItemTypes(ours)
                                                       : std::vector<std::string>();
        std::vector<bool> shown;
        for (const char *type : kWebos9Types)
            shown.push_back(!marked || contains(types, type));
        return shown;
    }

    bool removeOwnedLayout9(bool &had) const {
        had = false;
        for (const std::string &path : layoutFiles9()) {
            std::string current;
            if (!readFile(path, current) || current.find(kLayoutMark) == std::string::npos)
                continue;
            had = true;
            if (unlink(path.c_str()) != 0 && errno != ENOENT) return false;
        }
        (void)rmdir(layoutDir9().c_str());
        return true;
    }

    Json setPart9(const std::string &id, bool shown) {
        if (nativeTvHomeOn9())
            return failure("Native Tv Home is in LG's Home's place. Switch it off on the Home tab to change LG's Home screen.");
        size_t index = 0;
        while (index < 4 && id != kWebos9Ids[index]) ++index;
        if (index == 4) return failure("There is no such part of the Home screen");
        std::vector<bool> next = readParts9();
        next[index] = shown;
        if (std::find(next.begin(), next.end(), true) == next.end())
            return failure("At least one part of the Home screen has to stay");
        if (std::find(next.begin(), next.end(), false) == next.end()) {
            bool ignored = false;
            if (!removeOwnedLayout9(ignored)) return failure("could not restore LG's Home layout");
        } else {
            std::string original;
            if (!readFile(builtIn9(), original))
                return failure("Could not read LG's Home screen layout");
            std::vector<std::string> hiddenTypes;
            for (size_t i = 0; i < 4; ++i)
                if (!next[i]) hiddenTypes.push_back(kWebos9Types[i]);
            const std::vector<std::string> lines = linesOf(original);
            std::ostringstream output;
            for (size_t i = 0; i < lines.size(); ++i) {
                const std::vector<std::string> types = webos9ItemTypes(lines[i]);
                if (!types.empty() && contains(hiddenTypes, types[0])) continue;
                if (i) output << '\n';
                output << lines[i];
            }
            std::string layout = output.str();
            const std::regex declaration("^(<\\?xml[^>]*\\?>)");
            std::smatch declarationMatch;
            if (std::regex_search(layout, declarationMatch, declaration)) {
                layout = declarationMatch[1].str() + "\n" + kLayoutMark +
                         layout.substr(declarationMatch.length());
            } else {
                layout = std::string(kLayoutMark) + "\n" + layout;
            }
            if (!ensureDirectory(layoutDir9())) return failure("could not create the Home layout directory");
            for (const std::string &path : layoutFiles9())
                if (!writeAtomic(path, layout)) return failure("could not write the Home layout");
        }
        Json result = success(true);
        result.set("restart", Json::flag(true));
        return result;
    }

    std::vector<std::string> readHidden10() const {
        std::string source;
        if (!readFile(statePath10(), source)) return {};
        const Json state = Json::parse(source);
        const Json hidden = state["hidden"];
        if (!hidden.isArray()) return {};
        std::vector<std::string> result;
        for (const Json &item : hidden.items()) {
            const std::string id = item.string();
            if (item.isString() && std::find(std::begin(kWebos10Parts), std::end(kWebos10Parts), id) !=
                                       std::end(kWebos10Parts) && !contains(result, id))
                result.push_back(id);
        }
        return result;
    }

    bool saveHidden10(const std::vector<std::string> &hidden) const {
        if (hidden.empty()) {
            return unlink(statePath10().c_str()) == 0 || errno == ENOENT;
        }
        if (!ensureDirectory(dataRoot)) return false;
        Json list = Json::array();
        for (const std::string &id : hidden) list.add(Json::text(id));
        Json state = Json::object();
        state.set("hidden", list);
        return writeAtomic(statePath10(), state.dump() + "\n");
    }

    static unsigned itemHeight(const std::string &line) {
        std::smatch match;
        if (!std::regex_search(line, match, std::regex("itemHeight=\"([0-9]+)\""))) return 0;
        return static_cast<unsigned>(std::strtoul(match[1].str().c_str(), nullptr, 10));
    }

    static std::string replaceAttribute(const std::string &line, const std::string &name,
                                        const std::string &value) {
        const std::regex attribute(name + "=\"[0-9]+\"");
        if (!std::regex_search(line, attribute)) return line;
        return std::regex_replace(line, attribute, name + "=\"" + value + "\"",
                                  std::regex_constants::format_first_only);
    }

    std::string buildLayout10(const std::string &lg,
                              const std::vector<std::string> &hidden) const {
        std::vector<std::string> lines = linesOf(lg);
        std::vector<std::string> output;
        std::string sideLine;
        unsigned freed = 0;
        for (const std::string &line : lines) {
            const bool quick = contains(hidden, "quickCards") &&
                               line.find("id=\"qcardList\"") != std::string::npos;
            const bool recommended = contains(hidden, "recommended") &&
                                     line.find("id=\"recommendedShelf\"") != std::string::npos;
            if (quick || recommended) {
                freed += itemHeight(line);
                if (quick && !output.empty() &&
                    output.back().find("id=\"margin\"") != std::string::npos) {
                    freed += itemHeight(output.back());
                    output.pop_back();
                }
                continue;
            }
            if (contains(hidden, "sideIcons") &&
                line.find("id=\"globalline\"") != std::string::npos) {
                sideLine = replaceAttribute(replaceAttribute(line, "itemWidth", "0"),
                                            "itemHeight", "0");
                continue;
            }
            output.push_back(line);
            if (!sideLine.empty() && line.find("<item id=\"herobanner\"") != std::string::npos) {
                output.push_back(sideLine);
                sideLine.clear();
            }
        }
        if (freed) {
            for (std::string &line : output) {
                if (!std::regex_search(line, std::regex("id=\"(container|herobanner|globalline)\"")))
                    continue;
                const unsigned height = itemHeight(line);
                if (height) line = replaceAttribute(line, "itemHeight", std::to_string(height + freed));
            }
        }
        std::ostringstream result;
        for (size_t i = 0; i < output.size(); ++i) {
            if (i) result << '\n';
            result << output[i];
        }
        return result.str();
    }

    bool apply10(const std::vector<std::string> &hidden, std::string &error) {
        if (hidden.empty()) {
            return unmount10(&error);
        }
        const std::string original = lgLayout10();
        if (!recognised10(original)) {
            error = "Could not read LG's Home screen layout";
            return false;
        }
        if (!ensureDirectory(dataRoot) || !writeAtomic(lgCopy10(), original)) {
            error = "could not save LG's Home screen layout";
            return false;
        }
        if (!unmount10(&error)) return false;
        const std::string upper = overlayDir10() + "/upper";
        const std::string work = overlayDir10() + "/work";
        if (!ensureDirectory(upper) || !ensureDirectory(work) ||
            !writeFile(upper + "/home.xml", buildLayout10(original, hidden))) {
            error = "could not stage the Home screen overlay";
            return false;
        }
        const std::string mount = rooted(systemRoot, "/bin/mount");
        const std::string options = "lowerdir=" + assetsDir10() + ",upperdir=" + upper +
                                    ",workdir=" + work;
        const ChildResult mounted = runTool(children, mount,
            {"-t", "overlay", "overlay", "-o", options, assetsDir10()});
        if (!mounted.succeeded()) {
            error = mounted.standardError.empty() ? "could not mount the Home screen overlay"
                                                  : mounted.standardError;
            return false;
        }
        return true;
    }

    bool unmount10(std::string *error = nullptr) {
        if (mounted10()) {
            const std::string umount = rooted(systemRoot, "/bin/umount");
            const ChildResult result = runTool(children, umount, {assetsDir10()});
            if (!result.succeeded() || mounted10()) {
                if (error) *error = result.standardError.empty()
                    ? "could not unmount the Home screen overlay" : result.standardError;
                return false;
            }
        }
        if (!removeTree(overlayDir10())) {
            if (error) *error = "could not remove the Home screen overlay files";
            return false;
        }
        return true;
    }

    Json setPart10(const std::string &id, bool shown) {
        if (!allowExperimental)
            return failure("Switch on Home screen layout first. It is experimental on this TV.");
        if (std::find(std::begin(kWebos10Parts), std::end(kWebos10Parts), id) ==
            std::end(kWebos10Parts))
            return failure("There is no such part of the Home screen");
        std::vector<std::string> hidden = readHidden10();
        hidden.erase(std::remove(hidden.begin(), hidden.end(), id), hidden.end());
        if (!shown) hidden.push_back(id);
        if (!saveHidden10(hidden)) return failure("could not save the Home layout settings");
        std::string error;
        if (!apply10(hidden, error)) return failure(error);
        Json result = success(true);
        result.set("restart", Json::flag(true));
        return result;
    }

    std::vector<pid_t> homePids() const {
        DIR *directory = opendir(procRoot.c_str());
        if (!directory) return {};
        std::vector<pid_t> result;
        struct dirent *entry;
        while ((entry = readdir(directory)) != nullptr) {
            char *end = nullptr;
            const long parsed = std::strtol(entry->d_name, &end, 10);
            if (!end || *end || parsed <= 1) continue;
            const pid_t pid = static_cast<pid_t>(parsed);
            if (isHome(pid)) result.push_back(pid);
        }
        closedir(directory);
        return result;
    }

    bool isHome(pid_t pid) const {
        std::string command;
        if (readFile(procRoot + "/" + std::to_string(pid) + "/cmdline", command)) {
            std::vector<std::string> args;
            size_t start = 0;
            while (start < command.size()) {
                const size_t end = command.find('\0', start);
                args.push_back(command.substr(start, end == std::string::npos
                    ? std::string::npos : end - start));
                if (end == std::string::npos) break;
                start = end + 1;
            }
            if (!args.empty()) {
                const size_t slash = args[0].find_last_of('/');
                const std::string base = slash == std::string::npos
                    ? args[0] : args[0].substr(slash + 1);
                if (base == kHomeId) return true;
                for (size_t i = 1; i + 1 < args.size(); ++i)
                    if (args[i] == "-i" && args[i + 1] == kHomeId) return true;
            }
        }
        std::string name;
        return readFile(procRoot + "/" + std::to_string(pid) + "/comm", name) &&
               name.find(kHomeId) == 0;
    }
};

HomeScreenManager::HomeScreenManager(ChildProcessRunner &children,
                                     std::string systemRoot,
                                     std::string dataRoot,
                                     std::string procRoot,
                                     bool allowExperimental)
    : children_(children), systemRoot_(std::move(systemRoot)),
      dataRoot_(std::move(dataRoot)), procRoot_(std::move(procRoot)),
      allowExperimental_(allowExperimental) {}

bool HomeScreenManager::isWebos9() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const HomeScreenState current(children_, systemRoot_, dataRoot_, procRoot_,
                                  allowExperimental_);
    return current.isWebos9();
}

bool HomeScreenManager::supportsTakeover(bool allowExperimentalHome) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const HomeScreenState current(children_, systemRoot_, dataRoot_, procRoot_,
                                  allowExperimental_);
    return current.supportsTakeover(allowExperimentalHome);
}

Json HomeScreenManager::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const HomeScreenState current(children_, systemRoot_, dataRoot_, procRoot_,
                                  allowExperimental_);
    return current.state();
}

Json HomeScreenManager::setPart(const std::string &id, bool shown) {
    std::lock_guard<std::mutex> lock(mutex_);
    HomeScreenState current(children_, systemRoot_, dataRoot_, procRoot_,
                           allowExperimental_);
    return current.setPart(id, shown);
}

Json HomeScreenManager::startLayout(bool blank) {
    std::lock_guard<std::mutex> lock(mutex_);
    const HomeScreenState current(children_, systemRoot_, dataRoot_, procRoot_,
                                  allowExperimental_);
    return current.startLayout(blank);
}

Json HomeScreenManager::restore() {
    std::lock_guard<std::mutex> lock(mutex_);
    HomeScreenState current(children_, systemRoot_, dataRoot_, procRoot_,
                           allowExperimental_);
    return current.restore();
}

Json HomeScreenManager::setAllowed(bool allowed) {
    std::lock_guard<std::mutex> lock(mutex_);
    allowExperimental_ = allowed;
    HomeScreenState current(children_, systemRoot_, dataRoot_, procRoot_,
                           allowExperimental_);
    return current.setAllowed(allowed);
}

Json HomeScreenManager::reapply() {
    std::lock_guard<std::mutex> lock(mutex_);
    HomeScreenState current(children_, systemRoot_, dataRoot_, procRoot_,
                           allowExperimental_);
    return current.reapply();
}

Json HomeScreenManager::setAddedInputs(const Json &inputs) {
    std::lock_guard<std::mutex> lock(mutex_);
    HomeScreenState current(children_, systemRoot_, dataRoot_, procRoot_,
                            allowExperimental_);
    return current.setAddedInputs(inputs);
}

bool HomeScreenManager::restartHome(std::string *error) {
    std::lock_guard<std::mutex> lock(mutex_);
    HomeScreenState current(children_, systemRoot_, dataRoot_, procRoot_,
                           allowExperimental_);
    return current.restartHome(error);
}

bool HomeScreenManager::stopHome(bool &stopped, std::string *error) {
    std::lock_guard<std::mutex> lock(mutex_);
    HomeScreenState current(children_, systemRoot_, dataRoot_, procRoot_,
                            allowExperimental_);
    return current.stopHome(stopped, error);
}
} // namespace daemon
} // namespace nativetv
