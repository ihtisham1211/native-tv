// What test/test-devtools.js asserts about the rule that keeps port 9998 (the web
// app inspector) off the network: with fake iptables and ip6tables in a fake root.
#include "daemon/child_process.hpp"
#include "daemon/devtools_firewall.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

// Fixture folders are made and removed with the shell; a failure shows in the checks that read them.
static void runShell(const std::string &command) {
    if (std::system(command.c_str()) != 0) {
    }
}


using nativetv::daemon::ChildProcessRunner;
using nativetv::daemon::DevtoolsFirewall;

namespace {
int failures = 0;

void check(bool condition, const std::string &what) {
    std::printf("%s %s\n", condition ? "  ok" : "FAIL", what.c_str());
    if (!condition) ++failures;
}
bool exists(const std::string &path) { return access(path.c_str(), F_OK) == 0; }
std::string slurp(const std::string &path) {
    std::ifstream input(path);
    std::stringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}
void mkdirs(const std::string &path) {
    for (size_t at = 1; at <= path.size(); ++at)
        if (at == path.size() || path[at] == '/') mkdir(path.substr(0, at).c_str(), 0755);
}

// One TV: a root with the tools where the case puts them, a log of every call
// and the state each family's table is in (rule present, insert refused, delete stuck).
struct Tv {
    std::string root, calls, state;
    explicit Tv(const std::string &base) {
        root = base + "/root";
        calls = base + "/calls";
        state = base + "/state";
        mkdirs(root + "/var/luna/preferences");
        mkdirs(root + "/usr/sbin");
        mkdirs(root + "/sbin");
        mkdirs(state);
    }
    void developerMode() { std::ofstream(root + "/var/luna/preferences/devmode_enabled") << "1"; }
    void firewall() {
        mkdirs(root + "/proc/net");
        std::ofstream(root + "/proc/net/ip_tables_names") << "filter\n";
    }
    void tool(const std::string &name, const std::string &directory = "/usr/sbin") {
        const std::string path = root + directory + "/" + name;
        std::ofstream(path) << "#!/bin/sh\n"
                               "echo \"" << name << " $*\" >> '" << calls << "'\n"
                               "S='" << state << "/" << name << "'\n"
                               "case \"$1\" in\n"
                               "  -C) [ -f \"$S.rule\" ] ;;\n"
                               "  -I) [ -f \"$S.refuse\" ] && exit 1; : > \"$S.rule\" ;;\n"
                               "  -D) [ -f \"$S.stuck\" ] && exit 1; rm -f \"$S.rule\" ;;\n"
                               "esac\n";
        chmod(path.c_str(), 0755);
    }
    void with(const std::string &name, const std::string &flag) { std::ofstream(state + "/" + name + "." + flag) << "1"; }
    bool rule(const std::string &name) const { return exists(state + "/" + name + ".rule"); }
    std::string log() const { return slurp(calls); }
};
std::string statusOf(const nativetv::Json &reply) { return reply["devtools"].string(); }
} // namespace

