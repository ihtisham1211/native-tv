#include "service/background_services.hpp"
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace nativetv;

namespace {
std::string join(const std::string &root, const std::string &absolute) {
    return root + absolute;
}

void makeDirectory(const std::string &path) {
    assert(mkdir(path.c_str(), 0755) == 0);
}

void makeParents(const std::string &root, const std::string &absolute) {
    size_t start = 1;
    while (start < absolute.size()) {
        const size_t end = absolute.find('/', start);
        if (end == std::string::npos) break;
        const std::string current = join(root, absolute.substr(0, end));
        struct stat info = {};
        if (stat(current.c_str(), &info) != 0) makeDirectory(current);
        start = end + 1;
    }
}

void write(const std::string &path, const std::string &contents, mode_t mode = 0644) {
    std::ofstream output(path.c_str(), std::ios::binary);
    assert(output.good());
    output << contents;
    output.close();
    assert(chmod(path.c_str(), mode) == 0);
}

void removeTree(const std::string &root) {
    const std::string command = "rm -rf " + root;
    assert(std::system(command.c_str()) == 0);
}
} // namespace

int main() {
    char directory[] = "/tmp/nativetv-services-XXXXXX";
    char *created = mkdtemp(directory);
    assert(created);
    const std::string root(created);
    const std::string systemRoot = root + "/system";
    const std::string dataRoot = root + "/data";
    const std::string procRoot = root + "/proc";
    makeDirectory(systemRoot);
    makeDirectory(dataRoot);
    makeDirectory(procRoot);

    makeParents(systemRoot, "/bin/systemctl");
    write(join(systemRoot, "/bin/systemctl"), "#!/bin/sh\nexit 0\n", 0755);
    makeParents(systemRoot, "/sbin/initctl");
    write(join(systemRoot, "/sbin/initctl"), "#!/bin/sh\nexit 0\n", 0755);
    makeParents(systemRoot, "/lib/systemd/system/com.webos.service.camera.service");
    write(join(systemRoot, "/lib/systemd/system/com.webos.service.camera.service"),
          "[Unit]\nExecStart=/usr/sbin/camera --service\n");
    makeParents(systemRoot, "/usr/sbin/camera");
    write(join(systemRoot, "/usr/sbin/camera"), "program\n", 0755);
    makeParents(systemRoot, "/etc/init/uploadd.conf");
    write(join(systemRoot, "/etc/init/uploadd.conf"), "description uploadd\n");
    makeParents(systemRoot, "/usr/sbin/admanager");
    write(join(systemRoot, "/usr/sbin/admanager"), "program\n", 0755);
    makeParents(systemRoot, "/usr/lib/systemd/system/voiceconductor.service");
    write(join(systemRoot, "/usr/lib/systemd/system/voiceconductor.service"), "[Unit]\n");

    write(dataRoot + "/disabled_services.json",
          "[\"camera\",\"admanager\",\"alwaysready\"]\n");
    makeDirectory(procRoot + "/55");
    assert(symlink(join(systemRoot, "/usr/sbin/admanager").c_str(),
                   (procRoot + "/55/exe").c_str()) == 0);

    assert(service::backgroundServicesSupported(systemRoot));
    assert(service::backgroundSystemctlExecutable(systemRoot) ==
           join(systemRoot, "/bin/systemctl"));
    const std::vector<service::BackgroundServiceProbe> probes =
        service::backgroundServiceProbes(systemRoot);
    if (probes.size() != 3) {
        std::cerr << "probe count=" << probes.size() << "\n";
        for (const service::BackgroundServiceProbe &probe : probes)
            std::cerr << probe.id << " unit=" << probe.unit << " job=" << probe.upstart << "\n";
    }
    assert(probes.size() == 3);
    assert(probes[0].id == "camera" && probes[0].unit == "com.webos.service.camera.service");
    assert(probes[1].id == "uploadd" && probes[1].unit == "uploadd.service");
    assert(probes[2].id == "admanager" && probes[2].unit.empty());

    const std::map<std::string, bool> managerRunning = {
        {"camera", true}, {"uploadd", false}
    };
    Json response = service::backgroundServicesStatus(
        systemRoot, procRoot, dataRoot, managerRunning);
    assert(response["ok"].boolean());
    assert(response["supported"].boolean());
    const Json rows = response["services"];
    assert(rows.size() == 3);
    assert(rows[0]["id"].string() == "camera");
    assert(rows[0]["disabled"].boolean());
    assert(rows[0]["running"].boolean());
    assert(rows[0]["unit"].string() == "com.webos.service.camera.service");
    assert(rows[1]["id"].string() == "uploadd");
    assert(!rows[1]["disabled"].boolean());
    assert(!rows[1]["running"].boolean());
    assert(rows[2]["id"].string() == "admanager");
    assert(rows[2]["running"].boolean());
    assert(rows[2]["disabled"].boolean());
    assert(rows[2]["unit"].valid() == false);
    assert(rows[2]["title"].string() == "LG Advertising Service");
    for (const Json &row : rows.items())
        assert(row["id"].string() != "voiceconductor");

    service::BackgroundServiceInfo cameraInfo;
    assert(service::backgroundServiceInfo("camera", systemRoot, cameraInfo));
    assert(cameraInfo.unit == "com.webos.service.camera.service");
    assert(!service::backgroundServiceInfo("voiceconductor", systemRoot, cameraInfo));
    assert(service::backgroundHiddenServiceAvailable("voiceconductor", systemRoot));
    assert(!service::backgroundHiddenServiceAvailable("camera", systemRoot));
    service::BackgroundServiceInfo voiceInfo;
    assert(service::backgroundHiddenServiceInfo("voiceconductor", systemRoot,
                                                 voiceInfo));
    assert(voiceInfo.unit == "voiceconductor.service");
    assert(!service::backgroundHiddenServiceInfo("camera", systemRoot, voiceInfo));
    service::BackgroundServiceInfo voiceDefinition;
    assert(service::backgroundHiddenServiceDefinition("voiceconductor",
                                                       voiceDefinition));
    assert(voiceDefinition.unit == "voiceconductor.service");
    assert(!service::backgroundHiddenServiceDefinition("camera", voiceDefinition));
    const std::vector<std::string> cameraPrograms =
        service::backgroundServicePrograms("camera", systemRoot);
    assert(cameraPrograms.size() == 1);
    assert(cameraPrograms[0] == "/usr/sbin/camera");
    assert(service::backgroundServiceProcessIds("admanager", procRoot) ==
           std::vector<int>(1, 55));

    std::vector<std::string> disabled;
    std::string error;
    assert(service::setBackgroundServiceDisabled(dataRoot, "camera", false,
                                                  disabled, &error));
    assert(disabled.size() == 1 && disabled[0] == "admanager");
    assert(service::setBackgroundServiceDisabled(dataRoot, "camera", true,
                                                  disabled, &error));
    assert(disabled.size() == 2 && disabled[0] == "admanager" &&
           disabled[1] == "camera");
    assert(service::syncBackgroundServiceBootScript(systemRoot, dataRoot,
                                                     {"camera", "rdxd", "thinqai"},
                                                     &error));
    std::ifstream bootScript(join(systemRoot,
        "/var/lib/webosbrew/init.d/20-tvweb-services"));
    assert(bootScript.good());
    std::ostringstream scriptContents;
    scriptContents << bootScript.rdbuf();
    assert(scriptContents.str().find("systemctl stop \"com.webos.service.camera.service\" \"rdxd.service\"") !=
           std::string::npos);
    assert(scriptContents.str().find("jail \"native_mvpd\"") != std::string::npos);
    assert(service::syncBackgroundServiceBootScript(systemRoot, dataRoot, {}, &error));
    assert(access(join(systemRoot,
        "/var/lib/webosbrew/init.d/20-tvweb-services").c_str(), F_OK) != 0);

    write(dataRoot + "/disabled_services.json", "not json\n");
    assert(!service::readDisabledBackgroundServices(dataRoot, disabled, &error));
    write(dataRoot + "/disabled_services.json",
          "[\"camera\",\"admanager\",\"alwaysready\"]\n");

    const std::string unsupportedRoot = root + "/unsupported";
    makeDirectory(unsupportedRoot);
    Json unsupported = service::backgroundServicesStatus(
        unsupportedRoot, procRoot, dataRoot, {});
    assert(unsupported["ok"].boolean());
    assert(!unsupported["supported"].boolean());
    assert(unsupported["services"].size() == 0);

    const std::string legacyRoot = root + "/legacy";
    makeDirectory(legacyRoot);
    makeParents(legacyRoot, "/sbin/initctl");
    write(join(legacyRoot, "/sbin/initctl"), "#!/bin/sh\nexit 0\n", 0755);
    makeParents(legacyRoot, "/etc/init/crashreportd.conf");
    write(join(legacyRoot, "/etc/init/crashreportd.conf"), "description crashreportd\n");
    assert(service::backgroundServicesSupported(legacyRoot));
    assert(service::backgroundInitctlExecutable(legacyRoot) ==
           join(legacyRoot, "/sbin/initctl"));
    const std::vector<service::BackgroundServiceProbe> legacyProbes =
        service::backgroundServiceProbes(legacyRoot);
    assert(legacyProbes.size() == 1);
    assert(legacyProbes[0].id == "crashreportd");
    assert(legacyProbes[0].upstart == "crashreportd");

    removeTree(root);
    std::cout << "native background service projection tests passed\n";
    return 0;
}
