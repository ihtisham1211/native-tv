#include "platform/frame_stats.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace nativetv {
namespace {
// A missed vsync at 60 Hz is 33.3 ms, so anything over 33 is a dropped frame.
// Ambient frames are paced at 33.3 ms, so a dropped one is over 50.
constexpr double kLateAnimatingMs = 33.0, kLateAmbientMs = 50.0;

double percentile(const std::vector<double> &sorted, double fraction) {
    if (sorted.empty())
        return 0;
    // The epsilon keeps 0.99 * 100 from rounding up to rank 100.
    size_t rank = static_cast<size_t>(std::ceil(fraction * sorted.size() - 1e-9));
    return sorted[std::min(sorted.size(), std::max<size_t>(rank, 1)) - 1];
}
double megabytes(size_t bytes) {
    return bytes / (1024.0 * 1024.0);
}
} // namespace

const std::vector<double> &frameIntervalEdgesMs() {
    static const std::vector<double> edges = {8, 12, 15, 17, 20, 25, 33, 50, 100};
    return edges;
}

FrameLog::FrameLog(const std::string &path) {
    if (path.empty())
        return;
    file_ = std::fopen(path.c_str(), "w");
    if (!file_)
        throw std::runtime_error("Could not write the perf log: " + path);
}
FrameLog::~FrameLog() {
    if (file_)
        std::fclose(file_);
}

void FrameLog::record(double atMs, double intervalMs, int swapInterval, const FrameStats &stats) {
    const bool ambient = swapInterval > 1;
    if (file_) {
        std::fprintf(file_, "{\"frame\":%u,\"atMs\":%.1f,\"intervalMs\":", frames_, atMs);
        if (intervalMs >= 0)
            std::fprintf(file_, "%.2f", intervalMs);
        else
            std::fputs("null", file_);
        std::fprintf(file_,
                     ",\"swap\":%d,\"buildMs\":%.2f,\"submitMs\":%.2f,\"draws\":%u,"
                     "\"vertices\":%u,\"textureMB\":%.2f}\n",
                     swapInterval, stats.buildMs, stats.submitMs, stats.draws, stats.vertices,
                     megabytes(stats.textureBytes));
    }
    ++frames_;
    buildTotal_ += stats.buildMs;
    submitTotal_ += stats.submitMs;
    peak_.draws = std::max(peak_.draws, stats.draws);
    peak_.vertices = std::max(peak_.vertices, stats.vertices);
    peak_.textureBytes = std::max(peak_.textureBytes, stats.textureBytes);
    if (ambient)
        ++ambient_;
    if (intervalMs < 0)
        return;
    if (intervalMs > (ambient ? kLateAmbientMs : kLateAnimatingMs))
        ++late_;
    if (!ambient)
        intervals_.push_back(intervalMs);
}

FrameSummary FrameLog::summary() const {
    FrameSummary result;
    result.frames = frames_;
    result.ambientFrames = ambient_;
    result.lateFrames = late_;
    std::vector<double> sorted = intervals_;
    std::sort(sorted.begin(), sorted.end());
    result.p50 = percentile(sorted, 0.50);
    result.p95 = percentile(sorted, 0.95);
    result.p99 = percentile(sorted, 0.99);
    result.max = sorted.empty() ? 0 : sorted.back();
    if (frames_) {
        result.buildMs = buildTotal_ / frames_;
        result.submitMs = submitTotal_ / frames_;
    }
    result.draws = peak_.draws;
    result.vertices = peak_.vertices;
    result.textureMb = megabytes(peak_.textureBytes);
    const auto &edges = frameIntervalEdgesMs();
    result.histogram.assign(edges.size() + 1, 0);
    for (double interval : sorted)
        ++result.histogram[std::upper_bound(edges.begin(), edges.end(), interval) -
                           edges.begin()];
    return result;
}

void FrameLog::finish() {
    if (!file_)
        return;
    const FrameSummary s = summary();
    std::fprintf(file_,
                 "{\"summary\":{\"frames\":%u,\"ambientFrames\":%u,\"p50Ms\":%.2f,"
                 "\"p95Ms\":%.2f,\"p99Ms\":%.2f,\"maxMs\":%.2f,\"lateFrames\":%u,"
                 "\"buildMs\":%.2f,\"submitMs\":%.2f,\"draws\":%u,\"vertices\":%u,"
                 "\"textureMB\":%.2f,\"histogramEdgesMs\":[",
                 s.frames, s.ambientFrames, s.p50, s.p95, s.p99, s.max, s.lateFrames, s.buildMs,
                 s.submitMs, s.draws, s.vertices, s.textureMb);
    const auto &edges = frameIntervalEdgesMs();
    for (size_t i = 0; i < edges.size(); ++i)
        std::fprintf(file_, "%s%g", i ? "," : "", edges[i]);
    std::fputs("],\"histogram\":[", file_);
    for (size_t i = 0; i < s.histogram.size(); ++i)
        std::fprintf(file_, "%s%u", i ? "," : "", s.histogram[i]);
    std::fputs("]}}\n", file_);
    std::fflush(file_);
}

std::string summaryLine(const FrameSummary &s) {
    char line[256];
    std::snprintf(line, sizeof(line),
                  "perf: frames=%u ambient=%u p50=%.1f p95=%.1f p99=%.1f max=%.1f late=%u "
                  "build=%.2f submit=%.2f draws=%u vertices=%u textureMB=%.1f",
                  s.frames, s.ambientFrames, s.p50, s.p95, s.p99, s.max, s.lateFrames, s.buildMs,
                  s.submitMs, s.draws, s.vertices, s.textureMb);
    return line;
}

} // namespace nativetv
