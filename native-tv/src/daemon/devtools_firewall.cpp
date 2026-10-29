#include "daemon/devtools_firewall.hpp"
#include "daemon/child_process.hpp"
#include "daemon/logger.hpp"
#include <chrono>
#include <unistd.h>
#include <utility>
#include <vector>

namespace nativetv {
namespace daemon {
namespace {
const char *const kRule[] = {
    "INPUT", "-p", "tcp", "--dport", "9998", "!", "-i", "lo", "-j", "DROP"
};

std::string rooted(const std::string &root, const std::string &path) {
    if (root.empty() || root == "/") return path;
    size_t end = root.size();
    while (end > 1 && root[end - 1] == '/') --end;
    return root.substr(0, end) + path;
}

std::string findTool(const std::string &root, const std::string &name) {
    const std::string candidates[] = {
        "/usr/sbin/" + name, "/sbin/" + name
    };
    for (const std::string &candidate : candidates) {
        const std::string path = rooted(root, candidate);
        if (access(path.c_str(), X_OK) == 0) return path;
    }
    return std::string();
}

ChildResult runRule(ChildProcessRunner &children, const std::string &tool,
                    const std::string &operation) {
    ChildOptions options;
    options.arguments.push_back(tool);
    options.arguments.push_back(operation);
    for (const char *argument : kRule)
        options.arguments.push_back(argument);
    options.timeout = std::chrono::milliseconds(5000);
    options.maximumOutputBytes = 4096;
    return children.run(options);
}
} // namespace

DevtoolsFirewall::DevtoolsFirewall(ChildProcessRunner &children,
                                   std::string systemRoot,
                                   std::string procRoot, Logger *logger)
    : children_(children), systemRoot_(std::move(systemRoot)),
      procRoot_(std::move(procRoot)), logger_(logger) {}

Json DevtoolsFirewall::setNetworkAllowed(bool allowed) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string devmode = rooted(systemRoot_, "/var/luna/preferences/devmode_enabled");
    if (access(devmode.c_str(), F_OK) != 0) {
        status_ = "off";
        Json response = Json::object();
        response.set("ok", Json::flag(true));
        response.set("devtools", Json::text(status_));
        return response;
    }

    const std::vector<std::string> tools = {
        findTool(systemRoot_, "iptables"), findTool(systemRoot_, "ip6tables")
    };
    std::vector<std::string> held;
    for (const std::string &tool : tools) {
        if (tool.empty()) continue;
        const ChildResult check = runRule(children_, tool, "-C");
        if (allowed) {
            if (check.succeeded() && !runRule(children_, tool, "-D").succeeded())
                held.push_back(tool);
        } else if (check.succeeded() || runRule(children_, tool, "-I").succeeded()) {
            held.push_back(tool);
        }
    }

    if (allowed) {
        status_ = held.empty() ? "allowed" : "closed";
    } else if (!held.empty()) {
        status_ = "closed";
    } else if ((tools[0].empty() && tools[1].empty()) ||
               access((procRoot_ + "/net/ip_tables_names").c_str(), F_OK) != 0) {
        status_ = "unsupported";
    } else {
        status_ = "open";
    }
    if (logger_) {
        if (status_ == "closed")
            logger_->info("devtools: port 9998 bound to TV only");
        else if (status_ == "allowed")
            logger_->info("devtools: port 9998 left open to the network (allowNetworkDebugger)");
        else if (status_ == "unsupported")
            logger_->warning("devtools: port 9998 cannot be restricted by this TV's firewall");
        else
            logger_->warning("devtools: could not close port 9998 to the network");
    }
    Json response = Json::object();
    response.set("ok", Json::flag(true));
    response.set("devtools", Json::text(status_));
    return response;
}

std::string DevtoolsFirewall::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}
} // namespace daemon
} // namespace nativetv
