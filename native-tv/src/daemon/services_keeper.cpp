#include "daemon/services_keeper.hpp"
#include <algorithm>
#include <sys/stat.h>
#include <unistd.h>

namespace nativetv {
namespace daemon {
namespace {

// Ids an earlier release switched off that this one has no use for.
const char *const kRetired[] = {"tvdataexchanger", "alwaysready"};

std::string rooted(const std::string &root, const std::string &absolute) {
    return root == "/" || root.empty() ? absolute : root + absolute;
}

} // namespace

ServicesKeeper::ServicesKeeper(ServicesKeeperDeps deps) : deps_(std::move(deps)) {}

bool ServicesKeeper::infoFor(const std::string &id, service::BackgroundServiceInfo &info) const {
    return service::backgroundServiceInfo(id, deps_.systemRoot, info) ||
           service::backgroundHiddenServiceDefinition(id, info);
}

std::vector<std::string> ServicesKeeper::start() {
    std::vector<std::string> list;
    std::string error;
    if (!service::readDisabledBackgroundServices(deps_.dataRoot, list, &error)) {
        if (deps_.log) deps_.log("services: " + error);
        return {};
    }
    bool retired = false;
    for (const char *id : kRetired)
        retired = retired || std::find(list.begin(), list.end(), id) != list.end();
    if (retired && !service::setBackgroundServiceDisabled(deps_.dataRoot, kRetired[0], false, list, &error) &&
        deps_.log)
        deps_.log("services: could not retire old entries: " + error);
    (void)unlink(rooted(deps_.systemRoot, "/var/lib/webosbrew/init.d/20-services.sh").c_str());
    if (!service::syncBackgroundServiceBootScript(deps_.systemRoot, deps_.dataRoot, list, &error) &&
        deps_.log)
        deps_.log("services: could not write the boot hook: " + error);

    const std::string systemctl = service::backgroundSystemctlExecutable(deps_.systemRoot);
    if (systemctl.empty()) return list;
    const std::string transient = rooted(deps_.systemRoot, "/run/systemd/transient");
    (void)mkdir(transient.c_str(), 0755);
    // A mask an earlier version left on a retired unit.
    std::vector<std::string> unmasked;
    for (const char *id : kRetired)
        if (unlink((transient + "/" + id + ".service").c_str()) == 0)
            unmasked.push_back(std::string(id) + ".service");
    // The boot hook binds the stubs; this covers a TV the hook has not run on yet.
    for (const std::string &id : list) {
        service::BackgroundServiceInfo info;
        if (infoFor(id, info) && deps_.bindStubs) (void)deps_.bindStubs(info, id);
    }
    if (!unmasked.empty() && deps_.run) {
        (void)deps_.run(systemctl, {"daemon-reload"});
        std::vector<std::string> arguments = {"start"};
        arguments.insert(arguments.end(), unmasked.begin(), unmasked.end());
        (void)deps_.run(systemctl, arguments);
    }
    return list;
}

size_t ServicesKeeper::enforce() {
    std::vector<std::string> list;
    if (!service::readDisabledBackgroundServices(deps_.dataRoot, list) || list.empty()) return 0;
    const std::string systemctl = service::backgroundSystemctlExecutable(deps_.systemRoot);
    const std::string initctl = service::backgroundInitctlExecutable(deps_.systemRoot);
    size_t found = 0;
    for (const std::string &id : list) {
        service::BackgroundServiceInfo info;
        if (!infoFor(id, info) || info.bin.empty() ||
            service::backgroundServiceProcessIds(id, deps_.procRoot).empty())
            continue;
        ++found;
        if (deps_.log) deps_.log("services: " + id + " was running while switched off, stopping it");
        // The unit or job first, so nothing supervising it starts it again.
        if (deps_.run && !systemctl.empty() && !info.unit.empty()) (void)deps_.run(systemctl, {"stop", info.unit});
        if (deps_.run && !initctl.empty() && !info.upstart.empty()) (void)deps_.run(initctl, {"stop", info.upstart});
        // What the hub launched outside the unit is then signalled directly.
        if (deps_.later && deps_.terminate) {
            const std::string procRoot = deps_.procRoot;
            const auto terminate = deps_.terminate;
            deps_.later(std::chrono::milliseconds(3000), [id, procRoot, terminate]() {
                for (const int pid : service::backgroundServiceProcessIds(id, procRoot)) terminate(pid);
            });
        }
    }
    return found;
}

} // namespace daemon
} // namespace nativetv
