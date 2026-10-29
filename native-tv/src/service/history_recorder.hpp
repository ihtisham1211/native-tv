#pragma once

#include "core/json.hpp"
#include "domain/live_state.hpp"
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace nativetv {
namespace service {

struct HistoryRecorderOptions {
    std::string directory;
    bool enabled = false;
    std::function<int64_t()> wallNowMs;
    std::function<double()> monotonicNowMs;
};

// Owns watch-time segmentation and durable monthly history files. State is
// supplied by the native live-state cache; queries can overlay dirty in-memory
// hours without forcing a flash write.
class HistoryRecorder {
  public:
    explicit HistoryRecorder(HistoryRecorderOptions options);
    HistoryRecorder(const HistoryRecorder &) = delete;
    HistoryRecorder &operator=(const HistoryRecorder &) = delete;

    void start(const Json &snapshot);
    void observe(const state::Event &event, const Json &snapshot);
    bool setEnabled(bool enabled, std::string *error = nullptr);
    bool clear(std::string *error = nullptr);
    bool flush(std::string *error = nullptr);
    void stop(std::string *error = nullptr);
    Json overlay(int64_t fromMs, int64_t toMs);
    Json totals(int64_t fromMs, int64_t toMs);
    // Watched hours today and this week, and the most watched today, for the
    // telemetry topic (legacy lib/history.js summary); invalid while not running
    // or before the clock is set.
    Json summary(int64_t wallMs, int utcOffsetMinutes);
    Json status() const;

  private:
    struct PendingSegment {
        std::string key;
        double startMonoMs = 0;
        double endMonoMs = 0;
    };

    void startLocked(const Json &snapshot);
    void observeLocked(const state::Event *event, const Json &snapshot);
    void switchToLocked(const std::string &key);
    void checkpointLocked();
    void placePendingLocked();
    void addTimeLocked(const std::string &key, int64_t startWallMs,
                       int64_t endWallMs);
    void loadAllLocked();
    void pruneLocked();
    bool flushLocked(std::string *error);
    Json monthLocked(const std::string &month);
    Json overlayLocked(int64_t fromMs, int64_t toMs);
    std::string keyForLocked(const Json &snapshot) const;

    HistoryRecorderOptions options_;
    mutable std::mutex mutex_;
    std::map<std::string, Json> months_;
    std::set<std::string> dirty_;
    std::map<std::string, std::string> titles_;
    std::map<std::string, std::string> appIds_;
    Json state_;
    std::vector<PendingSegment> pending_;
    std::string currentKey_;
    double currentStartMonoMs_ = 0;
    bool enabled_ = false;
    bool running_ = false;
    bool loaded_ = false;
};

} // namespace service
} // namespace nativetv
