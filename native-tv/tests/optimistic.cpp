// The optimistic-update layer on its own: a fake Services whose replies are held back, reordered
// and refused by the test, and a clock the test moves.
#include "ui/optimistic.hpp"
#include <algorithm>
#include <cstdio>
#include <deque>
#include <map>
#include <stdexcept>

using namespace nativetv;
namespace {
int checks = 0;
void check(bool condition, const char *message) {
    ++checks;
    if (!condition)
        throw std::runtime_error(message);
}

struct Fake final : Services {
    struct Sent {
        std::string channel;
        Operation operation;
        Json body;
        unsigned generation;
        double at;
    };
    std::vector<Sent> writes, reads;
    std::map<std::string, unsigned> revisions;
    bool refuse = false;
    double *clock = nullptr;
    unsigned read(const std::string &channel, Operation operation, const Json &) override {
        reads.push_back({channel, operation, Json(), ++revisions[channel], clock ? *clock : 0});
        return revisions[channel];
    }
    unsigned write(const std::string &channel, Operation operation, const Json &body) override {
        if (refuse)
            return 0;
        writes.push_back({channel, operation, body, ++revisions[channel], clock ? *clock : 0});
        return revisions[channel];
    }
    void cancel(const std::string &channel) override {
        ++revisions[channel];
    }
};

struct World {
    double now = 0;
    Fake fake;
    std::vector<std::string> toasts;
    Optimistic layer;
    World()
        : layer(fake, [this] { return now; }, [this](const std::string &text) { toasts.push_back(text); }) {
        fake.clock = &now;
    }
    void advance(double ms) {
        now += ms;
        layer.tick();
    }
    // The reply to a write the fake has taken: delivered when the test says.
    void reply(const Fake::Sent &sent, bool ok = true, const std::string &error = "") {
        Response response;
        response.channel = sent.channel;
        response.generation = sent.generation;
        response.status = ok ? 200 : 503;
        response.bytes = ok ? R"({"ok":true})" : std::string(R"({"ok":false,"error":")") + error + "\"}";
        layer.response(response);
    }
    void readReply(const Fake::Sent &read, const std::string &json = R"({"ok":true})") {
        Response response;
        response.channel = read.channel;
        response.generation = read.generation;
        response.status = 200;
        response.bytes = json;
        layer.response(response);
    }
};

Write absolute(const char *action) {
    Write write;
    write.operation = Operation::Control;
    std::string name = action;
    write.body = [name](const Json &target) {
        Json body = Json::object();
        body.set("action", Json::text(name));
        body.set("value", target);
        return body;
    };
    write.readChannel = "stats";
    write.readOperation = Operation::Metrics;
    return write;
}
Write relativeSteps() {
    Write write = absolute("volumeStep");
    write.relative = true;
    return write;
}
double sentValue(const Fake::Sent &sent) {
    return sent.body["value"].number();
}
} // namespace

// (a) The value shows in the very next frame, and stays whatever the TV does or does not say.
void shownAtOnce() {
    World world;
    check(world.layer.number("volume", 24) == 24, "Without a change the server's value shows");
    world.now = 1000;
    world.layer.set("volume", Json::numeric(25), Json::numeric(24), absolute("volume"));
    check(world.layer.number("volume", 24) == 25 && world.layer.pending("volume"),
          "The new value shows the moment it is set, pending");
    check(world.fake.writes.size() == 1 && sentValue(world.fake.writes[0]) == 25,
          "The first write leaves at once");
    world.advance(3000);
    check(world.layer.number("volume", 24) == 25, "With no reply at all the value stays");
    check(world.layer.active(), "A pending write keeps the page ticking");
    check(world.layer.text("mode", "Cinema") == "Cinema" && world.layer.flag("on", true),
          "Typed readers fall back to the server's value");
    // An unchanged value is not a change.
    World same;
    same.layer.set("volume", Json::numeric(24), Json::numeric(24), absolute("volume"));
    check(same.fake.writes.empty() && !same.layer.pending("volume"), "Setting what is shown writes nothing");
}