int main() {
    char pattern[] = "/tmp/nativetv-devtools-XXXXXX";
    const std::string base = mkdtemp(pattern);
    int index = 0;
    const auto fresh = [&] { return Tv(base + "/" + std::to_string(++index)); };
    ChildProcessRunner children;

    {   // without Developer Mode nothing is touched
        Tv tv = fresh(); tv.tool("iptables"); tv.firewall();
        DevtoolsFirewall fw(children, tv.root, tv.root + "/proc");
        check(statusOf(fw.setNetworkAllowed(false)) == "off" && tv.log().empty(),
              "without Developer Mode nothing is touched");
    }
    {   // the rule is added for each family when it is missing
        Tv tv = fresh(); tv.developerMode(); tv.firewall(); tv.tool("iptables"); tv.tool("ip6tables");
        DevtoolsFirewall fw(children, tv.root, tv.root + "/proc");
        check(statusOf(fw.setNetworkAllowed(false)) == "closed" && tv.rule("iptables") && tv.rule("ip6tables"),
              "the rule is added for each family when it is missing");
        const std::string log = tv.log();
        check(log.find("iptables -I INPUT -p tcp --dport 9998 ! -i lo -j DROP") != std::string::npos &&
                  log.find("ip6tables -I INPUT -p tcp --dport 9998 ! -i lo -j DROP") != std::string::npos,
              "the rule drops 9998 from everything but loopback");
    }
    {   // a rule already in place is not added twice
        Tv tv = fresh(); tv.developerMode(); tv.firewall(); tv.tool("iptables"); tv.tool("ip6tables");
        tv.with("iptables", "rule"); tv.with("ip6tables", "rule");
        DevtoolsFirewall fw(children, tv.root, tv.root + "/proc");
        check(statusOf(fw.setNetworkAllowed(false)) == "closed" && tv.log().find(" -I ") == std::string::npos,
              "a rule already in place is not added twice");
    }
    {   // a kernel without ip6_tables still gets the IPv4 rule
        Tv tv = fresh(); tv.developerMode(); tv.firewall(); tv.tool("iptables"); tv.tool("ip6tables");
        tv.with("ip6tables", "refuse");
        DevtoolsFirewall fw(children, tv.root, tv.root + "/proc");
        check(statusOf(fw.setNetworkAllowed(false)) == "closed" && tv.rule("iptables") && !tv.rule("ip6tables"),
              "a kernel without ip6_tables still gets the IPv4 rule");
    }
    {   // the dashboard is told when the port could not be closed
        Tv tv = fresh(); tv.developerMode(); tv.firewall(); tv.tool("iptables"); tv.tool("ip6tables");
        tv.with("iptables", "refuse"); tv.with("ip6tables", "refuse");
        DevtoolsFirewall fw(children, tv.root, tv.root + "/proc");
        check(statusOf(fw.setNetworkAllowed(false)) == "open" && fw.status() == "open",
              "the dashboard is told when the port could not be closed");
    }
    {   // a kernel without a firewall is reported as one
        Tv tv = fresh(); tv.developerMode(); tv.tool("iptables"); tv.tool("ip6tables");
        tv.with("iptables", "refuse"); tv.with("ip6tables", "refuse");
        DevtoolsFirewall fw(children, tv.root, tv.root + "/proc");
        check(statusOf(fw.setNetworkAllowed(false)) == "unsupported",
              "a kernel without a firewall is reported as one");
    }
    {   // a TV without iptables is reported as having no firewall
        Tv tv = fresh(); tv.developerMode(); tv.firewall();
        DevtoolsFirewall fw(children, tv.root, tv.root + "/proc");
        check(statusOf(fw.setNetworkAllowed(false)) == "unsupported",
              "a TV without iptables is reported as having no firewall");
    }
    {   // the tools are found in /sbin too
        Tv tv = fresh(); tv.developerMode(); tv.firewall(); tv.tool("iptables", "/sbin"); tv.tool("ip6tables", "/sbin");
        DevtoolsFirewall fw(children, tv.root, tv.root + "/proc");
        check(statusOf(fw.setNetworkAllowed(false)) == "closed" && tv.rule("iptables"),
              "the tools are found in /sbin too");
    }
    {   // allowNetworkDebugger takes out a rule an earlier start left
        Tv tv = fresh(); tv.developerMode(); tv.firewall(); tv.tool("iptables"); tv.tool("ip6tables");
        tv.with("iptables", "rule"); tv.with("ip6tables", "rule");
        DevtoolsFirewall fw(children, tv.root, tv.root + "/proc");
        check(statusOf(fw.setNetworkAllowed(true)) == "allowed" && !tv.rule("iptables") && !tv.rule("ip6tables"),
              "allowNetworkDebugger takes out a rule an earlier start left");
    }
    {   // allowNetworkDebugger leaves a TV without the rule alone
        Tv tv = fresh(); tv.developerMode(); tv.firewall(); tv.tool("iptables"); tv.tool("ip6tables");
        DevtoolsFirewall fw(children, tv.root, tv.root + "/proc");
        check(statusOf(fw.setNetworkAllowed(true)) == "allowed" && tv.log().find(" -D ") == std::string::npos,
              "allowNetworkDebugger leaves a TV without the rule alone");
    }
    {   // a rule that cannot be taken out is reported as closed
        Tv tv = fresh(); tv.developerMode(); tv.firewall(); tv.tool("iptables"); tv.tool("ip6tables");
        tv.with("iptables", "rule"); tv.with("iptables", "stuck");
        DevtoolsFirewall fw(children, tv.root, tv.root + "/proc");
        check(statusOf(fw.setNetworkAllowed(true)) == "closed" && fw.status() == "closed",
              "a rule that cannot be taken out is reported as closed");
    }
    {   // allowNetworkDebugger without Developer Mode touches nothing
        Tv tv = fresh(); tv.firewall(); tv.tool("iptables"); tv.with("iptables", "rule");
        DevtoolsFirewall fw(children, tv.root, tv.root + "/proc");
        check(statusOf(fw.setNetworkAllowed(true)) == "off" && tv.rule("iptables") && tv.log().empty(),
              "allowNetworkDebugger without Developer Mode touches nothing");
    }

    const std::string cleanup = "rm -rf " + base;
    runShell(cleanup);
    return failures ? 1 : 0;
}
