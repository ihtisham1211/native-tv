// What a TV is called before its owner names it, from the model it reports.
#include "core/json.hpp"
#include "domain/names.hpp"
#include <cstdio>
#include <dirent.h>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>

using namespace nativetv;
namespace {
int checks = 0;
void check(bool condition, const std::string &message) {
    ++checks;
    if (!condition)
        throw std::runtime_error(message);
}
void expect(const std::string &model, const std::string &name) {
    std::string got = names::tvDefaultName(model);
    check(got == name, "'" + model + "' is named '" + got + "', not '" + name + "'");
}
} // namespace

int main() {
    try {
        // The series, by its letter and year, for the OLED line.
        expect("OLED65B3PUA", "B3");
        expect("OLED55C3PUA", "C3");
        expect("OLED65G4WUA", "G4");
        expect("OLED65G4", "G4");
        expect("OLED42C24LA", "C2");
        expect("OLED48A29LA", "A2");
        expect("OLED77CXAUA", "CX");
        expect("OLED65B8SLC", "B8");
        expect("OLED55C4PUA.DUSQLJR", "C4");
        expect("OLED65Z3PUA", "Z3");
        expect("LG OLED55C4", "C4");
        expect("oled65b3pua", "B3");
        // Other series keep their letters and number.
        expect("55UQ8000PUA", "UQ8000");
        expect("50UP7000PUA", "UP7000");
        expect("43UR8000PUA", "UR8000");
        expect("65QNED85UQA", "QNED85");
        expect("65NANO856PA", "NANO856");
        expect("55UH850T-UA", "UH850");
        expect("43LM6300PUB", "LM6300");
        // No model, or one that says nothing about the series.
        for (const char *unknown : {"", "   ", "webOS TV", "LG webOS TV", "LG", "HDMI", "OLED", "12345", "OLEDPUA"})
            expect(unknown, "LGTV");
        // Every real model in the fixtures is named after its series.
        const std::map<std::string, std::string> expected = {
            {"stats-b8-webos4.json", "B8"},       {"stats-c4-webos9.json", "C4"},
            {"stats-c4-webos9-hdr.json", "C4"},   {"stats-c4-webos9-standby.json", "C4"},
            {"stats-cx-webos5.json", "CX"},       {"stats-cx-webos5-hdr.json", "CX"},
            {"stats-g4-webos9.json", "G4"}};
        const std::string directory = NATIVETV_TV_FIXTURES;
        DIR *folder = opendir(directory.c_str());
        check(folder != nullptr, "the fixtures folder opens");
        size_t seen = 0;
        while (dirent *entry = readdir(folder)) {
            std::string file = entry->d_name;
            if (file.compare(0, 6, "stats-") != 0)
                continue;
            std::ifstream stream(directory + "/" + file);
            std::stringstream text;
            text << stream.rdbuf();
            const Json stats = Json::parse(text.str());
            check(stats["device"]["model"].isString(), file + " has a model");
            auto found = expected.find(file);
            check(found != expected.end(), file + " is a new fixture: say what its TV is called");
            expect(stats["device"]["model"].string(), found->second);
            ++seen;
        }
        closedir(folder);
        check(seen == expected.size(), "every expected fixture was read");
    } catch (const std::exception &error) {
        std::fprintf(stderr, "tv_name: %s\n", error.what());
        return 1;
    }
    std::printf("tv_name: %d checks passed\n", checks);
    return 0;
}
