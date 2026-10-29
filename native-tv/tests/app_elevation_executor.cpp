#include "daemon/app_elevation_executor.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {
void check(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

void makeDirectory(const std::string &path) {
    check(mkdir(path.c_str(), 0700) == 0, "could not create " + path);
}

std::string readText(const std::string &path) {
    std::ifstream input(path.c_str());
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}
} // namespace

int main() {
    char folder[] = "/tmp/nativetv-app-elevation-test-XXXXXX";
    char *created = mkdtemp(folder);
    check(created != nullptr, "could not create app elevation test directory");
    const std::string root(created);
    const std::string media = root + "/media";
    const std::string developer = media + "/developer";
    const std::string apps = developer + "/apps";
    const std::string usr = apps + "/usr";
    const std::string palm = usr + "/palm";
    const std::string services = palm + "/services";
    const std::string hbc = services + "/org.webosbrew.hbchannel.service";
    const std::string executable = hbc + "/elevate-service";
    const std::string calls = hbc + "/calls.log";
    makeDirectory(media);
    makeDirectory(developer);
    makeDirectory(apps);
    makeDirectory(usr);
    makeDirectory(palm);
    makeDirectory(services);
    makeDirectory(hbc);
    {
        std::ofstream script(executable.c_str());
        script << "#!/bin/sh\n"
               << "printf '%s\\n' \"$1\" >> '" << calls << "'\n"
               << "if [ \"$1\" = 'com.example.app.two' ]; then\n"
               << "  echo denied >&2\n"
               << "  exit 7\n"
               << "fi\n"
               << "exit 0\n";
        check(script.good(), "could not write fake elevate-service");
    }
    check(chmod(executable.c_str(), 0700) == 0,
          "could not make fake elevate-service executable");

    nativetv::daemon::ChildRunnerOptions runnerOptions;
    runnerOptions.startupGap = std::chrono::milliseconds(0);
    runnerOptions.regularGap = std::chrono::milliseconds(0);
    runnerOptions.startupWindow = std::chrono::milliseconds(0);
    nativetv::daemon::ChildProcessRunner runner(runnerOptions);
    nativetv::daemon::AppElevationExecutor executor(
        runner, root, std::chrono::milliseconds(1000));
    nativetv::daemon::AppElevationResult result;
    std::string error;
    check(executor.run("com.example.app",
                       {"com.example.app.one", "com.example.app.two"},
                       result, &error), "valid services should execute: " + error);
    check(result.done == std::vector<std::string>({"com.example.app.one"}),
          "successful services should be reported in order");
    check(result.failed.size() == 1 &&
              result.failed[0].service == "com.example.app.two" &&
              result.failed[0].error == "denied",
          "a failed service should preserve its first stderr line");
    check(readText(calls) == "com.example.app.one\ncom.example.app.two\n",
          "services should run sequentially as separate argv values");

    check(!executor.run("com.example.app", {"../outside"}, result, &error),
          "invalid service names must be rejected");
    check(!executor.run("com.example.app", {"org.other.service"}, result, &error),
          "a service from another package must be rejected");
    check(readText(calls) == "com.example.app.one\ncom.example.app.two\n",
          "invalid requests must not execute the elevation program");

    check(unlink(executable.c_str()) == 0, "could not remove fake elevate-service");
    check(executor.run("com.example.app", {"com.example.app.one"}, result, &error),
          "an unavailable helper should return per-service failures");
    check(result.done.empty() && result.failed.size() == 1 &&
              result.failed[0].error.find("unavailable") != std::string::npos,
          "a missing helper should not mark any service as elevated");

    unlink(calls.c_str());
    rmdir(hbc.c_str());
    rmdir(services.c_str());
    rmdir(palm.c_str());
    rmdir(usr.c_str());
    rmdir(apps.c_str());
    rmdir(developer.c_str());
    rmdir(media.c_str());
    rmdir(root.c_str());
    std::cout << "native app elevation executor tests passed\n";
    return 0;
}
