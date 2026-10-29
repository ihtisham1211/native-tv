#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

namespace nativetv {
namespace daemon {

class AppInstallUploadReservation {
  public:
    enum class ReserveResult { reserved, tooLarge, busy, noSpace, failed };

    explicit AppInstallUploadReservation(
        std::chrono::milliseconds holdDuration = std::chrono::minutes(30));

    ReserveResult reserve(const std::string &stagingDirectory, uint64_t bytes,
                          uint64_t maximumBytes, bool installBusy,
                          std::string &file, uint64_t &availableBytes,
                          std::string *error = nullptr);
    bool touch(const std::string &file);
    bool held();
    bool owns(const std::string &file);
    bool release(const std::string &stagingDirectory, const std::string &file,
                 bool removeFile, std::string *error = nullptr);
    bool complete(const std::string &file);

  private:
    void expireLocked(std::chrono::steady_clock::time_point now);

    std::chrono::milliseconds holdDuration_;
    std::mutex mutex_;
    std::string file_;
    std::chrono::steady_clock::time_point touchedAt_{};
};

} // namespace daemon
} // namespace nativetv
