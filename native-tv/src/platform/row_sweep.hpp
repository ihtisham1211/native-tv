#pragma once

#include "core/json.hpp"
#include "ui/screen.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace nativetv {

// A scripted walk through a Settings screen, for the end-to-end cases: it goes row by row and page by
// page, and for every switch and every choice row it presses OK, waits for the write to come back,
// checks the row shows another value, and presses OK until the row shows the first value again. The
// report lists each row with what it showed. It only presses rows the screen itself calls a toggle or
// a choice, and never a row whose title holds one of the words it was told to skip.
//
//   {"sweep": {"skip": ["reset"], "only": [], "confirm": [], "act": [], "press": true, "maxRows": 40,
//              "startAfter": 0, "withinMs": 180000}}
//
// No row is started after `withinMs` of the run, so that the client is not stopped at the end of its time
// with a row in the other state: the case gives it the run's length less what the slowest row needs.
// An action row (Reset the layout, Cancel a scheduled refresher) is pressed once only when its title holds a
// word of `act`, and its report says it was pressed; the case then fails if the client ends on it. A row that asks "press OK again to confirm" (switching off a burn-in protection) is left alone unless its
// title holds a word of `confirm`, which presses OK a second time. `only`, when it has words, leaves every row whose title holds none of them alone. `press: false` only lists the rows. `startAfter` leaves out that many rows from the first, so a long
// screen can be covered in several runs, and `maxRows` stops after that many pressed rows.
class RowSweep {
  public:
    void start(const Json &options, uint32_t now) {
        active_ = true;
        started_ = true;
        phase_ = Phase::Evaluate;
        press_ = options["press"].valid() ? options["press"].boolean() : true;
        maxRows_ = static_cast<int>(options["maxRows"].number(1000));
        toSkip_ = static_cast<int>(options["startAfter"].number(0));
        skip_.clear();
        only_.clear();
        confirm_.clear();
        act_.clear();
        for (const Json &word : options["act"].items())
            act_.push_back(lower(word.string()));
        for (const Json &word : options["confirm"].items())
            confirm_.push_back(lower(word.string()));
        for (const Json &word : options["skip"].items())
            skip_.push_back(lower(word.string()));
        for (const Json &word : options["only"].items())
            only_.push_back(lower(word.string()));
        results_ = Json::array();
        pages_.clear();
        lastAt_ = now;
        startedAt_ = now;
        withinMs_ = options["withinMs"].valid() ? static_cast<uint32_t>(options["withinMs"].number()) : 0;
        wait_ = 600;
        pressed_ = 0;
        stuck_ = 0;
    }
    bool active() const {
        return active_;
    }
    // The sweep has gone through every row (or ran out of pages) `ms` ago: a run that only waits for the
    // sweep can stop, instead of idling to the end of its time.
    bool finishedFor(uint32_t now, uint32_t ms) const {
        return started_ && !active_ && now - endedAt_ >= ms;
    }
    const Json &results() const {
        return results_;
    }

