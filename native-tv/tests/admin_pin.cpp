#include "service/admin_pin.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace nativetv;
using service::AdminPin;

namespace {
void require(bool condition, const char *message) {
    if (condition)
        return;
    std::fprintf(stderr, "%s\n", message);
    std::exit(1);
}
int64_t fakeNow = 1000;
int64_t testClock() {
    return fakeNow;
}
std::string readAll(const std::string &path) {
    std::ifstream input(path.c_str(), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}
} // namespace

int main() {
    char directory[] = "/tmp/nativetv-admin-pin-XXXXXX";
    require(mkdtemp(directory) != nullptr, "could not create the test directory");
    const std::string file = std::string(directory) + "/admin-pin.json";
    std::string session;
    int retryAfter = 0;

    AdminPin pin(file, testClock);
    require(!pin.configured(), "a PIN was reported before one was set");
    require(pin.unlock("1234", session, retryAfter) == AdminPin::Result::NotSet,
            "unlock without a PIN was not refused as NotSet");
    require(!pin.set("123") && !pin.set("12345") && !pin.set("12a4") && !pin.set(""),
            "a PIN that is not four digits was accepted");
    require(pin.set("4821") && pin.configured(), "a four-digit PIN was not stored");

    // The PIN is not in the file, and only the owner can read it.
    const std::string stored = readAll(file);
    require(stored.find("4821") == std::string::npos, "the PIN was written in clear");
    struct stat info;
    require(stat(file.c_str(), &info) == 0 && (info.st_mode & 077) == 0, "the PIN file is not owner-only");

    require(pin.unlock("0000", session, retryAfter) == AdminPin::Result::Wrong && session.empty(),
            "a wrong PIN opened a session");
    require(pin.unlock("4821", session, retryAfter) == AdminPin::Result::Ok && session.size() == 32,
            "the right PIN did not open a session");
    require(pin.sessionValid(session) && !pin.sessionValid("") && !pin.sessionValid("nope"),
            "session check wrong");

    // A use keeps a session open; ten idle minutes close it.
    fakeNow += 500;
    require(pin.sessionValid(session), "a session closed before its ten minutes");
    fakeNow += 500;
    require(pin.sessionValid(session), "a use did not keep the session open");
    fakeNow += 601;
    require(!pin.sessionValid(session), "an idle session stayed open");

    // Five misses lock it out, even for the right PIN, and each lock is twice the last.
    for (int miss = 0; miss < 4; ++miss)
        require(pin.unlock("1111", session, retryAfter) == AdminPin::Result::Wrong && retryAfter == 0,
                "a miss before the fifth locked");
    require(pin.unlock("1111", session, retryAfter) == AdminPin::Result::Wrong && retryAfter == 30,
            "the fifth miss did not lock for 30 seconds");
    require(pin.unlock("4821", session, retryAfter) == AdminPin::Result::Locked && retryAfter == 30,
            "the right PIN got in during a lock-out");
    fakeNow += 31;
    require(pin.unlock("1111", session, retryAfter) == AdminPin::Result::Wrong && retryAfter == 60,
            "the next lock-out was not doubled");
    fakeNow += 61;
    require(pin.unlock("4821", session, retryAfter) == AdminPin::Result::Ok,
            "the right PIN was refused after a lock-out ended");
    require(pin.unlock("1111", session, retryAfter) == AdminPin::Result::Wrong && retryAfter == 0,
            "a success did not reset the count of misses");

    // A new PIN ends every session; the file outlives the process.
    std::string other;
    require(pin.unlock("4821", other, retryAfter) == AdminPin::Result::Ok, "unlock failed");
    require(pin.set("9076") && !pin.sessionValid(other), "changing the PIN left a session open");
    AdminPin restarted(file, testClock);
    require(restarted.configured() && restarted.unlock("9076", session, retryAfter) == AdminPin::Result::Ok &&
                restarted.unlock("4821", session, retryAfter) == AdminPin::Result::Wrong,
            "the PIN was not kept across a restart");

    // lock() ends one session; clear() removes the PIN.
    require(restarted.unlock("9076", session, retryAfter) == AdminPin::Result::Ok, "unlock failed");
    restarted.lock(session);
    require(!restarted.sessionValid(session), "lock left the session open");
    require(restarted.clear() && !restarted.configured() && access(file.c_str(), F_OK) != 0,
            "clear did not remove the PIN");

    unlink(file.c_str());
    rmdir(directory);
    return 0;
}
