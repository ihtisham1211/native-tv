#pragma once

#include "core/json.hpp"
#include <string>

namespace nativetv {
namespace service {

struct SshPublicKey {
    std::string type;
    std::string blob;
    std::string comment;
    std::string options;
    std::string fingerprint;
};

bool parseSshPublicKey(const std::string &line, SshPublicKey &key);
std::string sshKeyFingerprint(const std::string &blob);
Json sshKeysStatus(const std::string &systemRoot, const std::string &procRoot);
Json addSshKey(const std::string &systemRoot, const std::string &procRoot,
               const std::string &line);
Json removeSshKey(const std::string &systemRoot, const std::string &procRoot,
                  const std::string &fingerprint);
Json deleteSshKeysFile(const std::string &systemRoot, const std::string &procRoot);

} // namespace service
} // namespace nativetv
