#include "platform/app_clock.hpp"
#include "platform/frame_stats.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using namespace nativetv;

namespace {
std::string readAll(const std::string &path) {
    std::ifstream file(path);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}
FrameStats stats(double build, double submit, unsigned draws, size_t megabytes) {
    FrameStats result;
    result.buildMs = build;
    result.submitMs = submit;
    result.draws = draws;
    result.vertices = draws * 6;
    result.textureBytes = megabytes * 1024 * 1024;
    return result;
}

void fixedClockRepeats() {
    AppClock first(true, 123456), second(true, 999);
    // Frame N is at N * 1000 / 60 ms, whatever the real time is.
    const unsigned expected[] = {0, 16, 33, 50, 66, 83, 100};
    for (unsigned want : expected) {
        assert(first.tick(5) == want);
        assert(second.tick(777777) == want);
    }
    // A pause does not move a fixed clock.
    first.pause(10);
    assert(first.tick(20) == 116);
    // Sixty frames are one second exactly, with no drift over a long run.
    AppClock long_run(true, 0);
    unsigned last = 0;
    for (unsigned i = 0; i <= 60 * 3600; ++i)
        last = long_run.tick(0);
    assert(last == 3600 * 1000);
}

void realClockSkipsPausedTime() {
    AppClock clock(false, 1000);
    assert(clock.tick(1000) == 0);
    assert(clock.tick(1500) == 500);
    clock.pause(1600);
    assert(clock.tick(1600) == 600);
    assert(clock.tick(9000) == 600); // frozen while paused
    clock.pause(9500);               // a second pause changes nothing
    clock.resume(10000);
    assert(clock.tick(10000) == 600); // 8400 ms paused
    assert(clock.tick(10100) == 700);
    clock.resume(10200); // resume without a pause changes nothing
    assert(clock.tick(10200) == 800);
}

void summarisesIntervals() {
    FrameLog log("");
    // The first frame has nothing to measure against.
    log.record(0, -1, 1, stats(1, 2, 10, 30));
    for (int i = 1; i <= 100; ++i)
        log.record(i * 16.7, i == 100 ? 40.0 : 16.7, 1, stats(1, 2, 10, 30));
    // Ambient frames are 33 ms apart and are not part of the percentiles.
    for (int i = 0; i < 4; ++i)
        log.record(2000 + i * 33.3, i == 3 ? 60.0 : 33.3, 2, stats(1, 2, 3, 12));
    FrameSummary s = log.summary();
    assert(s.frames == 105);
    assert(s.ambientFrames == 4);
    // One animating interval over 33 ms and one ambient interval over 50 ms.
    assert(s.lateFrames == 2);
    assert(s.p50 == 16.7 && s.p95 == 16.7 && s.p99 == 16.7);
    assert(s.max == 40.0);
    assert(s.buildMs == 1 && s.submitMs == 2);
    assert(s.draws == 10 && s.vertices == 60);
    assert(std::fabs(s.textureMb - 30) < 1e-9);
    // 99 intervals in [15, 17), one in [33, 50).
    const std::vector<unsigned> histogram = {0, 0, 0, 99, 0, 0, 0, 1, 0, 0};
    assert(s.histogram == histogram);
    assert(frameIntervalEdgesMs().size() + 1 == s.histogram.size());
    assert(summaryLine(s) ==
           "perf: frames=105 ambient=4 p50=16.7 p95=16.7 p99=16.7 max=40.0 late=2 "
           "build=1.00 submit=2.00 draws=10 vertices=60 textureMB=30.0");
}

void percentilesUseNearestRank() {
    FrameLog log("");
    for (int i = 1; i <= 100; ++i)
        log.record(i, i, 1, FrameStats());
    FrameSummary s = log.summary();
    assert(s.p50 == 50 && s.p95 == 95 && s.p99 == 99 && s.max == 100);
    // Nothing recorded: zeros, not a crash.
    FrameSummary none = FrameLog("").summary();
    assert(none.frames == 0 && none.p95 == 0 && none.max == 0);
    assert(summaryLine(none) ==
           "perf: frames=0 ambient=0 p50=0.0 p95=0.0 p99=0.0 max=0.0 late=0 build=0.00 "
           "submit=0.00 draws=0 vertices=0 textureMB=0.0");
}

void writesLogFile() {
    const std::string path = "frame_loop_test.jsonl";
    {
        FrameLog log(path);
        log.record(0, -1, 1, stats(0.5, 1.25, 12, 2));
        log.record(16.7, 16.7, 1, stats(0.5, 1.25, 12, 2));
        log.finish();
    }
    std::istringstream lines(readAll(path));
    std::string first, second, last, extra;
    assert(std::getline(lines, first) && std::getline(lines, second) && std::getline(lines, last));
    assert(!std::getline(lines, extra));
    assert(first ==
           "{\"frame\":0,\"atMs\":0.0,\"intervalMs\":null,\"swap\":1,\"buildMs\":0.50,"
           "\"submitMs\":1.25,\"draws\":12,\"vertices\":72,\"textureMB\":2.00,\"inputMs\":null}");
    assert(second.find("\"frame\":1,\"atMs\":16.7,\"intervalMs\":16.70,") == 1);
    assert(last.find("{\"summary\":{\"frames\":2,\"ambientFrames\":0,") == 0);
    assert(last.find("\"histogramEdgesMs\":[8,12,15,17,20,25,33,50,100],"
                     "\"histogram\":[0,0,0,1,0,0,0,0,0,0]}}") != std::string::npos);
    std::remove(path.c_str());

    bool refused = false;
    try {
        FrameLog("/nonexistent-directory/perf.jsonl");
    } catch (const std::exception &) {
        refused = true;
    }
    assert(refused);
}

void summarisesInputLatency() {
    FrameLog log("");
    // Frames that show no input (negative) are not counted; 1..20 ms are.
    for (int i = 0; i < 30; ++i) {
        FrameStats frame;
        frame.inputMs = i < 20 ? i + 1.0 : -1;
        log.record(i * 16.7, 16.7, 1, frame);
    }
    FrameSummary s = log.summary();
    assert(s.inputFrames == 20);
    assert(s.inputP50 == 10 && s.inputP95 == 19 && s.inputMax == 20);
    FrameSummary none = FrameLog("").summary();
    assert(none.inputFrames == 0 && none.inputP95 == 0 && none.inputMax == 0);

    const std::string path = "frame_loop_input_test.jsonl";
    {
        FrameLog file(path);
        FrameStats frame;
        frame.inputMs = 7.5;
        file.record(0, -1, 1, frame);
        file.finish();
    }
    const std::string text = readAll(path);
    assert(text.find("\"inputMs\":7.50}") != std::string::npos);
    assert(text.find("\"inputFrames\":1,\"inputP50Ms\":7.50,\"inputP95Ms\":7.50,"
                     "\"inputMaxMs\":7.50,") != std::string::npos);
    std::remove(path.c_str());
}
} // namespace

int main() {
    fixedClockRepeats();
    realClockSkipsPausedTime();
    summarisesIntervals();
    percentilesUseNearestRank();
    writesLogFile();
    summarisesInputLatency();
    return 0;
}