    // Called every turn of the frame loop; the snapshot is only taken when the sweep is due to act, since
    // building it costs a frame's time. `key` sends a key press.
    void step(uint32_t now, const std::function<Json()> &snapshot, const std::function<void(Key)> &key) {
        if (!active_ || now - lastAt_ < wait_)
            return;
        const Json snap = snapshot();
        if (snap["loading"].boolean() || snap["busy"].boolean() || snap["focusPending"].boolean()) {
            if (now - lastAt_ > kPendingLimitMs)
                finishRow("the write did not come back", now), phase_ = Phase::Advance;
            return;
        }
        const std::string zone = snap["zone"].string();
        switch (phase_) {
        case Phase::Evaluate: {
            if (zone != "rows") {
                key(Key::Down);
                tick(now, 500);
                return;
            }
            row_ = Json::object();
            row_.set("group", snap["group"]);
            row_.set("page", snap["page"]);
            row_.set("row", snap["focus"]);
            row_.set("id", snap["focusId"]);
            row_.set("kind", snap["focusKind"]);
            const std::string kind = snap["focusKind"].string();
            const std::string title = lower(snap["focus"].string());
            before_ = state(snap);
            if (kind == "action" && press_ && named(act_, title)) {
                row_.set("acted", Json::flag(true));
                pressedAt_ = now;
                presses_ = 1;
                key(Key::Ok);
                tick(now, kSettleMs);
                phase_ = Phase::Acted;
                return;
            }
            if (kind != "toggle" && kind != "choice") {
                finishRow("not a switch or a choice", now);
                phase_ = Phase::Advance;
                return;
            }
            if (!only_.empty()) {
                bool wanted = false;
                for (const auto &word : only_)
                    wanted = wanted || (!word.empty() && title.find(word) != std::string::npos);
                if (!wanted) {
                    finishRow("not in this run", now);
                    phase_ = Phase::Advance;
                    return;
                }
            }
            for (const auto &word : skip_)
                if (!word.empty() && title.find(word) != std::string::npos) {
                    finishRow("skipped by name", now);
                    phase_ = Phase::Advance;
                    return;
                }
            if (toSkip_ > 0) {
                --toSkip_;
                finishRow("left to another run", now);
                phase_ = Phase::Advance;
                return;
            }
            row_.set("before", Json::text(before_));
            if (press_ && withinMs_ && now - startedAt_ > withinMs_) {
                finishRow("out of time", now);
                phase_ = Phase::Advance;
                return;
            }
            if (!press_ || pressed_ >= maxRows_) {
                finishRow(press_ ? "past maxRows" : "listed only", now);
                phase_ = Phase::Advance;
                return;
            }
            ++pressed_;
            presses_ = 1;
            pressedAt_ = now;
            timeline_.clear();
            key(Key::Ok);
            tick(now, kSettleMs);
            phase_ = Phase::Check;
            return;
        }
        case Phase::Acted: {
            if (snap["confirming"].boolean() && presses_ < 2 && named(confirm_, lower(row_["row"].string()))) {
                ++presses_;
                key(Key::Ok);
                tick(now, kSettleMs);
                return;
            }
            if (snap["confirming"].boolean())
                key(Key::Back);
            finishRow("", now);
            phase_ = Phase::Advance;
            return;
        }
        case Phase::Check: {
            if (snap["confirming"].boolean() && confirmable(row_["row"].string()) && presses_ < 2) {
                ++presses_;
                key(Key::Ok);
                tick(now, kSettleMs);
                return;
            }
            if (snap["confirming"].boolean()) {
                key(Key::Back);
                row_.set("after", Json::text("asks to confirm"));
                finishRow("asks to confirm, left alone", now);
                tick(now, 600);
                phase_ = Phase::Advance;
                return;
            }
            const std::string after = state(snap);
            row_.set("after", Json::text(after));
            if (after == before_) {
                // A slow write (a service that takes seconds to stop) shows its answer late: give the
                // row a few seconds more, and say how long it took when it does change.
                timeline_ += "[+" + std::to_string(now - pressedAt_) + " " + snap["focusValue"].string() + (snap["focusOn"].boolean() ? " on" : " off") + (snap["focusPending"].boolean() ? " pending" : "") + "]";
                if (now - pressedAt_ < kChangeWaitMs) {
                    tick(now, 500);
                    return;
                }
                row_.set("timeline", Json::text(timeline_));
                finishRow("unchanged by OK", now);
                phase_ = Phase::Advance;
                return;
            }
            row_.set("changedAfterMs", Json::numeric(static_cast<double>(now - pressedAt_)));
            phase_ = Phase::Restore;
            tick(now, 100);
            return;
        }
        case Phase::Restore: {
            if (state(snap) == before_) {
                row_.set("restored", Json::flag(true));
                row_.set("presses", Json::numeric(presses_));
                finishRow("", now);
                phase_ = Phase::Advance;
                return;
            }
            if (presses_ >= kMaxPresses) {
                row_.set("restored", Json::flag(false));
                row_.set("last", Json::text(state(snap)));
                finishRow("not back after the most presses", now);
                phase_ = Phase::Advance;
                return;
            }
            ++presses_;
            key(Key::Ok);
            tick(now, kSettleMs);
            return;
        }
        case Phase::Advance: {
            if (snap["focusLast"].boolean()) {
                pages_.insert(snap["page"].string());
                phase_ = Phase::NextPage;
                up_ = 0;
                return;
            }
            const std::string here = snap["focusId"].string();
            stuck_ = here == lastId_ ? stuck_ + 1 : 0;
            lastId_ = here;
            if (stuck_ >= 3) {
                pages_.insert(snap["page"].string());
                phase_ = Phase::NextPage;
                up_ = 0;
                return;
            }
            key(Key::Down);
            tick(now, 450);
            phase_ = Phase::Evaluate;
            return;
        }
        case Phase::NextPage: {
            if (snap["pages"].number() < 2 || up_ > kMaxUp) {
                end(now);
                return;
            }
            if (zone == "rows") {
                key(Key::Up);
                ++up_;
                tick(now, 260);
                return;
            }
            if (zone == "tabs") {
                const std::string was = snap["page"].string();
                if (moved_) {
                    // Right was pressed: a page we have not done is the next one, else this was the last.
                    moved_ = false;
                    if (pages_.count(was)) {
                        end(now);
                        return;
                    }
                    key(Key::Down);
                    tick(now, 700);
                    phase_ = Phase::Evaluate;
                    lastId_.clear();
                    stuck_ = 0;
                    return;
                }
                key(Key::Right);
                moved_ = true;
                tick(now, 900);
                return;
            }
            end(now); // the focus left the page's rows and tabs: nothing more to walk
            return;
        }
        }
    }

