#include "daemon/app_elevation_store.hpp"
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {
void check(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}
}

int main() {
    char folder[] = "/tmp/nativetv-elevated-test-XXXXXX";
    char *root = mkdtemp(folder);
    check(root != nullptr, "could not create elevated services test directory");
    const std::string path = std::string(root) + "/elevated.json";
    nativetv::Json services;
    std::string error;
    check(nativetv::daemon::readElevatedServices(
              path, "org.example.app", services, &error) &&
              services.isArray() && services.size() == 0,
          "missing elevation state should read as an empty list");

    nativetv::Json selected = nativetv::Json::array();
    selected.add(nativetv::Json::text("org.example.app.service"));
    selected.add(nativetv::Json::text("org.example.worker"));
    check(nativetv::daemon::recordElevatedServices(
              path, "org.example.app", selected, &error),
          "valid elevation state should persist: " + error);
    check(nativetv::daemon::readElevatedServices(
              path, "org.example.app", services, &error) &&
              services.dump() == selected.dump(),
          "elevation state should round trip");

    nativetv::Json other = nativetv::Json::array();
    other.add(nativetv::Json::text("org.example.other.service"));
    check(nativetv::daemon::recordElevatedServices(
              path, "org.example.other", other, &error),
          "another package's elevation state should persist");
    check(nativetv::daemon::readElevatedServices(
              path, "org.example.app", services, &error) &&
              services.dump() == selected.dump(),
          "updating one package must preserve the other package state");

    nativetv::Json invalid = nativetv::Json::array();
    invalid.add(nativetv::Json::text("../outside"));
    check(!nativetv::daemon::recordElevatedServices(
              path, "org.example.app", invalid, &error),
          "invalid service names should be rejected");
    check(!nativetv::daemon::readElevatedServices(
              path, "../outside", services, &error),
          "invalid app ids should be rejected");
    unlink((path + ".last-good").c_str());
    unlink(path.c_str());
    rmdir(root);
    std::cout << "native app elevation store tests passed\n";
    return 0;
}
