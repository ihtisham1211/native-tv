#include "service/privacy_service.hpp"
#include <cctype>
#include <dirent.h>
#include <fstream>
#include <string>
#include <vector>

namespace nativetv {
namespace service {
namespace {
std::string trim(const std::string &text) {
    size_t begin = 0;
    while (begin < text.size() &&
           std::isspace(static_cast<unsigned char>(text[begin])))
        ++begin;
    size_t end = text.size();
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(text[end - 1])))
        --end;
    return text.substr(begin, end - begin);
}

std::string rooted(const std::string &root, const std::string &absolute) {
    if (root.empty() || root == "/") return absolute;
    return root[root.size() - 1] == '/' ? root + absolute.substr(1)
                                        : root + absolute;
}

std::string firstArgument(const std::string &path) {
    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
    std::string value;
    if (input) std::getline(input, value, '\0');
    return value;
}

bool numericName(const std::string &name) {
    if (name.empty()) return false;
    for (unsigned char character : name)
        if (!std::isdigit(character)) return false;
    return true;
}

std::string basename(const std::string &path) {
    const size_t slash = path.find_last_of('/');
    return path.substr(slash == std::string::npos ? 0 : slash + 1);
}

bool processRuns(const std::string &procRoot, const std::string &program) {
    DIR *directory = ::opendir(procRoot.c_str());
    if (!directory) return false;
    const std::string expectedName = basename(program);
    bool found = false;
    struct dirent *entry = nullptr;
    while (!found && (entry = ::readdir(directory)) != nullptr) {
        const std::string pid(entry->d_name);
        if (!numericName(pid)) continue;
        const std::string argument = firstArgument(procRoot + "/" + pid + "/cmdline");
        found = !argument.empty() &&
            (argument == program || basename(argument) == expectedName);
    }
    ::closedir(directory);
    return found;
}

bool serviceFileMatches(const std::string &path, const std::string &serviceName,
                        std::string &program) {
    std::ifstream input(path.c_str());
    if (!input) return false;
    bool dynamic = false;
    bool named = false;
    std::string line;
    while (std::getline(input, line)) {
        const size_t equals = line.find('=');
        if (equals == std::string::npos) continue;
        const std::string key = trim(line.substr(0, equals));
        const std::string value = trim(line.substr(equals + 1));
        if (key == "Type") {
            dynamic = value == "dynamic";
        } else if (key == "Exec") {
            const size_t end = value.find_first_of(" \t");
            program = value.substr(0, end);
        } else if (key == "Name") {
            size_t begin = 0;
            while (begin <= value.size()) {
                const size_t end = value.find(';', begin);
                const std::string name = trim(value.substr(
                    begin, end == std::string::npos ? std::string::npos : end - begin));
                if (name == serviceName) named = true;
                if (end == std::string::npos) break;
                begin = end + 1;
            }
        }
    }
    return dynamic && named && !program.empty();
}
} // namespace

bool privacyServiceWouldStart(const std::string &systemRoot,
                              const std::string &procRoot,
                              const std::string &serviceName) {
    const std::string directoryPath = rooted(
        systemRoot, "/usr/share/dbus-1/system-services");
    DIR *directory = ::opendir(directoryPath.c_str());
    if (!directory) return false;

    std::string program;
    struct dirent *entry = nullptr;
    while (program.empty() && (entry = ::readdir(directory)) != nullptr) {
        const std::string file(entry->d_name);
        if (file.size() < 8 || file.compare(file.size() - 8, 8, ".service") != 0)
            continue;
        std::string candidate;
        if (serviceFileMatches(directoryPath + "/" + file, serviceName, candidate))
            program = candidate;
    }
    ::closedir(directory);
    return !program.empty() && !processRuns(procRoot, program);
}

std::vector<std::string> privacyProcessFirstArguments(const std::string &procRoot) {
    std::vector<std::string> result;
    DIR *directory = ::opendir(procRoot.c_str());
    if (!directory) return result;
    struct dirent *entry = nullptr;
    while ((entry = ::readdir(directory)) != nullptr) {
        const std::string pid(entry->d_name);
        if (!numericName(pid)) continue;
        const std::string argument = firstArgument(procRoot + "/" + pid + "/cmdline");
        if (!argument.empty()) result.push_back(argument);
    }
    ::closedir(directory);
    return result;
}

} // namespace service
} // namespace nativetv