// (b) Held key: many writes, replies late and out of order, stale reads, and the value only moves
// the way the key does.
void heldKey(bool up) {
    World world;
    const int from = up ? 5 : 95, to = up ? 95 : 5, direction = up ? 1 : -1;
    double staleServer = from; // what the page last read from the TV
    double previous = from;
    int value = from;
    std::deque<Fake::Sent> unanswered;
    size_t answeredUpTo = 0;
    int step = 0;
    while (value != to) {
        value += direction;
        world.now += 30; // a repeat every 30 ms
        world.layer.set("volume", Json::numeric(value), Json::numeric(from), absolute("volume"));
        world.layer.tick();
        // Late and out of order: every seventh repeat the replies that have piled up come back
        // newest first, and a stale read of an intermediate value reaches the page.
        for (; answeredUpTo < world.fake.writes.size(); ++answeredUpTo)
            unanswered.push_back(world.fake.writes[answeredUpTo]);
        if (++step % 7 == 0) {
            staleServer = from + direction * (step / 2);
            while (!unanswered.empty()) {
                world.reply(unanswered.back());
                unanswered.pop_back();
            }
        }
        double shown = world.layer.number("volume", staleServer);
        check(up ? shown >= previous : shown <= previous, "The shown value never goes backwards while keys repeat");
        check(shown == value, "The shown value is the one the key reached");
        previous = shown;
    }
    // Let go: the final target is still sent, within one interval, and every reply that comes
    // late changes nothing.
    size_t before = world.fake.writes.size();
    world.advance(130);
    check(world.fake.writes.size() == before + 1 || world.fake.writes.size() == before,
          "At most the trailing write follows");
    check(sentValue(world.fake.writes.back()) == to, "The last write carries the final value");
    check(world.fake.writes.size() <= static_cast<size_t>(90 * 30 / 120) + 3,
          "Writes are limited to one per interval");
    for (size_t i = 1; i < world.fake.writes.size(); ++i)
        check(world.fake.writes[i].at - world.fake.writes[i - 1].at >= 120 - 1e-6,
              "Two writes are never closer than the interval");
    // Every reply comes, the older ones first and the newest last: the older ones change nothing.
    std::vector<Fake::Sent> all = world.fake.writes;
    for (size_t i = 0; i + 1 < all.size(); ++i)
        world.reply(all[i]);
    check(world.layer.number("volume", staleServer) == to, "Late replies to older writes change nothing");
    world.reply(all.back());
    check(!world.layer.pending("volume") && world.layer.number("volume", staleServer) == to,
          "The newest reply settles it and the value stays");
    check(world.fake.reads.empty(), "No read-back before the wait is over");
    world.advance(800);
    check(world.fake.reads.size() == 1 && world.fake.reads[0].channel == "stats", "One read-back follows");
    // A stale server value still does not win; the read-back's answer does.
    check(world.layer.number("volume", staleServer) == to, "A stale server value does not replace it");
    world.readReply(world.fake.reads[0]);
    check(world.layer.number("volume", up ? 90 : 10) == (up ? 90 : 10),
          "After the read-back the TV's own value, capped or not, is what shows");
    check(!world.layer.active(), "Nothing is left pending");
}

