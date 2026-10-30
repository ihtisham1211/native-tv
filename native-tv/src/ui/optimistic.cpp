#include "ui/optimistic.hpp"
#include "domain/say.hpp"
#include <algorithm>
#include <cmath>

namespace nativetv {
namespace {
constexpr unsigned kReadBacks = 3; // a failed read-back is asked again this many times
}

Optimistic::Optimistic(Services &services, Clock clock, Toast toast, Settings settings)
    : services_(services), clock_(std::move(clock)), toast_(std::move(toast)), settings_(settings) {}

bool Optimistic::same(const Json &a, const Json &b) {
    if (!a.valid() || !b.valid())
        return a.valid() == b.valid();
    if (a.isNumber() && b.isNumber())
        return std::fabs(a.number() - b.number()) < 1e-9;
    if (a.isBoolean() && b.isBoolean())
        return a.boolean() == b.boolean();
    if (a.isString() && b.isString())
        return a.string() == b.string();
    return a.dump() == b.dump();
}
std::string Optimistic::channelOf(const Entry &entry) const {
    return "w:" + entry.id;
}
Optimistic::Entry &Optimistic::entry(const std::string &id) {
    Entry &found = entries_[id];
    found.id = id;
    return found;
}
void Optimistic::erase(const std::string &id) {
    auto found = entries_.find(id);
    if (found == entries_.end())
        return;
    for (const auto &key : found->second.keys) {
        auto owner = owner_.find(key);
        if (owner != owner_.end() && owner->second == id)
            owner_.erase(owner);
    }
    entries_.erase(found);
}
void Optimistic::clear() {
    entries_.clear();
    owner_.clear();
}

// ---- What a page draws ------------------------------------------------------------------------

Json Optimistic::shown(const std::string &key, const Json &server) {
    auto owner = owner_.find(key);
    if (owner == owner_.end())
        return server;
    auto found = entries_.find(owner->second);
    if (found == entries_.end() || found->second.action)
        return server;
    Entry &held = found->second;
    const Json intended = held.intended[key];
    if (!held.writing) {
        // The TV took it. The overlay stays until the server shows it too, or until the read-back
        // has brought the TV's own answer, which then wins.
        if (held.rereadDone) {
            std::string id = held.id;
            erase(id);
            return server;
        }
        if (same(server, intended)) {
            // This key has caught up; a batch keeps holding the others.
            std::string id = held.id;
            held.keys.erase(std::remove(held.keys.begin(), held.keys.end(), key), held.keys.end());
            held.intended.erase(key);
            held.confirmed.erase(key);
            held.sent.erase(key);
            owner_.erase(key);
            if (held.keys.empty())
                erase(id);
            return server;
        }
    }
    return intended;
}
double Optimistic::number(const std::string &key, double server) {
    return shown(key, Json::numeric(server)).number(server);
}
bool Optimistic::flag(const std::string &key, bool server) {
    return shown(key, Json::flag(server)).boolean(server);
}
std::string Optimistic::text(const std::string &key, const std::string &server) {
    return shown(key, Json::text(server)).string(server);
}
bool Optimistic::pending(const std::string &key) const {
    auto owner = owner_.find(key);
    if (owner == owner_.end())
        return false;
    auto found = entries_.find(owner->second);
    return found != entries_.end() && found->second.writing;
}
bool Optimistic::active() const {
    for (const auto &item : entries_) {
        const Entry &held = item.second;
        if (held.writing || (!held.rereadSent && held.write.readOperation != Operation::None))
            return true;
    }
    return false;
}

// ---- What a page does -------------------------------------------------------------------------

void Optimistic::touch(Entry &held, double now) {
    held.writing = true;
    if (!held.dirty && !held.inFlight)
        held.dirtySince = now;
    held.dirty = true;
    held.rereadSent = held.rereadDone = false;
    held.rereads = 0;
    held.doneCalled = false;
    if (now >= held.sentAt + settings_.gapMs && (!held.write.relative || !held.inFlight))
        send(held, now);
}
void Optimistic::set(const std::string &key, const Json &value, const Json &server, const Write &write) {
    apply(key, {{key, value}}, {{key, server}}, write, false);
}
void Optimistic::setBatch(const std::string &batch, const std::vector<std::pair<std::string, Json>> &values,
                          const std::vector<std::pair<std::string, Json>> &servers, const Write &write) {
    apply(batch, values, servers, write, true);
}
void Optimistic::apply(const std::string &batch, const std::vector<std::pair<std::string, Json>> &values,
                       const std::vector<std::pair<std::string, Json>> &servers, const Write &write,
                       bool isBatch) {
    const double now = clock_();
    auto existing = entries_.find(batch);
    if (existing == entries_.end()) {
        // Nothing to tell when nothing differs from what the page already shows.
        bool differs = false;
        for (size_t i = 0; i < values.size(); ++i)
            differs = differs || i >= servers.size() || !same(values[i].second, servers[i].second);
        if (!differs)
            return;
    }
    Entry &held = entry(batch);
    held.batch = isBatch;
    held.action = false;
    for (size_t i = 0; i < values.size(); ++i) {
        const std::string &key = values[i].first;
        auto owner = owner_.find(key);
        if (owner != owner_.end() && owner->second != batch) {
            // The key moves to this change: the one that had it keeps the rest.
            std::string previous = owner->second;
            auto old = entries_.find(previous);
            if (old != entries_.end()) {
                auto &keys = old->second.keys;
                keys.erase(std::remove(keys.begin(), keys.end(), key), keys.end());
                old->second.intended.erase(key);
                old->second.confirmed.erase(key);
                if (keys.empty())
                    erase(previous);
            }
        }
        if (!held.intended.count(key)) {
            held.keys.push_back(key);
            held.confirmed[key] = i < servers.size() ? servers[i].second : Json();
        }
        held.intended[key] = values[i].second;
        owner_[key] = batch;
    }
    held.write = write;
    touch(held, now);
}
void Optimistic::commit(const std::string &key) {
    auto owner = owner_.find(key);
    if (owner == owner_.end())
        return;
    auto found = entries_.find(owner->second);
    if (found == entries_.end())
        return;
    Entry &held = found->second;
    if (held.dirty && (!held.write.relative || !held.inFlight))
        send(held, clock_());
}
bool Optimistic::run(const std::string &key, const Write &write) {
    auto found = entries_.find(key);
    if (found != entries_.end() && found->second.writing)
        return false;
    erase(key);
    Entry &held = entry(key);
    held.action = true;
    held.keys = {key};
    held.intended[key] = Json::flag(true);
    held.confirmed[key] = Json::flag(false);
    held.write = write;
    owner_[key] = key;
    touch(held, clock_());
    return true;
}

// ---- Writing ----------------------------------------------------------------------------------

void Optimistic::send(Entry &held, double now) {
    Json target;
    if (held.batch) {
        target = Json::object();
        for (const auto &key : held.keys)
            target.set(key, held.intended[key]);
    } else
        target = held.intended[held.keys[0]];
    if (held.write.relative) {
        double net = target.number() - held.confirmed[held.keys[0]].number();
        if (net == 0) {
            // Back where the TV is: nothing to say.
            held.dirty = false;
            if (!held.inFlight)
                settle(held, true, Json(), now);
            return;
        }
        target = Json::numeric(net);
    }
    Json body = held.write.body ? held.write.body(target) : Json::object();
    unsigned sequence = services_.write(channelOf(held), held.write.operation, body);
    held.sent = held.intended;
    held.sentAt = now;
    held.sequence = sequence;
    if (sequence) {
        if (!held.inFlight)
            held.waitingSince = now;
        held.inFlight = true;
        held.dirty = false;
    } else
        held.dirty = true; // the transport did not take it: the next interval tries again
}
void Optimistic::settle(Entry &held, bool ok, const Json &reply, double now) {
    held.writing = false;
    held.confirmedAt = now;
    held.readAt = now + settings_.reconcileMs;
    std::function<void(bool, const Json &)> done = held.doneCalled ? nullptr : held.write.done;
    held.doneCalled = true;
    if (held.action) {
        std::string id = held.id;
        erase(id);
    }
    if (done)
        done(ok, reply);
}
void Optimistic::fail(Entry &held, const std::string &reason, const Json &reply, double now) {
    held.intended = held.confirmed;
    held.dirty = held.inFlight = false;
    held.rereadSent = held.rereadDone = false;
    std::string message = held.write.failure.empty() ? say::msg("tv.write.failed", "That did not work")
                                                     : held.write.failure;
    if (toast_)
        toast_(reason.empty() ? message : message + ": " + reason);
    settle(held, false, reply, now);
}
void Optimistic::answered(Entry &held, bool ok, const Json &reply, double now) {
    held.inFlight = false;
    if (ok)
        held.confirmed = held.sent; // the TV took what was sent
    if (held.dirty)
        return; // the person went on: the newest target is still to be sent
    if (ok)
        settle(held, true, reply, now);
    else
        fail(held, reply["error"].string(), reply, now);
}

// ---- The application's side -------------------------------------------------------------------

bool Optimistic::response(const Response &reply) {
    if (reply.channel.compare(0, 2, "w:") == 0) {
        auto found = entries_.find(reply.channel.substr(2));
        // An entry that has gone (it timed out) or a reply to an older write says nothing.
        if (found == entries_.end() || !found->second.inFlight || reply.generation != found->second.sequence)
            return true;
        Json data = reply.json();
        bool ok = reply.ok() && data.valid() && (!data.has("ok") || data["ok"].boolean());
        answered(found->second, ok, data, clock_());
        return true;
    }
    for (auto &item : entries_) {
        Entry &held = item.second;
        if (!held.rereadSent || held.rereadDone || reply.channel != held.write.readChannel ||
            reply.generation < held.rereadGeneration)
            continue;
        if (reply.ok() && reply.json().valid())
            held.rereadDone = true;
        else {
            held.rereadSent = false; // asked again after the next wait, up to kReadBacks times
            held.readAt = clock_() + settings_.reconcileMs;
        }
    }
    return false;
}
bool Optimistic::tick() {
    const double now = clock_();
    bool changed = false;
    std::vector<std::string> ids;
    for (const auto &item : entries_)
        ids.push_back(item.first);
    for (const std::string &id : ids) {
        auto found = entries_.find(id);
        if (found == entries_.end())
            continue;
        Entry &held = found->second;
        if (held.writing) {
            const std::string silence = say::msg("tv.write.timeout", "the TV did not answer");
            if (held.inFlight && now - held.waitingSince >= settings_.timeoutMs) {
                fail(held, silence, Json(), now);
                changed = true;
            } else if (held.dirty && !held.inFlight && now - held.dirtySince >= settings_.timeoutMs) {
                fail(held, silence, Json(), now);
                changed = true;
            } else if (held.dirty && now >= held.sentAt + settings_.gapMs &&
                       (!held.write.relative || !held.inFlight)) {
                send(held, now);
                changed = true;
            }
            continue;
        }
        if (held.rereadDone || now >= held.confirmedAt + settings_.holdMs) {
            erase(id);
            changed = true;
            continue;
        }
        if (!held.rereadSent && held.write.readOperation != Operation::None && held.rereads < kReadBacks &&
            now >= held.readAt) {
            unsigned generation = services_.read(held.write.readChannel, held.write.readOperation,
                                                 held.write.readArguments);
            if (generation) {
                held.rereadSent = true;
                held.rereadGeneration = generation;
            }
            ++held.rereads;
            if (!held.rereadSent)
                held.readAt = now + settings_.reconcileMs;
        }
    }
    return changed;
}
} // namespace nativetv
