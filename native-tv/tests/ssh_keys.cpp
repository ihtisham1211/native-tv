#include "service/ssh_keys.hpp"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace nativetv;

namespace {
const std::string kEd =
    "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIHNWUlvIOI3rP9HRKBnPo43I42IfLCo105Go4U7x4Vef ed@test";
const std::string kEdFingerprint = "SHA256:2zyRteKuXNx0xachsViK0oWVjR5mFrpcCEQwrjhb7mM";
const std::string kEc =
    "ecdsa-sha2-nistp256 AAAAE2VjZHNhLXNoYTItbmlzdHAyNTYAAAAIbmlzdHAyNTYAAABBBOjDvPcUSd/g8kmInR3WZYXM+kJHSfnV0P/dEmnHENV/V3qJ+vZCMTqAxgX6zJWMwyjIfJYAI0KPTECjGTuC22Y= ec@test";
const std::string kEcFingerprint = "SHA256:slxSqMW+W9tmAUsZZPzeYNhCuxTcAQzSq3YbVP2ofsc";

void write(const std::string &path, const std::string &contents) {
    std::ofstream output(path.c_str(), std::ios::binary);
    assert(output.good());
    output << contents;
}

std::string path(const std::string &root, const std::string &absolute) {
    return root + absolute;
}

void removeTree(const std::string &root) {
    const std::string command = "rm -rf " + root;
    assert(std::system(command.c_str()) == 0);
}
} // namespace

int main() {
    service::SshPublicKey parsed;
    assert(service::parseSshPublicKey(kEd, parsed));
    assert(parsed.type == "ssh-ed25519");
    assert(parsed.blob == kEd.substr(12, kEd.find(' ', 12) - 12));
    assert(parsed.comment == "ed@test");
    assert(parsed.options.empty());
    assert(parsed.fingerprint == kEdFingerprint);
    assert(service::sshKeyFingerprint(parsed.blob) == kEdFingerprint);
    assert(service::parseSshPublicKey(
        "from=\"10.0.0.1, 10.0.0.2\",no-pty " + kEd, parsed));
    assert(parsed.options == "from=\"10.0.0.1, 10.0.0.2\",no-pty");
    assert(parsed.comment == "ed@test");
    assert(service::parseSshPublicKey(kEc, parsed));
    assert(parsed.fingerprint == kEcFingerprint);
    assert(!service::parseSshPublicKey("# " + kEd, parsed));
    assert(!service::parseSshPublicKey("ssh-rsa " + kEd.substr(kEd.find(' ') + 1), parsed));
    assert(!service::parseSshPublicKey("ssh-ed25519 not-base64!", parsed));
    assert(!service::parseSshPublicKey("ssh-ed25519 AAA=", parsed));

    char directory[] = "/tmp/nativetv-ssh-XXXXXX";
    char *created = mkdtemp(directory);
    assert(created);
    const std::string root(created);
    assert(mkdir(path(root, "/etc").c_str(), 0755) == 0);
    assert(mkdir(path(root, "/proc").c_str(), 0755) == 0);
    assert(mkdir(path(root, "/proc/27").c_str(), 0755) == 0);
    write(path(root, "/etc/shadow"),
          "root:xGVw8H4GqkKg6:19000:0:99999:7:::\nnobody:*:19000::::::\n");
    write(path(root, "/proc/27/comm"), "dropbear\n");

    Json status = service::sshKeysStatus(root, path(root, "/proc"));
    assert(status["ok"].boolean());
    assert(status["running"].boolean());
    assert(!status["fileExists"].boolean());
    assert(status["passwordNow"].boolean());
    assert(status["passwordAfterReboot"].boolean());
    assert(status["keys"].size() == 0);

    Json added = service::addSshKey(root, path(root, "/proc"), kEd);
    assert(added["ok"].boolean());
    assert(added["fileExists"].boolean());
    assert(added["passwordNow"].boolean());
    assert(!added["passwordAfterReboot"].boolean());
    assert(added["keys"].size() == 1);
    assert(added["keys"][0]["fingerprint"].string() == kEdFingerprint);
    assert(added["keys"][0]["comment"].string() == "ed@test");
    struct stat fileInfo = {};
    assert(stat(path(root, "/home/root/.ssh/authorized_keys").c_str(), &fileInfo) == 0);
    assert((fileInfo.st_mode & 0777) == 0600);
    assert(stat(path(root, "/home/root/.ssh").c_str(), &fileInfo) == 0);
    assert((fileInfo.st_mode & 0777) == 0700);
    std::ifstream saved(path(root, "/home/root/.ssh/authorized_keys").c_str());
    std::string contents((std::istreambuf_iterator<char>(saved)),
                         std::istreambuf_iterator<char>());
    assert(contents == kEd + "\n");

    Json duplicate = service::addSshKey(root, path(root, "/proc"),
                                        kEd.substr(0, kEd.size() - 1) + "other");
    assert(!duplicate["ok"].boolean());
    duplicate = service::addSshKey(root, path(root, "/proc"), kEd);
    assert(!duplicate["ok"].boolean());
    assert(duplicate["error"].string() == "That key is already there.");
    Json bad = service::addSshKey(root, path(root, "/proc"),
                                  "-----BEGIN OPENSSH PRIVATE KEY-----");
    assert(!bad["ok"].boolean());
    assert(bad["error"].string().find("private key") != std::string::npos);
    bad = service::addSshKey(root, path(root, "/proc"), kEd + "\n" + kEc);
    assert(!bad["ok"].boolean());

    added = service::addSshKey(root, path(root, "/proc"), kEc);
    assert(added["ok"].boolean());
    assert(added["keys"].size() == 2);
    Json removed = service::removeSshKey(root, path(root, "/proc"), kEdFingerprint);
    assert(removed["ok"].boolean());
    assert(removed["keys"].size() == 1);
    assert(removed["keys"][0]["fingerprint"].string() == kEcFingerprint);
    Json missing = service::removeSshKey(root, path(root, "/proc"), kEdFingerprint);
    assert(!missing["ok"].boolean());

    write(path(root, "/home/root/.ssh/authorized_keys"), "# keep\n" + kEc + "\n");
    removed = service::removeSshKey(root, path(root, "/proc"), kEcFingerprint);
    assert(removed["ok"].boolean());
    std::ifstream comments(path(root, "/home/root/.ssh/authorized_keys").c_str());
    contents.assign(std::istreambuf_iterator<char>(comments),
                    std::istreambuf_iterator<char>());
    assert(contents == "# keep\n");

    Json deleted = service::deleteSshKeysFile(root, path(root, "/proc"));
    assert(deleted["ok"].boolean());
    assert(!deleted["fileExists"].boolean());
    assert(deleted["passwordAfterReboot"].boolean());
    assert(service::deleteSshKeysFile(root, path(root, "/proc"))["ok"].boolean());

    removeTree(root);
    std::cout << "native SSH key policy and atomic file tests passed\n";
    return 0;
}
