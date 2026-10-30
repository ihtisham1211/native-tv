#include "daemon/config.hpp"
#include "daemon/json_file.hpp"
#include "daemon/mqtt_setup.hpp"
#include <chrono>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

using namespace nativetv;
using namespace nativetv::daemon;

namespace {
void check(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}

std::string temporaryDirectory() {
    char path[] = "/tmp/nativetv-config-XXXXXX";
    char *created = mkdtemp(path);
    if (!created)
        throw std::runtime_error("could not create test directory");
    return created;
}

void writeFile(const std::string &path, const std::string &contents, mode_t mode = 0600) {
    std::ofstream output(path.c_str(), std::ios::binary);
    output << contents;
    output.close();
    check(output.good(), "could not write test config " + path);
    check(chmod(path.c_str(), mode) == 0, "could not set test config mode");
}

void removeDirectory(const std::string &path) {
    DIR *directory = opendir(path.c_str());
    if (directory) {
        struct dirent *entry;
        while ((entry = readdir(directory)) != nullptr) {
            std::string name = entry->d_name;
            if (name == "." || name == "..")
                continue;
            unlink((path + "/" + name).c_str());
        }
        closedir(directory);
    }
    rmdir(path.c_str());
}

ConfigLoadOptions optionsFor(const std::string &root) {
    ConfigLoadOptions options;
    options.primaryPath = root + "/primary.json";
    options.fallbackPath = root + "/config.json";
    return options;
}

void testDefaultsAndFirstSavePath(const std::string &root) {
    ConfigLoadResult result = ConfigLoader::load(optionsFor(root));
    check(!result.loaded, "missing config should leave defaults active");
    check(result.path == root + "/primary.json", "fresh install should write to primary path");
    check(result.config.port == 8080 && result.config.host == "::" && result.config.token.empty() &&
              result.config.allowControl && result.config.allowPower && !result.config.sideload,
          "default file-only config values changed");
}

void testPrimaryPrecedenceAndMode(const std::string &root) {
    ConfigLoadOptions options = optionsFor(root);
    writeFile(options.primaryPath,
              "{\"port\":9090,\"host\":\"127.0.0.1\",\"token\":\"secret\","
              "\"allowControl\":false,\"allowPower\":false,"
              "\"apps\":{\"sideload\":true,\"custom\":7},\"extra\":\"retained\"}",
              0644);
    writeFile(options.fallbackPath, "{\"port\":8081}");

    ConfigLoadResult result = ConfigLoader::load(options);
    struct stat info;
    check(result.loaded && result.path == options.primaryPath,
          "primary config should win over fallback");
    check(result.config.port == 9090 && result.config.host == "127.0.0.1" &&
              result.config.token == "secret" && !result.config.allowControl &&
              !result.config.allowPower && result.config.sideload,
          "file-only config values were not read");
    check(result.config.document["extra"].string() == "retained" &&
              result.config.document["apps"]["custom"].number() == 7,
          "unknown config fields should remain available for round-trip callers");
    check(stat(options.primaryPath.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600,
          "loaded config should be tightened to mode 0600");
}

void testMalformedPrimaryFallsBack(const std::string &root) {
    ConfigLoadOptions options = optionsFor(root);
    writeFile(options.primaryPath, "{invalid json");
    writeFile(options.fallbackPath, "{\"port\":8081,\"apps\":{\"sideload\":true}}", 0644);
    ConfigLoadResult result = ConfigLoader::load(options);
    struct stat info;
    check(result.loaded && result.path == options.fallbackPath && result.config.port == 8081 &&
              result.config.sideload,
          "malformed primary should allow the fallback config to load");
    check(stat(options.fallbackPath.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600,
          "fallback config should be tightened to mode 0600");
}

void testOverrideDoesNotUseFallback(const std::string &root) {
    ConfigLoadOptions options = optionsFor(root);
    options.overridePath = root + "/override.json";
    writeFile(options.fallbackPath, "{\"port\":8081}");
    ConfigLoadResult missing = ConfigLoader::load(options);
    check(!missing.loaded && missing.path == options.overridePath && missing.config.port == 8080,
          "explicit config path should suppress fallback lookup");

    writeFile(options.overridePath, "{\"port\":70000,\"allowControl\":\"false\"}");
    ConfigLoadResult invalidValues = ConfigLoader::load(options);
    check(invalidValues.loaded && invalidValues.path == options.overridePath &&
              invalidValues.config.port == 8080 && invalidValues.config.allowControl,
          "invalid typed values should leave safe defaults");
    check(invalidValues.warnings.size() >= 2,
          "invalid typed values should produce actionable warnings");
}

void testDataDirectorySeam(const std::string &root) {
    const std::string directory = root + "/native-data";
    const char *previous = std::getenv("TVWEB_DATA_DIR");
    const bool hadPrevious = previous != nullptr;
    const std::string oldValue = previous ? previous : "";
    check(setenv("TVWEB_DATA_DIR", directory.c_str(), 1) == 0,
          "could not set data directory test seam");
    ConfigLoadOptions options;
    int restoreResult =
        hadPrevious ? setenv("TVWEB_DATA_DIR", oldValue.c_str(), 1) : unsetenv("TVWEB_DATA_DIR");
    check(restoreResult == 0, "could not restore data directory test seam");
    check(options.primaryPath == directory + "/config.json",
          "TVWEB_DATA_DIR should relocate the native config path");
    check(options.fallbackPath == "./config.json",
          "relocating the data directory should preserve the local fallback path");
}

void testNetworkConfigWritesPreserveDocument(const std::string &root) {
    const std::string path = root + "/network.json";
    writeFile(path,
              "{\"port\":8089,\"token\":\"kept\","
              "\"mqtt\":{\"host\":\"broker.local\",\"password\":\"secret\"},"
              "\"custom\":{\"owner\":\"user\"}}",
              0644);
    ConfigStore store(path);
    std::string error;
    check(store.setNetworkOpen(false, &error), "closing network access failed: " + error);

    JsonFile file(path);
    JsonFileResult closed = file.load();
    check(closed.found && closed.value["host"].string() == "127.0.0.1" &&
              closed.value["port"].number() == 8089 && closed.value["token"].string() == "kept" &&
              closed.value["mqtt"]["password"].string() == "secret" &&
              closed.value["custom"]["owner"].string() == "user",
          "network config write did not preserve existing and unknown settings");
    struct stat info;
    check(stat(path.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600,
          "network config write did not keep config permissions private");

    check(store.setNetworkOpen(true, &error), "opening network access failed: " + error);
    JsonFileResult opened = file.load();
    check(opened.found && opened.value["host"].string() == "::" &&
              opened.value["mqtt"]["host"].string() == "broker.local",
          "opening network access did not select dual-stack while preserving config");
}

void testPhoneMqttSettingsAndAtomicSave(const std::string &root) {
    Json config = Json::parse(
        "{\"mqtt\":{\"tlsRejectUnauthorized\":false,\"topicPrefix\":\"saved_tv\","
        "\"discoveryPrefix\":\"ha\",\"telemetryIntervalMs\":2500,"
        "\"password\":\"old-secret\",\"unknown\":17,"
        "\"entities\":{\"controls\":false,\"oled\":\"off\","
        "\"disabled\":[\"valid_id\",\"bad id\"]}},"
        "\"device\":{\"id\":\"lg_old\",\"name\":\"Old TV\",\"custom\":true},"
        "\"custom\":{\"owner\":\"user\"}}");
    Json payload = Json::parse(
        "{\"host\":\" broker.local \",\"port\":\"1883\",\"tls\":true,"
        "\"username\":\" user \"}");
    Json mqtt;
    Json device;
    std::string error;
    check(buildPhoneMqttSettings(payload, config, Json(), mqtt, device, error),
          "valid phone MQTT settings were rejected: " + error);
    check(mqtt["enabled"].boolean() && mqtt["host"].string() == "broker.local" &&
              mqtt["port"].number() == 1883 && mqtt["tls"].boolean() &&
              !mqtt["tlsRejectUnauthorized"].boolean() && mqtt["username"].string() == "user" &&
              !mqtt.has("password") && mqtt["topicPrefix"].string() == "saved_tv" &&
              mqtt["discoveryPrefix"].string() == "ha" &&
              mqtt["telemetryIntervalMs"].number() == 2500,
          "phone MQTT fields did not match the legacy normalized values");
    check(!mqtt["entities"]["controls"].boolean() && mqtt["entities"]["oled"].boolean() &&
              mqtt["entities"]["video"].boolean() &&
              mqtt["entities"]["disabled"].size() == 1 &&
              mqtt["entities"]["disabled"][static_cast<size_t>(0)].string() == "valid_id" &&
              device["id"].string() == "lg_old" && device["name"].string() == "Old TV",
          "phone setup entity defaults, disabled IDs or device identity changed");

    const std::string path = root + "/mqtt.json";
    writeFile(path,
              "{\"token\":\"kept\",\"mqtt\":{\"password\":\"previous\","
              "\"privateExtension\":\"keep\"},\"device\":{\"custom\":7},"
              "\"custom\":{\"owner\":\"user\"}}",
              0644);
    ConfigStore store(path);
    check(store.setMqttSetup(mqtt, device, &error),
          "atomic MQTT setup config write failed: " + error);
    JsonFileResult saved = JsonFile(path).load();
    check(saved.found && saved.value["token"].string() == "kept" &&
              saved.value["custom"]["owner"].string() == "user" &&
              saved.value["mqtt"]["privateExtension"].string() == "keep" &&
              saved.value["mqtt"]["password"].string() == "previous" &&
              saved.value["device"]["custom"].number() == 7,
          "MQTT setup write did not merge the validated fields into existing config");
    struct stat info;
    check(stat(path.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600,
          "MQTT setup config write did not keep config permissions private");

    Json clearPayload = Json::parse(
        "{\"host\":\"broker.local\",\"port\":1883,\"password\":\"\"}");
    check(buildPhoneMqttSettings(clearPayload, config, Json(), mqtt, device, error) &&
              mqtt.has("password") && mqtt["password"].string().empty(),
          "an explicitly empty phone password should clear the stored credential");

    Json fresh = Json::parse("{\"mqtt\":{},\"device\":{}}");
    Json identity = Json::parse("{\"id\":\"lg_c2_abcd\",\"prefix\":\"lgtv_c2_abcd\"}");
    Json freshPayload = Json::parse("{\"host\":\"broker\",\"port\":1883}");
    check(buildPhoneMqttSettings(freshPayload, fresh, identity, mqtt, device, error) &&
              device["id"].string() == "lg_c2_abcd" &&
              mqtt["topicPrefix"].string() == "lgtv_c2_abcd",
          "phone setup did not apply a newly generated TV identity: " + error);

    Json invalid = Json::parse("{\"host\":\"  \",\"port\":70000}");
    check(!buildPhoneMqttSettings(invalid, Json::parse("{\"device\":{}}"), Json(),
                                  mqtt, device, error) &&
              error.find("broker address is required") != std::string::npos &&
              error.find("port must be between 1 and 65535") != std::string::npos &&
              error.find("device id must") != std::string::npos,
          "invalid phone MQTT settings did not report all validation failures");

    // The address typed on the phone is a host name or address, not a URL.
    Json scheme = Json::parse("{\"host\":\"mqtt://broker.local\",\"port\":1883}");
    check(!buildPhoneMqttSettings(scheme, config, Json(), mqtt, device, error) &&
              error.find("no mqtt://") != std::string::npos,
          "a phone MQTT address with a scheme was accepted: " + error);
}

void testObjectSectionPatchPreservesConfig(const std::string &root) {
    const std::string path = root + "/screen-time-config.json";
    writeFile(path,
              "{\"token\":\"kept\",\"screenTime\":{"
              "\"enabled\":true,\"dailyMinutes\":30,\"customPolicy\":\"kept\"},"
              "\"mqtt\":{\"host\":\"broker.local\"},\"custom\":7}",
              0644);
    ConfigStore store(path);
    Json patch = Json::parse("{\"dailyMinutes\":45,\"bedtimeStart\":\"22:00\"}");
    Json savedSection;
    std::string error;
    check(store.patchObjectSection("screenTime", patch, &savedSection, &error),
          "native screen-time config patch failed: " + error);
    check(savedSection["enabled"].boolean() && savedSection["dailyMinutes"].number() == 45 &&
              savedSection["bedtimeStart"].string() == "22:00" &&
              savedSection["customPolicy"].string() == "kept",
          "native screen-time patch did not merge existing section values");
    JsonFileResult saved = JsonFile(path).load();
    check(saved.found && saved.value["token"].string() == "kept" &&
              saved.value["mqtt"]["host"].string() == "broker.local" &&
              saved.value["custom"].number() == 7 &&
              saved.value["screenTime"]["dailyMinutes"].number() == 45,
          "native screen-time patch did not preserve unrelated config values");
    struct stat info;
    check(stat(path.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600,
          "native screen-time patch did not keep config permissions private");

    const std::string lockPath = path + ".lock";
    check(mkdir(lockPath.c_str(), 0700) == 0,
          "could not create stale config lock fixture");
    writeFile(lockPath + "/owner", "2147483647:stale", 0600);
    Json staleRecoveryPatch = Json::parse("{\"action\":\"warn\"}");
    check(store.patchObjectSection("screenTime", staleRecoveryPatch, &savedSection, &error),
          "native config store did not recover a dead writer lock: " + error);
    check(access(lockPath.c_str(), F_OK) != 0 &&
              savedSection["action"].string() == "warn",
          "native config store left a stale lock or lost the recovered update");
    check(!store.patchObjectSection("screenTime", Json::array(), nullptr, &error) &&
              !error.empty(),
          "native config store accepted a non-object section patch");

    // A writer that holds the lock (this process is a live owner) is waited for, and the
    // update made after it is merged onto what that writer saved (test-routes.js).
    check(mkdir(lockPath.c_str(), 0700) == 0, "could not create live config lock fixture");
    writeFile(lockPath + "/owner", std::to_string(getpid()) + ":test", 0600);
    std::thread holder([&path, &lockPath]() {
        usleep(80000);
        writeFile(path, "{\"token\":\"kept\",\"custom\":7,\"screenTime\":{\"enabled\":true,"
                        "\"dailyMinutes\":15,\"action\":\"warn\",\"customPolicy\":\"kept\","
                        "\"bedtimeStart\":\"22:00\"},"
                        "\"mqtt\":{\"host\":\"broker.local\",\"port\":1883}}", 0600);
        unlink((lockPath + "/owner").c_str());
        rmdir(lockPath.c_str());
    });
    const auto waitStart = std::chrono::steady_clock::now();
    Json livePatch = Json::parse("{\"action\":\"act\"}");
    Json liveSection;
    const bool liveWritten = store.patchObjectSection("screenTime", livePatch, &liveSection, &error);
    const auto waited = std::chrono::steady_clock::now() - waitStart;
    holder.join();
    check(liveWritten, "native config store failed behind a live writer: " + error);
    check(waited >= std::chrono::milliseconds(60),
          "native config store did not wait for a live config lock");
    saved = JsonFile(path).load();
    check(saved.value["mqtt"]["port"].number() == 1883 &&
              saved.value["screenTime"]["dailyMinutes"].number() == 15 &&
              saved.value["screenTime"]["action"].string() == "act",
          "native config store lost the update made while it waited");

    const Json backupPatch = Json::parse(
        "{\"logging\":{\"level\":\"debug\"},"
        "\"screenTime\":{\"dailyMinutes\":60}} ");
    Json changedBackupFields = Json::array();
    check(store.patchBackupSettings(backupPatch, &error, &changedBackupFields),
          "native backup config patch failed: " + error);
    check(changedBackupFields.size() == 2 &&
              changedBackupFields[0].string() == "logging.level" &&
              changedBackupFields[1].string() == "screenTime.dailyMinutes",
          "native backup config patch did not report its changed fields in order");
    saved = JsonFile(path).load();
    check(saved.found && saved.value["logging"]["level"].string() == "debug" &&
              saved.value["screenTime"]["dailyMinutes"].number() == 60 &&
              saved.value["screenTime"]["customPolicy"].string() == "kept" &&
              saved.value["token"].string() == "kept" &&
              saved.value["custom"].number() == 7,
          "native backup config patch changed unrelated settings or lost a section");
    Json unchangedBackupFields = Json::array();
    check(store.patchBackupSettings(backupPatch, &error, &unchangedBackupFields) &&
              unchangedBackupFields.size() == 0,
          "native backup config patch reported already-matching fields as changed");
    // A setting the file leaves out stands at the server's default: restoring the
    // default changes nothing (a restore would otherwise always restart the server).
    {
        const std::string defaultsPath = path + ".defaults";
        {
            std::ofstream out(defaultsPath);
            out << "{\"token\":\"kept\"}";
        }
        ConfigStore defaultsStore(defaultsPath);
        Json defaultsChanged = Json::array();
        check(defaultsStore.patchBackupSettings(Json::parse(
                  "{\"logging\":{\"level\":\"info\"},\"syslog\":{\"port\":514,"
                  "\"redact\":true,\"sources\":[\"system\",\"nativetv\"]},"
                  "\"history\":{\"enabled\":false},"
                  "\"screenTime\":{\"dailyMinutes\":0,\"action\":\"off\",\"apps\":{}}}"),
                  &error, &defaultsChanged) && defaultsChanged.size() == 0,
              "restoring settings that equal the defaults was reported as a change");
        check(defaultsStore.patchBackupSettings(Json::parse(
                  "{\"syslog\":{\"port\":515}}"), &error, &defaultsChanged) &&
                  defaultsChanged.size() == 1,
              "a setting that differs from its default was not reported as a change");
        unlink(defaultsPath.c_str());
    }
    check(!store.patchBackupSettings(Json::parse(
              "{\"mqtt\":{\"password\":\"restore-me\"}}"), &error),
          "native backup config patch accepted a private section");
    check(!store.patchBackupSettings(Json::parse(
              "{\"logging\":false}"), &error),
          "native backup config patch accepted a non-object subsection");
    check(stat(path.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600,
          "native backup config patch did not keep config permissions private");
}

void testNetworkDebuggerWritePreservesConfig(const std::string &root) {
    const std::string path = root + "/devtools-config.json";
    writeFile(path,
              "{\"host\":\"127.0.0.1\",\"port\":8090,\"token\":\"keep\","
              "\"allowControl\":false,\"allowPower\":false,"
              "\"mqtt\":{\"password\":\"secret\"},\"custom\":{\"id\":7}}",
              0644);
    ConfigStore store(path);
    std::string error;
    check(store.setNetworkDebuggerAllowed(true, &error),
          "native DevTools config write failed: " + error);
    JsonFileResult saved = JsonFile(path).load();
    check(saved.found && saved.value["allowNetworkDebugger"].boolean() &&
              saved.value["host"].string() == "127.0.0.1" &&
              saved.value["port"].number() == 8090 &&
              saved.value["token"].string() == "keep" &&
              !saved.value["allowControl"].boolean() &&
              !saved.value["allowPower"].boolean() &&
              saved.value["mqtt"]["password"].string() == "secret" &&
              saved.value["custom"]["id"].number() == 7,
          "DevTools config write changed unrelated or file-only settings");
    struct stat info;
    check(stat(path.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600,
          "DevTools config write did not keep config permissions private");
    check(store.setNetworkDebuggerAllowed(false, &error),
          "native DevTools config reset failed: " + error);
    saved = JsonFile(path).load();
    check(saved.found && !saved.value["allowNetworkDebugger"].boolean() &&
              saved.value["mqtt"]["password"].string() == "secret",
          "DevTools config reset did not preserve other config fields");
}

void testJsonParserFuzz(const std::string &root) {
    check(!Json::parse("{\"port\":8080} trailing").valid(),
          "JSON parser accepted trailing non-whitespace bytes");
    check(!Json::parse(std::string("{\"port\":8080}\0x", 15)).valid(),
          "JSON parser accepted embedded NUL/trailing bytes");
    check(Json::parse("{ \"port\": 8080 }\n\t").valid(),
          "JSON parser rejected legal trailing whitespace");

    uint32_t state = 0x4f1bbcdcU;
    for (size_t sample = 0; sample < 2048; ++sample) {
        state = state * 1664525U + 1013904223U;
        const size_t length = state % 257;
        std::string bytes(length, '\0');
        for (size_t i = 0; i < length; ++i) {
            state = state * 1664525U + 1013904223U;
            bytes[i] = static_cast<char>(state >> 24);
        }
        Json parsed = Json::parse(bytes);
        if (parsed.valid())
            check(Json::parse(parsed.dump()).valid(),
                  "valid random JSON did not survive a dump/parse round trip");
    }
    check(!Json::parse(std::string(8 * 1024 * 1024 + 1, 'x')).valid(),
          "JSON parser accepted input above its size limit");

    ConfigLoadOptions options = optionsFor(root);
    const char *samples[] = {"{}",
                             "[]",
                             "null",
                             "{\"port\":true}",
                             "{\"port\":0}",
                             "{\"port\":65536}",
                             "{\"host\":17}",
                             "{\"apps\":[]}",
                             "{\"apps\":{\"sideload\":\"yes\"}}",
                             "{\"allowControl\":false}",
                             "{\"token\":null}",
                             "{invalid",
                             "{\"port\":8080} trailing"};
    for (const char *sample : samples) {
        writeFile(options.primaryPath, sample);
        ConfigLoadResult result = ConfigLoader::load(options);
        check(result.config.port >= 1 && result.config.port <= 65535,
              "config parser emitted an out-of-range port");
    }
}
} // namespace

int main() {
    try {
        std::string root = temporaryDirectory();
        testDefaultsAndFirstSavePath(root);
        testPrimaryPrecedenceAndMode(root);
        testMalformedPrimaryFallsBack(root);
        testOverrideDoesNotUseFallback(root);
        testDataDirectorySeam(root);
        testNetworkConfigWritesPreserveDocument(root);
        testPhoneMqttSettingsAndAtomicSave(root);
        testObjectSectionPatchPreservesConfig(root);
        testNetworkDebuggerWritePreservesConfig(root);
        testJsonParserFuzz(root);
        removeDirectory(root);
        return 0;
    } catch (const std::exception &error) {
        fprintf(stderr, "config test failed: %s\n", error.what());
        return 1;
    }
}
