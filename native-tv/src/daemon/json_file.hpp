#pragma once

#include "core/json.hpp"
#include <cstddef>
#include <functional>
#include <string>

namespace nativetv {
struct JsonFileResult {
    Json value;
    bool found = false;
    bool recovered = false;
    std::string error;
};

// Changes whenever the file is replaced or rewritten: modification time to the nanosecond, size
// and inode, or "-" when it is missing. A poller compares it to skip reading a file that has
// not changed; the daemon's own files are written by atomic rename, which changes the inode.
std::string fileStamp(const std::string &path);

// A small crash-safe JSON file store for daemon-owned state. Writes replace
// the file atomically and keep the previous valid value as a recovery copy.
class JsonFile {
  public:
    explicit JsonFile(std::string path, size_t maxBytes = 8 * 1024 * 1024);

    JsonFileResult load() const;
    // The optional hook runs after the temporary file is synced and before it
    // replaces the committed file. It allows deterministic interruption tests.
    bool save(const Json &value, std::string *error = nullptr,
              const std::function<void()> &beforeCommit = std::function<void()>()) const;
    const std::string &path() const;

  private:
    std::string path_;
    size_t maxBytes_;
};
} // namespace nativetv
