#pragma once
#include "ui/keyboard.hpp"
#include "ui/kit/entry.hpp"
#include "ui/motion/motion.hpp"
#include "ui/screen.hpp"
#include <string>
#include <vector>

namespace nativetv {
// The on-screen keyboard of the TV pages: the Keyboard text model, the pad's focus and its
// animation, and the nodes. One pad per page that types; Setup uses it for the broker's address,
// port, user and password, and the editors can take it in place of kit::keyboard.
//
// A page owns a KeyboardPad, forwards Key presses and hardware text to it, and draws it:
//
//     pad.begin(motion, "", {"setup:kb"});            // once, when the page opens
//     Result result = pad.key(motion, nowMs, key);    // Done: the page takes pad.value()
//     pad.paint(paint, motion);                       // inside build()
//     kit::TextField field = pad.field(nowMs);        // the line the typing shows in
//
// Every Motion id the pad owns starts with Options::id, so the page drops them with
// Motion::erasePrefix. Hits are "<id>:<row>:<column>", which click() takes back.
class KeyboardPad {
  public:
    enum class Result {
        None,   // the key did nothing here (an edge of the pad: the page may use it)
        Moved,  // focus moved
        Edited, // the text changed
        Done    // the Done key was pressed
    };
    struct Options {
        std::string id = "kb";       // prefix of the Motion ids and hits
        Rect origin = kit::metrics::setup::key1; // top-left of the first key
        size_t maximum = 80;         // characters
        bool address = false;        // "." where a name has "'"
        bool digits = false;         // a number: only the digits, over delete and Done
        std::string doneLabel = "Done", doneIcon = "check";
    };
    // Sets the text (caret at its end), puts focus on "q" (on "1" for digits) and clears the layers.
    void begin(Motion &motion, const std::string &value, const Options &options);
    // Arrows move focus, OK presses the key, Delete erases before the caret.
    Result key(Motion &motion, double nowMs, Key key);
    // A pointer press on a hit this pad drew.
    Result click(Motion &motion, double nowMs, const std::string &hit);
    // Characters from a hardware keyboard.
    Result type(double nowMs, const std::string &text);
    // Shows or hides the focus (false while the page's focus is elsewhere).
    void activate(Motion &motion, bool on);
    // Replaces the text and puts the caret at its end.
    void setValue(const std::string &value, double nowMs);
    std::string value() const {
        return model_.value();
    }
    size_t cursor() const {
        return model_.cursor();
    }
    const Keyboard &model() const {
        return model_;
    }
    const Options &options() const {
        return options_;
    }
    void setDone(const std::string &label, const std::string &icon = "check") {
        options_.doneLabel = label;
        options_.doneIcon = icon;
    }
    // Whether the caret is lit at this time: it restarts at every edit.
    bool caretLit(double nowMs) const {
        return kit::caretLit(nowMs, editedAt_);
    }
    // The focused key's id ("q", "shift", "done") for tests and for a page's own checks.
    std::string focusedKey() const;
    void paint(kit::Paint paint, const Motion &motion, kit::Enter enter = {}) const;
    // The line the typing shows in, at `rest`, with the caret lit as the blink says.
    kit::TextField field(double nowMs, Rect rest, const std::string &placeholder,
                         bool secret = false) const;

  private:
    struct Cap {
        std::string id;
        Rect rect;
    };
    std::vector<std::vector<Cap>> layout() const;
    Result press(Motion &motion, double nowMs, size_t row, size_t column);
    void focusKey(Motion &motion, size_t row, size_t column);
    std::string motionId(const char *kind, size_t row, size_t column) const;
    Keyboard model_;
    Options options_;
    size_t row_ = 1, column_ = 0;
    bool active_ = true;
    double editedAt_ = 0;
};
} // namespace nativetv
