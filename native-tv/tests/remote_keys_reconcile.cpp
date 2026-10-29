#include "core/json.hpp"
#include "daemon/child_process.hpp"
#include "daemon/command_policy.hpp"
#include "daemon/event_loop.hpp"
#include "daemon/http_frontdoor.hpp"
#include "daemon/json_file.hpp"
#include "daemon/luna.hpp"
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>

using namespace nativetv;
using namespace nativetv::daemon;

namespace {
void check(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

ChildRunnerOptions fastStarts() {
    ChildRunnerOptions options;
    options.startupGap = std::chrono::milliseconds(0);
    options.regularGap = std::chrono::milliseconds(0);
    options.startupWindow = std::chrono::milliseconds(0);
    return options;
}

std::string temporaryDirectory() {
    char pattern[] = "/tmp/nativetv-remote-keys-reconcile-XXXXXX";
    char *created = mkdtemp(pattern);
    check(created != nullptr, "could not create reconciliation fixture");
    return created;
}

class LoopThread {
  public:
    explicit LoopThread(EventLoop &loop) : loop_(loop), thread_([&loop]() {
        loop.run();
    }) {}
    ~LoopThread() { stop(); }
    void stop() {
        if (!thread_.joinable()) return;
        if (!loop_.post([this]() { loop_.stop(); })) loop_.stop();
        thread_.join();
    }

  private:
    EventLoop &loop_;
    std::thread thread_;
};

void executable(const std::string &path, const std::string &contents) {
    std::ofstream output(path.c_str());
    check(static_cast<bool>(output), "could not write " + path);
    output << contents;
    output.close();
    check(chmod(path.c_str(), 0755) == 0, "could not make executable " + path);
}

Json readJson(const std::string &path) {
    std::ifstream input(path.c_str(), std::ios::binary);
    check(static_cast<bool>(input), "could not read " + path);
    return Json::parse(std::string(std::istreambuf_iterator<char>(input),
                                   std::istreambuf_iterator<char>()));
}

bool wroteMappingTo(const std::string &path, const std::string &appId) {
    std::ifstream input(path.c_str());
    std::string line;
    while (std::getline(input, line)) {
        const Json call = Json::parse(line);
        if (call["uri"].string() !=
                "com.webos.settingsservice/setSystemSettings" ||
            !call["payload"]["settings"]["mapping_info"].isArray())
            continue;
        for (const Json &row :
             call["payload"]["settings"]["mapping_info"].items()) {
            if (row["netflix"]["app_id"].string() == appId)
                return true;
        }
    }
    return false;
}
} // namespace

int main() {
    const std::string root = temporaryDirectory();
    const std::string data = root + "/data";
    const std::string system = root + "/system";
    const std::string proc = root + "/proc";
    const std::string details = root + "/luna-details.jsonl";
    const std::string systemLog = root + "/systemctl.log";
    const std::string mountLog = root + "/mount.log";
    const std::string bootScript = system +
        "/var/lib/webosbrew/init.d/20-tvweb-services";
    check(mkdir(data.c_str(), 0700) == 0, "could not create data fixture");
    check(mkdir(system.c_str(), 0700) == 0, "could not create system fixture");
    check(mkdir(proc.c_str(), 0700) == 0, "could not create proc fixture");
    check(mkdir((system + "/bin").c_str(), 0700) == 0,
          "could not create system binary fixture");
    check(mkdir((system + "/lib").c_str(), 0700) == 0,
          "could not create system library fixture");
    check(mkdir((system + "/lib/systemd").c_str(), 0700) == 0,
          "could not create systemd fixture");
    check(mkdir((system + "/lib/systemd/system").c_str(), 0700) == 0,
          "could not create systemd unit fixture");
    check(mkdir((system + "/usr").c_str(), 0700) == 0,
          "could not create system usr fixture");
    check(mkdir((system + "/usr/sbin").c_str(), 0700) == 0,
          "could not create system service fixture");
    executable(system + "/bin/systemctl",
               "#!/bin/sh\necho \"$*\" >> \"" + systemLog + "\"\nexit 0\n");
    executable(system + "/bin/mount",
               "#!/bin/sh\necho \"mount $*\" >> \"" + mountLog + "\"\nexit 0\n");
    executable(system + "/bin/umount",
               "#!/bin/sh\necho \"umount $*\" >> \"" + mountLog + "\"\nexit 0\n");
    {
        std::ofstream unit((system +
            "/lib/systemd/system/voiceconductor.service").c_str());
        unit << "[Service]\nExecStart=/usr/sbin/voiceconductor\n";
        std::ofstream program((system + "/usr/sbin/voiceconductor").c_str());
        program << "fixture\n";
        check(chmod((system + "/usr/sbin/voiceconductor").c_str(), 0755) == 0,
              "could not make voice service executable");
    }
    {
        std::ofstream disabled((data + "/disabled_services.json").c_str());
        disabled << "[\"camera\"]\n";
    }
    check(setenv("TVWEB_DATA_DIR", data.c_str(), 1) == 0,
          "could not set fixture data root");

    EventLoop loop(4, 32, 64);
    try {
        Json map = Json::object();
        Json keys = Json::object();
        keys.set("netflix", Json::text("org.example.youtube"));
        keys.set("ai", Json::text("org.example.youtube"));
        map.set("keys", keys);
        JsonFile mapStore(data + "/remotekeys.json", 64 * 1024);
        std::string error;
        check(mapStore.save(map, &error), "could not write fixture key map: " + error);

        ChildProcessRunner children(fastStarts());
        LunaClientOptions lunaOptions;
        lunaOptions.executable = NATIVETV_FAKE_LUNA_SEND;
        lunaOptions.environment.push_back("NATIVETV_FAKE_LUNA_MODE=remote-keys-native");
        lunaOptions.environment.push_back("NATIVETV_FAKE_LUNA_DETAILS=" + details);
        LunaClient luna(loop, children, nullptr, lunaOptions);
        CommandPolicyExecutor policy(luna);
        ServerConfig config;
        HttpFrontDoor frontDoor(config, "1.0.0", data, root + "/config.json",
                                HttpProxy(HttpProxyOptions()),
                                HttpProxy(HttpProxyOptions()), luna, children,
                                root + "/assets", proc, system, 5000, nullptr,
                                nullptr, nullptr, &policy);
        LoopThread loopThread(loop);

        bool wrote = false;
        check(frontDoor.reconcileNativeRemoteKeys(wrote, error),
              "initial native key apply failed: " + error);
        check(wrote, "initial native key apply did not write the requested map");
        check(readJson(data + "/remotekeys-lg.json")["mapping"]["netflix"][
                  "app_id"].string() == "netflix",
              "LG's original Netflix mapping was not saved");
        check(wroteMappingTo(details, "org.example.youtube"),
              "requested Netflix mapping was not applied");
        const Json disabledAfterAi = readJson(data + "/disabled_services.json");
        check(disabledAfterAi.size() == 2 &&
                  disabledAfterAi[1].string() == "voiceconductor",
              "AI mapping did not disable voiceconductor while preserving other services");
        {
            std::ifstream unitLog(systemLog.c_str());
            const std::string contents{std::istreambuf_iterator<char>(unitLog),
                                       std::istreambuf_iterator<char>()};
            check(contents.find("stop voiceconductor.service") != std::string::npos,
                  "AI mapping did not stop voiceconductor");
        }

        {
            std::ofstream calls(details.c_str(), std::ios::app);
            calls << "{\"uri\":\"com.webos.settingsservice/setSystemSettings\","
                     "\"payload\":{\"category\":\"other\",\"settings\":{"
                     "\"mapping_info\":[{\"netflix\":{\"launch_param\":null,"
                     "\"app_id\":\"netflix\",\"isActive\":true}},{"
                     "\"amazon\":{\"launch_param\":null,\"app_id\":\"amazon\","
                     "\"isActive\":true}}]}}}\n";
        }
        wrote = false;
        check(frontDoor.reconcileNativeRemoteKeys(wrote, error),
              "LG reset reconciliation failed: " + error);
        check(wrote, "LG reset was not detected and corrected");
        check(wroteMappingTo(details, "org.example.youtube"),
              "the configured mapping was not restored after LG reset");

        Json restoredMap = Json::object();
        restoredMap.set("keys", Json::object());
        check(mapStore.save(restoredMap, &error),
              "could not clear fixture key map: " + error);
        {
            std::ofstream mounts((proc + "/mounts").c_str());
            mounts << "/var/lib/tvweb/service-off " << system
                   << "/usr/sbin/voiceconductor none rw 0 0\n";
        }
        wrote = false;
        check(frontDoor.reconcileNativeRemoteKeys(wrote, error),
              "LG restoration failed: " + error);
        check(wrote, "clearing the Native Tv map did not restore LG settings");
        check(wroteMappingTo(details, "netflix"),
              "LG's original mapping was not restored");
        const Json disabledAfterRestore = readJson(data + "/disabled_services.json");
        check(disabledAfterRestore.size() == 1 &&
                  disabledAfterRestore[0].string() == "camera",
              "restoring the AI key did not preserve the owner's disabled services");
        {
            std::ifstream unitLog(systemLog.c_str());
            const std::string contents{std::istreambuf_iterator<char>(unitLog),
                                       std::istreambuf_iterator<char>()};
            check(contents.find("start voiceconductor.service") != std::string::npos,
                  "restoring the AI key did not start voiceconductor");
        }
        check(access((data + "/remotekeys-lg.json").c_str(), F_OK) != 0,
              "the completed restore left a stale LG ownership record");

        loopThread.stop();
    } catch (const std::exception &exception) {
        std::cerr << exception.what() << '\n';
        loop.stop();
        unlink((data + "/remotekeys.json").c_str());
        unlink((data + "/remotekeys.json.last-good").c_str());
        unlink((data + "/remotekeys-lg.json").c_str());
        unlink((data + "/remotekeys-lg.json.last-good").c_str());
        unlink((data + "/disabled_services.json").c_str());
        unlink((data + "/service-off").c_str());
        unlink((data + "/service-off.js").c_str());
        unlink((data + "/20-tvweb-services").c_str());
        unlink((proc + "/mounts").c_str());
        unlink((system + "/bin/systemctl").c_str());
        unlink((system + "/bin/mount").c_str());
        unlink((system + "/bin/umount").c_str());
        unlink((system + "/lib/systemd/system/voiceconductor.service").c_str());
        unlink((system + "/usr/sbin/voiceconductor").c_str());
        unlink(bootScript.c_str());
        unlink(details.c_str());
        unlink(systemLog.c_str());
        unlink(mountLog.c_str());
        rmdir(data.c_str());
        rmdir((system + "/bin").c_str());
        rmdir((system + "/lib/systemd/system").c_str());
        rmdir((system + "/lib/systemd").c_str());
        rmdir((system + "/lib").c_str());
        rmdir((system + "/usr/sbin").c_str());
        rmdir((system + "/usr").c_str());
        rmdir((system + "/var/lib/webosbrew/init.d").c_str());
        rmdir((system + "/var/lib/webosbrew").c_str());
        rmdir((system + "/var/lib").c_str());
        rmdir((system + "/var").c_str());
        rmdir(system.c_str());
        rmdir(proc.c_str());
        rmdir(root.c_str());
        return 1;
    }

    unlink((data + "/remotekeys.json").c_str());
    unlink((data + "/remotekeys.json.last-good").c_str());
    unlink((data + "/remotekeys-lg.json.last-good").c_str());
    unlink((data + "/remotekeys-lg.json").c_str());
    unlink((data + "/disabled_services.json").c_str());
    unlink((data + "/service-off").c_str());
    unlink((data + "/service-off.js").c_str());
    unlink((data + "/20-tvweb-services").c_str());
    unlink((proc + "/mounts").c_str());
    unlink((system + "/bin/systemctl").c_str());
    unlink((system + "/bin/mount").c_str());
    unlink((system + "/bin/umount").c_str());
    unlink((system + "/lib/systemd/system/voiceconductor.service").c_str());
    unlink((system + "/usr/sbin/voiceconductor").c_str());
    unlink(bootScript.c_str());
    unlink(details.c_str());
    unlink(systemLog.c_str());
    unlink(mountLog.c_str());
    rmdir(data.c_str());
    rmdir((system + "/bin").c_str());
    rmdir((system + "/lib/systemd/system").c_str());
    rmdir((system + "/lib/systemd").c_str());
    rmdir((system + "/lib").c_str());
    rmdir((system + "/usr/sbin").c_str());
    rmdir((system + "/usr").c_str());
    rmdir((system + "/var/lib/webosbrew/init.d").c_str());
    rmdir((system + "/var/lib/webosbrew").c_str());
    rmdir((system + "/var/lib").c_str());
    rmdir((system + "/var").c_str());
    rmdir(system.c_str());
    rmdir(proc.c_str());
    rmdir(root.c_str());
    std::cout << "native remote-key reconciliation passed\n";
    return 0;
}
