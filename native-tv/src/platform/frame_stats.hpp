#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace nativetv {

// Counters for one rendered frame. The loop measures buildMs and submitMs; the
// renderer's own counters feed draws, vertices and textureBytes.
struct FrameStats {
    double buildMs = 0, submitMs = 0;
    unsigned draws = 0, vertices = 0;
    size_t textureBytes = 0;
};

struct FrameSummary {
    unsigned frames = 0, ambientFrames = 0, lateFrames = 0;
    // Intervals between swaps of frames drawn at the interactive pace (1), in ms.
    double p50 = 0, p95 = 0, p99 = 0, max = 0;
    // Means over all frames.
    double buildMs = 0, submitMs = 0;
    // Peaks over all frames.
    unsigned draws = 0, vertices = 0;
    double textureMb = 0;
    // Counts per bin of frameIntervalEdgesMs(), plus one overflow bin.
    std::vector<unsigned> histogram;
};

const std::vector<double> &frameIntervalEdgesMs();

// Per-frame log behind `--perf`: every frame is appended to the file as a JSON
// line when it is recorded, and finish() appends the summary record.
class FrameLog {
    std::FILE *file_ = nullptr;
    std::vector<double> intervals_;
    unsigned frames_ = 0, ambient_ = 0, late_ = 0;
    double buildTotal_ = 0, submitTotal_ = 0;
    FrameStats peak_;

  public:
    // An empty path keeps the statistics in memory only.
    explicit FrameLog(const std::string &path);
    ~FrameLog();
    FrameLog(const FrameLog &) = delete;
    FrameLog &operator=(const FrameLog &) = delete;

    // intervalMs is the time since the previous swap; negative when there is no
    // previous frame to measure against (the first one, or the first after the
    // loop idled). swapInterval is 1 while animating and 2 in ambient mode.
    void record(double atMs, double intervalMs, int swapInterval, const FrameStats &stats);
    FrameSummary summary() const;
    void finish();
};

// "perf: frames=3600 ambient=0 p50=16.7 p95=16.8 ..." (see frame_stats.cpp).
std::string summaryLine(const FrameSummary &summary);

} // namespace nativetv