// (c) A refusal puts the value back and says so; an older refusal behind a newer write says nothing.
void failure() {
    World world;
    Write write = absolute("volume");
    bool called = false, ok = true;
    write.done = [&](bool result, const Json &) {
        called = true;
        ok = result;
    };
    world.layer.set("volume", Json::numeric(30), Json::numeric(24), write);
    world.reply(world.fake.writes[0], false, "Volume locked");
    check(world.layer.number("volume", 24) == 24 && !world.layer.pending("volume"),
          "A refusal brings the value back");
    check(world.toasts.size() == 1 && world.toasts[0] == "That did not work: Volume locked",
          "It says so, with the reason");
    check(called && !ok, "The page hears it");
    // An older write that is refused while a newer one waits.
    World busy;
    busy.layer.set("volume", Json::numeric(30), Json::numeric(24), absolute("volume"));
    busy.now += 30;
    busy.layer.set("volume", Json::numeric(31), Json::numeric(24), absolute("volume")); // waits: inside the interval
    busy.reply(busy.fake.writes[0], false, "Busy");
    check(busy.layer.number("volume", 24) == 31 && busy.toasts.empty(),
          "A refusal of an older write while a newer target waits is not shown");
    busy.advance(130);
    check(busy.fake.writes.size() == 2 && sentValue(busy.fake.writes[1]) == 31, "The newer target is sent");
    busy.reply(busy.fake.writes[1], false, "Locked");
    check(busy.layer.number("volume", 24) == 24 && busy.toasts.size() == 1,
          "The newest write's refusal goes back to the last value the TV took (none: 24)");
    // After one accepted write a refusal goes back to the accepted value, not to the server's.
    World accepted;
    accepted.layer.set("light", Json::numeric(50), Json::numeric(40), absolute("backlight"));
    accepted.reply(accepted.fake.writes[0]);
    accepted.now += 130;
    accepted.layer.set("light", Json::numeric(60), Json::numeric(40), absolute("backlight"));
    accepted.reply(accepted.fake.writes[1], false, "No");
    check(accepted.layer.number("light", 40) == 50, "A later refusal goes back to what the TV last accepted");
}

// (d) Silence is a failure too.
void timeout() {
    World world;
    world.layer.set("volume", Json::numeric(30), Json::numeric(24), absolute("volume"));
    world.advance(7900);
    check(world.layer.number("volume", 24) == 30, "Within the time the value holds");
    world.advance(200);
    check(world.layer.number("volume", 24) == 24 && world.toasts.size() == 1 &&
              world.toasts[0] == "That did not work: the TV did not answer",
          "Without an answer the value goes back after eight seconds and says so");
    // A write that says it may take longer is given that long: a service of the TV takes 12 s to stop.
    World slow;
    Write patient = absolute("volume");
    patient.timeoutMs = 30000;
    slow.layer.set("volume", Json::numeric(30), Json::numeric(24), patient);
    slow.advance(12500);
    check(slow.layer.number("volume", 24) == 30 && slow.toasts.empty(),
          "A write with its own, longer time is still waited for after twelve seconds");
    slow.advance(18000);
    check(slow.layer.number("volume", 24) == 24 && slow.toasts.size() == 1,
          "and it too fails when that time has passed");
    // The transport that never takes a write is the same.
    World refused;
    refused.fake.refuse = true;
    refused.layer.set("volume", Json::numeric(30), Json::numeric(24), absolute("volume"));
    refused.advance(500);
    check(refused.layer.number("volume", 24) == 30 && refused.layer.pending("volume"),
          "A write the transport did not take stays pending");
    refused.fake.refuse = false;
    refused.advance(130);
    check(refused.fake.writes.size() == 1, "It is sent when the transport can");
    World never;
    never.fake.refuse = true;
    never.layer.set("volume", Json::numeric(30), Json::numeric(24), absolute("volume"));
    never.advance(8100);
    check(never.layer.number("volume", 24) == 24 && never.toasts.size() == 1, "A write never taken goes back too");
}

// (e) What is read while a write is pending, or before the read-back, does not overwrite.
void readsDoNotOverwrite() {
    World world;
    world.layer.set("volume", Json::numeric(30), Json::numeric(24), absolute("volume"));
    check(world.layer.number("volume", 77) == 30, "A read that arrives while the write is pending does not overwrite");
    world.reply(world.fake.writes[0]);
    check(world.layer.number("volume", 24) == 30, "Nor the old value after the TV has answered");
    check(world.layer.number("volume", 30) == 30, "A server that agrees ends the hold");
    // Read-back lands on a value the TV changed.
    World capped;
    capped.layer.set("volume", Json::numeric(100), Json::numeric(90), absolute("volume"));
    capped.reply(capped.fake.writes[0]);
    capped.advance(800);
    check(capped.fake.reads.size() == 1, "The read-back is asked for");
    check(capped.layer.number("volume", 90) == 100, "Until it lands the hold stays");
    capped.readReply(capped.fake.reads[0]);
    check(capped.layer.number("volume", 95) == 95, "The TV's answer wins over the wanted value");
    // A read that fails is asked for again, and the hold lasts meanwhile.
    World failing;
    failing.layer.set("volume", Json::numeric(30), Json::numeric(24), absolute("volume"));
    failing.reply(failing.fake.writes[0]);
    failing.advance(800);
    Response bad;
    bad.channel = "stats";
    bad.generation = failing.fake.reads[0].generation;
    bad.status = 500;
    failing.layer.response(bad);
    check(failing.layer.number("volume", 24) == 30, "A failed read-back keeps the value");
    failing.advance(800);
    check(failing.fake.reads.size() == 2, "The read-back is asked again");
    // A read-back is only the one asked for after the last answer.
    World late;
    late.layer.set("volume", Json::numeric(30), Json::numeric(24), absolute("volume"));
    Response early;
    early.channel = "stats";
    early.generation = 1;
    early.status = 200;
    early.bytes = R"({"ok":true})";
    late.layer.response(early);
    late.reply(late.fake.writes[0]);
    check(late.layer.number("volume", 24) == 30, "A read that came before the answer is not the read-back");
}

