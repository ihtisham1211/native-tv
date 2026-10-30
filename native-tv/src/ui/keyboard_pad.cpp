#include "ui/keyboard_pad.hpp"
#include "domain/say.hpp"
#include "ui/kit/tuning.hpp"
#include <algorithm>
#include <cmath>

namespace nativetv {
namespace {
namespace layout = kit::layout;
constexpr float kPadWidth = 10 * layout::keyW + 9 * layout::keyGap;
// Shift, delete and the symbols key are wide, the caret keys are plain, Done is accent-wide and
// space takes what is left of the last row.
float capWidth(const std::string &id) {
    if (id == "shift" || id == "delete" || id == "symbols")
        return layout::keyWide;
    if (id == "done")
        return layout::keyDone;
    return layout::keyW;
}
constexpr double kPressStiffness = 720, kPressDamping = .9;
} // namespace

std::vector<std::vector<KeyboardPad::Cap>> KeyboardPad::layout() const {
    std::vector<std::vector<Cap>> rows;
    const auto keys = model_.padRows();
    for (size_t r = 0; r < keys.size(); ++r) {
        float used = 0;
        for (const auto &id : keys[r])
            if (id != "space")
                used += capWidth(id);
        used += (keys[r].size() - 1) * layout::keyGap;
        float x = options_.origin.x, y = options_.origin.y + r * kit::geometry::keyRowPitch();
        std::vector<Cap> row;
        for (const auto &id : keys[r]) {
            float width = id == "space" ? std::max<float>(layout::keyW, kPadWidth - used)
                                        : capWidth(id);
            row.push_back({id, {x, y, width, layout::keyH}});
            x += width + layout::keyGap;
        }
        rows.push_back(std::move(row));
    }
    return rows;
}
std::string KeyboardPad::motionId(const char *kind, size_t row, size_t column) const {
    return options_.id + ":" + kind + ":" + std::to_string(row) + ":" + std::to_string(column);
}
void KeyboardPad::begin(Motion &motion, const std::string &value, const Options &options) {
    motion.erasePrefix(options_.id + ":");
    options_ = options;
    model_.reset(value, options.maximum, options.address, options.digits);
    row_ = options.digits ? 0 : 1;
    column_ = 0;
    active_ = true;
    editedAt_ = 0;
    motion.set(motionId("f", row_, column_), 1);
}
void KeyboardPad::focusKey(Motion &motion, size_t row, size_t column) {
    motion.tween(motionId("f", row_, column_), 0, kit::motion::ms::instant, Easing::out());
    row_ = row;
    column_ = column;
    motion.tween(motionId("f", row_, column_), active_ ? 1 : 0, kit::motion::ms::instant,
                 Easing::out());
}
void KeyboardPad::activate(Motion &motion, bool on) {
    active_ = on;
    motion.tween(motionId("f", row_, column_), on ? 1 : 0, kit::motion::ms::instant, Easing::out());
}
void KeyboardPad::setValue(const std::string &value, double nowMs) {
    model_.reset(value, options_.maximum, options_.address, options_.digits);
    editedAt_ = nowMs;
}
std::string KeyboardPad::focusedKey() const {
    auto keys = model_.padRows();
    return row_ < keys.size() && column_ < keys[row_].size() ? keys[row_][column_] : "";
}
KeyboardPad::Result KeyboardPad::press(Motion &motion, double nowMs, size_t row, size_t column) {
    auto keys = model_.padRows();
    if (row >= keys.size() || column >= keys[row].size())
        return Result::None;
    const std::string id = keys[row][column];
    motion.set(motionId("p", row, column), 1);
    motion.spring(motionId("p", row, column), 0,
                  SpringParams::fromDampingRatio(kPressStiffness, kPressDamping));
    if (id == "done")
        return Result::Done;
    bool edited = model_.press(id);
    if (id == "left" || id == "right")
        editedAt_ = nowMs; // the caret shows where it went
    if (edited)
        editedAt_ = nowMs;
    // The layers have different key counts: keep focus on a key that exists.
    auto now = model_.padRows();
    column_ = std::min(column_, now[row_].size() - 1);
    return edited ? Result::Edited : Result::Moved;
}
KeyboardPad::Result KeyboardPad::key(Motion &motion, double nowMs, Key key) {
    if (key == Key::Delete) {
        if (!model_.erase())
            return Result::None;
        editedAt_ = nowMs;
        return Result::Edited;
    }
    if (key == Key::Ok)
        return press(motion, nowMs, row_, column_);
    auto caps = layout();
    if (row_ >= caps.size())
        return Result::None;
    if (key == Key::Left || key == Key::Right) {
        size_t next = column_;
        if (key == Key::Left && column_ > 0)
            --next;
        else if (key == Key::Right && column_ + 1 < caps[row_].size())
            ++next;
        else
            return Result::None;
        focusKey(motion, row_, next);
        return Result::Moved;
    }
    if (key == Key::Up || key == Key::Down) {
        if ((key == Key::Up && row_ == 0) || (key == Key::Down && row_ + 1 >= caps.size()))
            return Result::None;
        size_t target = key == Key::Up ? row_ - 1 : row_ + 1;
        // The key above or below whose centre is nearest the one that has focus.
        float centre = caps[row_][column_].rect.x + caps[row_][column_].rect.w / 2;
        size_t best = 0;
        float nearest = 1e9f;
        for (size_t c = 0; c < caps[target].size(); ++c) {
            float distance = std::fabs(caps[target][c].rect.x + caps[target][c].rect.w / 2 - centre);
            if (distance < nearest) {
                nearest = distance;
                best = c;
            }
        }
        focusKey(motion, target, best);
        return Result::Moved;
    }
    return Result::None;
}
KeyboardPad::Result KeyboardPad::click(Motion &motion, double nowMs, const std::string &hit) {
    const std::string prefix = options_.id + ":";
    if (hit.compare(0, prefix.size(), prefix) != 0)
        return Result::None;
    size_t split = hit.find(':', prefix.size());
    if (split == std::string::npos)
        return Result::None;
    size_t row = static_cast<size_t>(std::atoi(hit.c_str() + prefix.size())),
           column = static_cast<size_t>(std::atoi(hit.c_str() + split + 1));
    auto caps = layout();
    if (row >= caps.size() || column >= caps[row].size())
        return Result::None;
    active_ = true;
    focusKey(motion, row, column);
    return press(motion, nowMs, row, column);
}
KeyboardPad::Result KeyboardPad::type(double nowMs, const std::string &text) {
    if (!model_.insert(text))
        return Result::None;
    editedAt_ = nowMs;
    return Result::Edited;
}
void KeyboardPad::paint(kit::Paint paint, const Motion &motion, kit::Enter enter) const {
    const auto caps = layout();
    for (size_t r = 0; r < caps.size(); ++r)
        for (size_t c = 0; c < caps[r].size(); ++c) {
            const std::string &id = caps[r][c].id;
            kit::KeyCap cap;
            cap.rest = caps[r][c].rect;
            cap.id = options_.id + ":" + std::to_string(r) + ":" + std::to_string(c);
            cap.focus = static_cast<float>(motion.value(motionId("f", r, c)));
            cap.press = static_cast<float>(motion.value(motionId("p", r, c)));
            cap.enter = enter;
            if (id == "shift") {
                cap.icon = "shift";
                cap.latched = model_.upper();
            } else if (id == "delete")
                cap.icon = "backspace";
            else if (id == "left")
                cap.icon = "arrowleft";
            else if (id == "right")
                cap.icon = "arrowright";
            else if (id == "done") {
                cap.icon = options_.doneIcon;
                cap.label = options_.doneLabel;
                cap.accent = true;
            } else if (id == "space") {
                cap.label = say::msg("setup.key.space", "space");
                cap.hint = true;
            } else
                cap.label = model_.label(id);
            if (id == "symbols")
                cap.latched = model_.symbols();
            kit::keyCap(paint, cap);
        }
}
kit::TextField KeyboardPad::field(double nowMs, Rect rest, const std::string &placeholder,
                                  bool secret) const {
    kit::TextField field;
    field.rest = rest;
    field.id = options_.id + ":field";
    field.value = model_.value();
    field.cursor = model_.cursor();
    field.placeholder = placeholder;
    field.secret = secret;
    field.caret = caretLit(nowMs) ? 1.0f : 0.0f;
    return field;
}
} // namespace nativetv
