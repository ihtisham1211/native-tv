#include "daemon/json_file.hpp"
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace nativetv;

namespace {
void check(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}

std::string temporaryDirectory() {
    char path[] = "/tmp/nativetv-json-file-XXXXXX";
    char *created = mkdtemp(path);
    if (!created)
        throw std::runtime_error("could not create test directory");
    return created;
}

void removeDirectory(const std::string &path) {
    DIR *directory = opendir(path.c_str());
    if (directory) {
        struct dirent *entry;
        while ((entry = readdir(directory)) != nullptr) {
            std::string name = entry->d_name;
            if (name == "." || name == "..")
                continue;
            std::string child = path + "/" + name;
            struct stat info;
            if (lstat(child.c_str(), &info) == 0 && S_ISDIR(info.st_mode))
                removeDirectory(child);
            else
                unlink(child.c_str());
        }
        closedir(directory);
    }
    rmdir(path.c_str());
}

Json generation(int value) {
    Json result = Json::object();
    result.set("generation", Json::numeric(value));
    return result;
}

std::string readText(const std::string &path) {
    std::ifstream input(path.c_str(), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

void writeText(const std::string &path, const std::string &value) {
    std::ofstream output(path.c_str(), std::ios::binary);
    output << value;
    output.close();
    check(output.good(), "could not write test file " + path);
}

std::string findTemporary(const std::string &directory, const std::string &prefix,
                          size_t minimumSize) {
    DIR *dir = opendir(directory.c_str());
    if (!dir)
        return "";
    std::string found;
    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name.compare(0, prefix.size(), prefix) != 0)
            continue;
        std::string path = directory + "/" + name;
        struct stat info;
        if (stat(path.c_str(), &info) == 0 && info.st_size >= 0 &&
            static_cast<size_t>(info.st_size) >= minimumSize) {
            found = path;
            break;
        }
    }
    closedir(dir);
    return found;
}

int crashSave(const std::string &path, int signalFd) {
    Json value = Json::object();
    value.set("blob", Json::text(std::string(6 * 1024 * 1024, 'x')));
    JsonFile file(path);
    std::string error;
    return file.save(value, &error, [signalFd]() {
        const char marker = 'x';
        if (write(signalFd, &marker, 1) != 1)
            _exit(3);
        pause();
    }) ? 0 : 2;
}

void testBasicSaveAndLastGood(const std::string &root) {
    std::string path = root + "/nested/state.json";
    JsonFile file(path);
    std::string error;
    check(file.save(generation(1), &error), "first save failed: " + error);
    JsonFileResult first = file.load();
    check(first.found && !first.recovered && first.value["generation"].number() == 1,
          "first save did not load as the active value");
    check(file.save(generation(2), &error), "second save failed: " + error);
    Json backup = Json::parse(readText(path + ".last-good"));
    check(backup["generation"].number() == 1, "last-good copy did not preserve old value");
    struct stat info;
    check(stat(path.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600,
          "saved file must be private (0600)");
    check(file.path() == path, "file path accessor changed the configured path");
}

void testQuarantineAndRecovery(const std::string &root) {
    std::string path = root + "/recover.json";
    JsonFile file(path);
    std::string error;
    check(file.save(generation(1), &error), "recovery setup save failed: " + error);
    check(file.save(generation(2), &error), "recovery backup setup failed: " + error);
    writeText(path, "{broken");
    JsonFileResult recovered = file.load();
    check(recovered.found && recovered.recovered && recovered.value["generation"].number() == 1,
          "invalid primary was not recovered from the last-good copy");
    check(Json::parse(readText(path))["generation"].number() == 1,
          "recovered value was not restored as the primary file");
    check(findTemporary(root, "recover.json.corrupt.", 1) != "",
          "invalid primary was not quarantined");
}

void testMissingAndInvalidFiles(const std::string &root) {
    std::string path = root + "/missing.json";
    JsonFile missing(path);
    JsonFileResult absent = missing.load();
    check(!absent.found && absent.error.empty(), "missing state should be a clean empty result");

    writeText(path, "not json");
    JsonFileResult invalid = missing.load();
    check(!invalid.found && !invalid.error.empty(), "invalid state should report no recovery");
    check(findTemporary(root, "missing.json.corrupt.", 1) != "",
          "invalid state without a backup was not quarantined");

    JsonFile small(path, 16);
    std::string error;
    check(!small.save(generation(1000), &error) && !error.empty(),
          "oversize JSON value should not be saved");
}

void testCrashBeforeRename(const std::string &root) {
    std::string path = root + "/crash.json";
    JsonFile file(path);
    std::string error;
    check(file.save(generation(1), &error), "crash test setup save failed: " + error);

    int signalPipe[2];
    check(pipe(signalPipe) == 0, "could not create interruption test pipe");
    // This test process is single-threaded; production daemon code never forks.
    pid_t child = fork();
    check(child >= 0, "fork failed for crash test");
    if (child == 0) {
        close(signalPipe[0]);
        _exit(crashSave(path, signalPipe[1]));
    }
    close(signalPipe[1]);

    int status = 0;
    char marker = 0;
    ssize_t received = read(signalPipe[0], &marker, 1);
    close(signalPipe[0]);
    check(received == 1 && marker == 'x', "child did not reach the pre-rename commit point");
    check(kill(child, SIGKILL) == 0, "could not interrupt save before rename");
    check(waitpid(child, &status, 0) == child && WIFSIGNALED(status),
          "child was not killed at the uncommitted write stage");

    JsonFileResult intact = file.load();
    check(intact.found && !intact.recovered && intact.value["generation"].number() == 1,
          "interrupted write damaged the previously committed state");
}
} // namespace

int main() {
    std::string root;
    try {
        root = temporaryDirectory();
        testBasicSaveAndLastGood(root);
        testQuarantineAndRecovery(root);
        testMissingAndInvalidFiles(root);
        testCrashBeforeRename(root);
        removeDirectory(root);
        std::cout << "JSON file store: atomic saves, permissions, recovery and interruption pass\n";
        return 0;
    } catch (const std::exception &error) {
        if (!root.empty())
            removeDirectory(root);
        std::cerr << "JSON file store: " << error.what() << "\n";
        return 1;
    }
}
