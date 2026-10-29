#include "daemon/logger.hpp"
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

using namespace nativetv;
using namespace nativetv::daemon;

namespace {
void check(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}

struct SinkLine {
    LogLevel level;
    std::string line;
    bool flushed;
};

void testLevelPrecedenceAndFiltering() {
    std::vector<SinkLine> output;
    LoggerOptions options;
    options.environmentLevel = "debug";
    options.configLevel = "quiet";
    options.sink = [&output](LogLevel level, const std::string &line, bool flush) {
        output.push_back({level, line, flush});
    };
    Logger logger(options);
    logger.debug("diagnostic");
    logger.info("ready");
    logger.warning("slow response");
    logger.error("request failed");
    check(logger.activeLevel() == "debug" && output.size() == 4,
          "environment verbosity should override the config file");
    check(output[0].flushed && !output[1].flushed && !output[2].flushed,
          "debug lines should flush immediately");

    LoggerOptions quietOptions;
    quietOptions.environmentLevel = "quiet";
    quietOptions.configLevel = "quiet";
    quietOptions.sink = [&output](LogLevel level, const std::string &line, bool flush) {
        output.push_back({level, line, flush});
    };
    Logger quiet(quietOptions);
    const size_t before = output.size();
    quiet.info("hidden");
    quiet.debug("hidden too");
    quiet.warning("retained");
    check(output.size() == before + 1 && quiet.size() == 1,
          "quiet should retain warnings/errors but suppress info/debug");
}

void testBoundedRingAndQuery() {
    LoggerOptions options;
    options.configLevel = "info";
    options.ringCapacity = 3;
    options.process = "daemon-test";
    options.sink = [](LogLevel, const std::string &, bool) {};
    Logger logger(options);
    logger.info("first");
    logger.info("match second");
    logger.warning("third");
    logger.error("fourth");
    check(logger.size() == 3, "ring should discard its oldest entry at capacity");

    std::vector<LogRecord> latest = logger.snapshot(2);
    check(latest.size() == 2 && latest[0].message == "third" &&
              latest[1].message == "fourth",
          "snapshot should return the newest records in chronological order");
    std::vector<LogRecord> filtered = logger.snapshot(100, "MATCH SECOND");
    check(filtered.size() == 1 && filtered[0].message == "match second",
          "snapshot should apply a case-insensitive filter");
    Json json = logger.snapshotJson(1);
    check(json.isArray() && json.size() == 1 &&
              json[0]["source"].string() == "nativetv" &&
              json[0]["proc"].string() == "daemon-test" &&
              json[0]["level"].string() == "error" &&
              json[0]["msg"].string() == "fourth",
          "snapshot JSON should expose the /api/logs entry fields");
}

void testMessageBound() {
    LoggerOptions options;
    options.sink = [](LogLevel, const std::string &, bool) {};
    Logger logger(options);
    logger.info(std::string(20 * 1024, 'x'));
    std::vector<LogRecord> entries = logger.snapshot();
    check(entries.size() == 1 && entries[0].message.size() <= 16 * 1024 &&
              entries[0].message.find("[truncated]") != std::string::npos,
          "oversized messages should be truncated to bound ring memory");
}
} // namespace

int main() {
    try {
        testLevelPrecedenceAndFiltering();
        testBoundedRingAndQuery();
        testMessageBound();
        return 0;
    } catch (const std::exception &error) {
        fprintf(stderr, "logger test failed: %s\n", error.what());
        return 1;
    }
}