  private:
    enum class Phase { Evaluate, Acted, Check, Restore, Advance, NextPage };
    static constexpr uint32_t kSettleMs = 1100, kPendingLimitMs = 45000, kChangeWaitMs = 8000;
    static constexpr int kMaxPresses = 40, kMaxUp = 120;

    static std::string lower(std::string text) {
        for (char &c : text)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return text;
    }
    // A switch is what its switch shows: the words beside it ("Kept ready", "Switched off") are the
    // TV's last reading and catch up with a press a moment later. A choice is the value it names.
    static std::string state(const Json &snap) {
        if (snap["focusKind"].string() == "toggle")
            return snap["focusOn"].boolean() ? "on" : "off";
        return snap["focusValue"].string();
    }
    static bool named(const std::vector<std::string> &words, const std::string &title) {
        for (const auto &word : words)
            if (!word.empty() && title.find(word) != std::string::npos)
                return true;
        return false;
    }
    bool confirmable(const std::string &title) const {
        const std::string name = lower(title);
        for (const auto &word : confirm_)
            if (!word.empty() && name.find(word) != std::string::npos)
                return true;
        return false;
    }
    void end(uint32_t now) {
        active_ = false;
        endedAt_ = now;
    }
    void tick(uint32_t now, uint32_t wait) {
        lastAt_ = now;
        wait_ = wait;
    }
    void finishRow(const std::string &skipped, uint32_t now) {
        if (!skipped.empty())
            row_.set("skipped", Json::text(skipped));
        results_.add(row_);
        tick(now, 200);
    }

    bool active_ = false, started_ = false, press_ = true, moved_ = false;
    uint32_t endedAt_ = 0;
    Phase phase_ = Phase::Evaluate;
    int maxRows_ = 1000, toSkip_ = 0, pressed_ = 0, presses_ = 0, stuck_ = 0, up_ = 0;
    uint32_t lastAt_ = 0, wait_ = 0, pressedAt_ = 0, startedAt_ = 0, withinMs_ = 0;
    std::vector<std::string> skip_, only_, confirm_, act_;
    std::set<std::string> pages_;
    std::string before_, lastId_, timeline_;
    Json row_ = Json::object(), results_ = Json::array();
};

} // namespace nativetv
