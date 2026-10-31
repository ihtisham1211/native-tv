#pragma once
#include "core/json.hpp"
#include "core/operations.hpp"
#include "core/services.hpp"
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

// The one way a page changes something on the TV or on the server.
//
// A change shows the moment the key is pressed, whatever the TV takes to answer. The page does not
// send anything itself: it tells this layer the value it wants and how to write it, and reads what
// to draw from shown(). The layer
//
//   - shows the wanted value from the next frame and keeps showing it while the write is pending;
//   - sends the newest value at most every `gapMs` (a held key makes one write per interval, never a
//     pile) and the last one however soon, so what the person ended on is what the TV is told;
//   - tags every write with the sequence the transport gave it and ignores the reply of any write
//     but the newest, so a late or out-of-order reply cannot move the value back;
//   - never lets a read of the TV replace the shown value while a write is waiting or on its way;
//   - after the last write has been answered reads the value back once, and lets the TV's own
//     answer win if it differs (a volume the TV capped);
//   - on a refusal, or an answer that does not come within `timeoutMs`, goes back to the last value
//     the TV accepted and says so in a short message.
//
// Writes carry absolute targets where the route takes one, so a newer write simply replaces an older
// one. A route that only takes a change (volume steps) is `relative`: the layer sends the net change
// since the last accepted value, one request at a time.
//
// Callbacks (`Write::body`, `Write::done`) outlive the page that made them: capture the host or
// nothing, never the page.
namespace nativetv {

struct Write {
    Operation operation = Operation::None;
    // The request body for a target. For a batch the target is an object of {key: value}; for a
    // relative write it is the net change as a number.
    std::function<Json(const Json &target)> body;
    bool relative = false;
    // How to read the value back once the last write has been answered; the reply reaches the page
    // by its own channel, as every read does. Leave the operation None for no read-back: `done` must
    // then bring the page's own copy of the value up to date.
    std::string readChannel;
    Operation readOperation = Operation::None;
    Json readArguments;
    // The message when the TV refuses ("That did not work"); the reply's own reason follows it.
    std::string failure;
    // How long this write may go unanswered before it fails; 0 is the layer's own `timeoutMs`. A write
    // that waits on a service of the TV (12 s to stop one) needs more than the usual eight seconds, or
    // the row goes back to a state the TV no longer has.
    double timeoutMs = 0;
    // The newest write was answered (ok) or failed, and nothing newer waits. Called once per
    // settling. On failure the shown value has already gone back.
    std::function<void(bool ok, const Json &reply)> done;
};

struct OptimisticSettings {
    double gapMs = 120;       // at most one write this often per change
    double timeoutMs = 8000;  // an unanswered write fails after this
    double reconcileMs = 700; // the read-back waits this long after the last answer
    double holdMs = 30000;    // an answered value is held at most this long without a read-back
};

class Optimistic {
  public:
    using Settings = OptimisticSettings;
    using Clock = std::function<double()>;
    using Toast = std::function<void(const std::string &)>;
    Optimistic(Services &services, Clock clock, Toast toast, Settings settings = Settings());

    // ---- What a page draws -------------------------------------------------------------------

    // The wanted value while one is held, otherwise what the server last said.
    Json shown(const std::string &key, const Json &server);
    double number(const std::string &key, double server);
    bool flag(const std::string &key, bool server);
    std::string text(const std::string &key, const std::string &server);
    // A write for the key is waiting or on its way: the page may draw it quietly (a dimmed value, a
    // dot). False once the TV has answered.
    bool pending(const std::string &key) const;
    // The page needs frames and ticks: a write waits or is on its way, or a read-back is due.
    bool active() const;

    // ---- What a page does --------------------------------------------------------------------

    // `key` is now `value`. `server` is what the page showed before (where it goes back to if the TV
    // refuses and nothing was accepted yet). The newest `write` replaces the one before it.
    void set(const std::string &key, const Json &value, const Json &server, const Write &write);
    // Several keys changed by one action travel as one write, answered and reverted together. The
    // write's body receives {key: value, ...}.
    void setBatch(const std::string &batch, const std::vector<std::pair<std::string, Json>> &values,
                  const std::vector<std::pair<std::string, Json>> &servers, const Write &write);
    // The key was released or the choice made: send the newest value now, whatever the gap.
    void commit(const std::string &key);
    // An action that has no value to show (screen off, switch input): pending until the reply.
    // `key` names it for pending(); a second run of the same key while one is pending is dropped.
    // Returns false when it was.
    bool run(const std::string &key, const Write &write);

    // ---- What the application does -----------------------------------------------------------

    // Sends what is due, fails what timed out, asks for the read-backs. True when something changed
    // on screen.
    bool tick();
    // Every reply passes through here. True when it answered one of this layer's writes (the page
    // need not see it); read replies are noted and passed on.
    bool response(const Response &response);
    void clear();

  private:
    struct Entry {
        std::string id;
        std::vector<std::string> keys;
        std::map<std::string, Json> intended, confirmed, sent;
        Write write;
        bool batch = false, action = false;
        bool writing = true; // else answered: held until the read-back or the server agrees
        bool dirty = false, inFlight = false, doneCalled = false;
        bool rereadSent = false, rereadDone = false;
        double sentAt = -1e9, dirtySince = 0, waitingSince = 0, confirmedAt = 0, readAt = 0;
        unsigned sequence = 0, rereadGeneration = 0, rereads = 0;
    };
    static bool same(const Json &a, const Json &b);
    std::string channelOf(const Entry &entry) const;
    Entry &entry(const std::string &id);
    void apply(const std::string &id, const std::vector<std::pair<std::string, Json>> &values,
               const std::vector<std::pair<std::string, Json>> &servers, const Write &write, bool batch);
    void erase(const std::string &id);
    void touch(Entry &entry, double now);
    void send(Entry &entry, double now);
    void answered(Entry &entry, bool ok, const Json &reply, double now);
    void fail(Entry &entry, const std::string &reason, const Json &reply, double now);
    void settle(Entry &entry, bool ok, const Json &reply, double now);

    Services &services_;
    Clock clock_;
    Toast toast_;
    Settings settings_;
    std::map<std::string, Entry> entries_;
    std::map<std::string, std::string> owner_; // key -> entry id
};
} // namespace nativetv