// (f) Coalescing.
void coalescing() {
    World world;
    for (int i = 1; i <= 100; ++i) {
        world.now += 10;
        world.layer.set("volume", Json::numeric(24 + i), Json::numeric(24), absolute("volume"));
        world.layer.tick();
    }
    check(world.fake.writes.size() <= 9, "A thousand milliseconds of repeats make at most nine writes");
    check(sentValue(world.fake.writes.front()) == 25, "The first goes at once");
    check(world.layer.number("volume", 24) == 124, "The shown value is the last one");
    world.layer.commit("volume");
    check(sentValue(world.fake.writes.back()) == 124, "Commit sends the last value now");
    size_t count = world.fake.writes.size();
    world.layer.commit("volume");
    world.advance(500);
    check(world.fake.writes.size() == count, "Nothing more is sent once the last value is out");
    // Without commit the last value goes by itself within one interval.
    World trailing;
    trailing.layer.set("volume", Json::numeric(30), Json::numeric(24), absolute("volume"));
    trailing.now += 10;
    trailing.layer.set("volume", Json::numeric(31), Json::numeric(24), absolute("volume"));
    check(trailing.fake.writes.size() == 1, "A second change inside the interval waits");
    trailing.advance(120);
    check(trailing.fake.writes.size() == 2 && sentValue(trailing.fake.writes[1]) == 31, "It goes when the interval is over");
}

// Routes that only take a change: the net sum, one at a time.
void relativeWrites() {
    World world;
    world.layer.set("volume", Json::numeric(25), Json::numeric(24), relativeSteps());
    check(world.fake.writes.size() == 1 && sentValue(world.fake.writes[0]) == 1, "The first step goes as +1");
    for (int i = 26; i <= 29; ++i) {
        world.now += 40;
        world.layer.set("volume", Json::numeric(i), Json::numeric(24), relativeSteps());
        world.layer.tick();
    }
    check(world.fake.writes.size() == 1 && world.layer.number("volume", 24) == 29,
          "Nothing more goes while the step is on its way");
    world.reply(world.fake.writes[0]);
    world.advance(0);
    check(world.fake.writes.size() == 2 && sentValue(world.fake.writes[1]) == 4, "The net four go as one");
    world.reply(world.fake.writes[1]);
    check(!world.layer.pending("volume"), "Settled");
    // Going back where the TV is sends nothing.
    World back;
    back.layer.set("volume", Json::numeric(25), Json::numeric(24), relativeSteps());
    back.now += 40;
    back.layer.set("volume", Json::numeric(24), Json::numeric(24), relativeSteps());
    back.reply(back.fake.writes[0]);
    back.advance(130);
    check(back.fake.writes.size() == 2 && sentValue(back.fake.writes[1]) == -1,
          "A step the other way goes as -1 against what the TV took");
}

// Several keys changed by one action.
void batches() {
    World world;
    Write write;
    write.operation = Operation::SetHomeLayout;
    write.body = [](const Json &target) { return target; };
    world.layer.setBatch("layout", {{"showLive", Json::flag(true)}, {"showInputs", Json::flag(false)}},
                         {{"showLive", Json::flag(false)}, {"showInputs", Json::flag(true)}}, write);
    check(world.fake.writes.size() == 1 && world.fake.writes[0].body["showLive"].boolean() &&
              !world.fake.writes[0].body["showInputs"].boolean(),
          "A batch is one write with every key");
    check(world.layer.flag("showLive", false) && !world.layer.flag("showInputs", true) &&
              world.layer.pending("showLive") && world.layer.pending("showInputs"),
          "Every key of it shows at once");
    world.reply(world.fake.writes[0], false, "Read-only");
    check(!world.layer.flag("showLive", false) && world.layer.flag("showInputs", false) == true,
          "A refusal puts every key back");
    check(world.toasts.size() == 1, "With one message");
    // An accepted batch is held until the server shows all of it.
    World ok;
    ok.layer.setBatch("layout", {{"a", Json::flag(true)}, {"b", Json::flag(true)}},
                      {{"a", Json::flag(false)}, {"b", Json::flag(false)}}, write);
    ok.reply(ok.fake.writes[0]);
    check(ok.layer.flag("a", false) && ok.layer.flag("b", false), "Held after the answer");
    check(ok.layer.flag("a", true), "A converged key ends its hold");
}

// A toggle that reveals sub-rows: the rows follow what is shown, not what the server last said.
void dependentRows() {
    World world;
    auto visibleSubRows = [&](bool serverOn) {
        return world.layer.flag("clock.show", serverOn) ? std::vector<std::string>{"format", "seconds"}
                                                         : std::vector<std::string>{};
    };
    check(visibleSubRows(false).empty(), "Off: no sub-rows");
    Write write = absolute("show");
    world.layer.set("clock.show", Json::flag(true), Json::flag(false), write);
    check(visibleSubRows(false).size() == 2, "The sub-rows appear with the toggle, before the TV has answered");
    world.layer.set("clock.format", Json::text("24h"), Json::text("12h"), write);
    check(world.layer.text("clock.format", "12h") == "24h", "And they can be changed at once too");
}

// Actions without a value.
void actions() {
    World world;
    bool called = false, ok = false;
    Write write;
    write.operation = Operation::Control;
    write.body = [](const Json &) { return Json::object(); };
    write.done = [&](bool result, const Json &) {
        called = true;
        ok = result;
    };
    check(world.layer.run("screen.off", write), "An action starts");
    check(world.layer.pending("screen.off") && !world.layer.run("screen.off", write),
          "It is pending, and the same action is not started twice");
    check(world.fake.writes.size() == 1, "One write");
    world.reply(world.fake.writes[0]);
    check(called && ok && !world.layer.pending("screen.off"), "The reply ends it");
    called = false;
    check(world.layer.run("screen.off", write), "It can be run again");
    world.reply(world.fake.writes[1], false, "Busy");
    check(called && !ok && world.toasts.size() == 1 && world.toasts[0] == "That did not work: Busy",
          "A refused action says so");
    // An older reply never ends a newer run.
    check(world.layer.run("screen.off", write), "A third run");
    check(world.layer.pending("screen.off"), "pending");
    world.reply(world.fake.writes[1], true);
    check(world.layer.pending("screen.off"), "The reply of the run before is ignored");
}

// The layer never fills the page with its own writes: a key moves to the newest change.
void keysMove() {
    World world;
    Write write = absolute("volume");
    world.layer.setBatch("one", {{"x", Json::numeric(1)}}, {{"x", Json::numeric(0)}}, write);
    world.layer.set("x", Json::numeric(2), Json::numeric(0), write);
    check(world.layer.number("x", 0) == 2, "A key that was in one change belongs to the next");
    world.layer.clear();
    check(!world.layer.active() && world.layer.number("x", 7) == 7, "Cleared");
}

int main() {
    try {
        shownAtOnce();
        heldKey(true);
        heldKey(false);
        failure();
        timeout();
        readsDoNotOverwrite();
        coalescing();
        relativeWrites();
        batches();
        dependentRows();
        actions();
        keysMove();
        std::printf("%d native optimistic checks passed\n", checks);
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
