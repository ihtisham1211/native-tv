#include "ui/settings_list.hpp"
#include "ui/application.hpp"
#include "ui/kit/tuning.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <sstream>

namespace nativetv {
namespace {
namespace ms = kit::motion::ms;
namespace layout = kit::layout;
constexpr float kLabelHeight = 40, kLabelGap = 12;
constexpr float kLabelTextTop = 6; // the 32 px line and its descenders stay inside the 40 px slot
constexpr size_t kEntranceRows = 12; // rows past the first screenful have no entrance
constexpr int kPanelOutMs = 80, kPanelInMs = 200;
constexpr int kFadeMs = 180;        // a list replaced by another in the same place
constexpr float kPendingFade = .3f; // how much of its strength a row loses while its change is out
constexpr int kStripBodyLines = 2;  // description lines the strip's box has room for
constexpr int kStripBodyMax = 3;    // a longer description ends in an ellipsis

float labelHeight(const SettingItem &item) {
    return item.kind == SettingItem::Kind::Heading ? kLabelHeight : layout::srowH;
}

// The lines `text` takes in `width`, up to `limit`. It wraps greedily at spaces and breaks a
// paragraph at "\n", measuring candidate lines with the glyphs that draw them, so it agrees with
// the layout without a string being known to wrap.
int wrappedLineCount(const kit::Glyphs &glyphs, const std::string &text,
                     const kit::TypeStyle &style, float width, int limit) {
    int lines = 0;
    for (size_t begin = 0; lines < limit && begin <= text.size();) {
        size_t end = std::min(text.find('\n', begin), text.size());
        std::istringstream words(text.substr(begin, end - begin));
        std::string line;
        ++lines;
        for (std::string word; words >> word;) {
            std::string joined = line.empty() ? word : line + ' ' + word;
            if (!line.empty() && glyphs.measure(joined, style) > width + .01f) {
                if (++lines >= limit)
                    return limit;
                joined = word;
            }
            line = joined;
        }
        begin = end + 1;
    }
    return lines;
}
} // namespace

std::vector<float> SettingsList::layout(const std::vector<SettingItem> &items, float &total) {
    std::vector<float> tops;
    float y = 0;
    for (const SettingItem &item : items) {
        if (item.kind == SettingItem::Kind::Heading) {
            if (y > 0)
                y += kLabelGap;
            tops.push_back(y);
            y += kLabelHeight;
        } else {
            tops.push_back(y);
            y += layout::srowH + layout::srowGap;
        }
    }
    total = y;
    return tops;
}

// The list follows once focus passes the middle of what is in view, as the kit's fixed-pitch list
// does, and keeps the section label with the first row under it.
float SettingsList::scrollTarget(const std::vector<float> &tops, size_t focus, float total,
                                 float viewportHeight) {
    if (focus >= tops.size())
        return 0;
    float anchor = tops[focus];
    if (focus && tops[focus - 1] < anchor && anchor - tops[focus - 1] <= kLabelHeight)
        anchor = tops[focus - 1];
    float pitch = kit::geometry::settingsRowPitch();
    float visible = viewportHeight - layout::srowGap;
    float room = std::max(0.0f, total - visible);
    int keep = std::max(1, static_cast<int>(visible / pitch) / 2);
    float wanted = anchor - static_cast<float>(keep) * pitch;
    return -kit::geometry::clamp(wanted, 0, room);
}

void SettingsList::set(ScreenHost &host, std::vector<SettingItem> items, Reset reset) {
    Motion &motion = host.motion();
    const bool fresh = reset != Reset::Keep;
    std::string focusedId = focusId();
    std::vector<SettingItem> before = std::move(items_);
    items_ = std::move(items);
    {
        // A row with no id of its own is named by its title and which of that title it is.
        std::map<std::string, int> seen;
        for (SettingItem &item : items_)
            if (item.id.empty())
                item.id = item.title + "#" + std::to_string(seen[item.title]++);
    }
    tops_ = layout(items_, total_);
    if (fresh) {
        motion.erasePrefix(prefix_ + ":");
        focus_ = 0;
        while (focus_ < items_.size() && !items_[focus_].focusable())
            ++focus_;
        Timeline timeline;
        for (size_t i = 0; i < items_.size() && i < kEntranceRows; ++i)
            timeline.add(id("E", std::to_string(i)),
                         reset == Reset::Fade
                             ? Tween(1, 0, kFadeMs, kit::easing(kit::motion::ease::out))
                             : Tween(1, 0, ms::base, kit::easing(kit::motion::ease::out),
                                     kit::motion::stagger::pageBase +
                                         kit::motion::stagger::pageItem *
                                             std::min<int>(static_cast<int>(i),
                                                           kit::motion::stagger::pageItemMax)));
        timeline.play(motion);
    } else {
        // The focus follows its row, which may have moved; a row that went leaves it where it was.
        size_t found = items_.size();
        for (size_t i = 0; i < items_.size(); ++i)
            if (items_[i].focusable() && items_[i].id == focusedId) {
                found = i;
                break;
            }
        if (found < items_.size())
            focus_ = found;
        else {
            focus_ = std::min(focus_, items_.empty() ? 0 : items_.size() - 1);
            while (focus_ + 1 < items_.size() && !items_[focus_].focusable())
                ++focus_;
            while (focus_ > 0 && !items_[focus_].focusable())
                --focus_;
        }
    }
    std::map<std::string, const SettingItem *> before_by_id;
    for (const SettingItem &item : before)
        before_by_id[item.id] = &item;
    for (const SettingItem &item : items_) {
        if (item.kind == SettingItem::Kind::Toggle) {
            double target = item.on ? 1 : 0;
            if (!fresh && before_by_id.count(item.id))
                motion.tween(id("on", item.id), target, ms::fast,
                             kit::easing(kit::motion::ease::out));
            else
                motion.set(id("on", item.id), target);
        }
        double pending = item.pending ? 1 : 0;
        if (!fresh && before_by_id.count(item.id))
            motion.tween(id("pn", item.id), pending, ms::focus,
                         kit::easing(kit::motion::ease::out));
        else
            motion.set(id("pn", item.id), pending);
    }
    shown_ = focus_;
    swapping_ = false;
    motion.set(prefix_ + ":pd", 0);
    for (size_t i = 0; i < items_.size(); ++i)
        motion.set(id("f", items_[i].id), !fresh && onPage_ && i == focus_ ? 1 : 0);
    if (fresh && onPage_ && focus_ < items_.size())
        motion.tween(id("f", items_[focus_].id), 1, ms::focus, kit::easing(kit::motion::ease::out),
                     kit::motion::stagger::pageBase);
    retarget(host, fresh);
    host.invalidate();
}

void SettingsList::moveViewport(ScreenHost &host, Rect viewport) {
    if (viewport.x == style_.viewport.x && viewport.y == style_.viewport.y &&
        viewport.w == style_.viewport.w && viewport.h == style_.viewport.h)
        return;
    style_.viewport = viewport;
    retarget(host, true);
    host.invalidate();
}

void SettingsList::retarget(ScreenHost &host, bool jump) {
    double target = scrollTarget(tops_, focus_, total_, style_.viewport.h);
    if (jump)
        host.motion().set(prefix_ + ":sy", target);
    else
        host.motion().spring(prefix_ + ":sy", target, kit::springParams(kit::motion::spring::list));
}

void SettingsList::startSwap(ScreenHost &host) {
    swapping_ = true;
    host.motion().tween(prefix_ + ":pd", 1, kPanelOutMs, kit::easing(kit::motion::ease::out));
}

void SettingsList::select(ScreenHost &host, size_t index) {
    if (index >= items_.size() || !items_[index].focusable() || index == focus_)
        return;
    Motion &motion = host.motion();
    if (onPage_) {
        motion.tween(id("f", items_[focus_].id), 0, ms::focus, kit::easing(kit::motion::ease::out));
        motion.tween(id("f", items_[index].id), 1, ms::focus, kit::easing(kit::motion::ease::out));
    }
    focus_ = index;
    retarget(host, false);
    startSwap(host);
    host.invalidate();
}

void SettingsList::selectId(ScreenHost &host, const std::string &wanted) {
    for (size_t i = 0; i < items_.size(); ++i)
        if (items_[i].id == wanted && items_[i].focusable()) {
            select(host, i);
            return;
        }
}

bool SettingsList::move(ScreenHost &host, int direction, int rows) {
    if (items_.empty())
        return false;
    int index = static_cast<int>(focus_), count = static_cast<int>(items_.size());
    bool moved = false;
    for (int step = 0; step < rows; ++step) {
        int next = index + direction;
        while (next >= 0 && next < count && !items_[static_cast<size_t>(next)].focusable())
            next += direction;
        if (next < 0 || next >= count)
            break;
        index = next;
        moved = true;
    }
    if (moved)
        select(host, static_cast<size_t>(index));
    return moved;
}

void SettingsList::focusFirst(ScreenHost &host) {
    size_t first = 0;
    while (first < items_.size() && !items_[first].focusable())
        ++first;
    if (first < items_.size())
        select(host, first);
}

void SettingsList::focusLast(ScreenHost &host) {
    size_t last = items_.size();
    while (last > 0 && !items_[last - 1].focusable())
        --last;
    if (last)
        select(host, last - 1);
}

void SettingsList::show(ScreenHost &host, bool onPage) {
    if (onPage == onPage_)
        return;
    onPage_ = onPage;
    if (focus_ < items_.size())
        host.motion().tween(id("f", items_[focus_].id), onPage ? 1 : 0, ms::focus,
                            kit::easing(kit::motion::ease::out));
    if (onPage)
        startSwap(host);
    host.invalidate();
}

void SettingsList::refreshDetail(ScreenHost &host) {
    startSwap(host);
}

// The detail dips for the first 80 ms of a change of row, swaps its words while it is hidden, and
// comes back (spec 5.9).
void SettingsList::update(ScreenHost &host) {
    Motion &motion = host.motion();
    if (swapping_ && motion.value(prefix_ + ":pd") >= .999) {
        shown_ = focus_;
        swapping_ = false;
        motion.tween(prefix_ + ":pd", 0, kPanelInMs, kit::easing(kit::motion::ease::out));
    }
}

std::string SettingsList::hitId(size_t index) const {
    return prefix_ + ":row:" + std::to_string(index);
}
bool SettingsList::parseHit(const std::string &hit, size_t &index) const {
    std::string head = prefix_ + ":row:";
    if (hit.compare(0, head.size(), head) != 0)
        return false;
    index = static_cast<size_t>(std::atoi(hit.c_str() + head.size()));
    return index < items_.size();
}

void SettingsList::clear(ScreenHost &host) {
    host.motion().erasePrefix(prefix_ + ":");
    items_.clear();
    tops_.clear();
    focus_ = shown_ = 0;
    swapping_ = false;
}

void SettingsList::build(ScreenHost &host, kit::Paint paint) {
    Motion &motion = host.motion();
    Scene &scene = paint.scene;
    if (items_.empty())
        return;
    const Rect &viewport = style_.viewport;
    const float scroll = static_cast<float>(motion.value(prefix_ + ":sy"));
    scene.pushClip(viewport);
    for (size_t i = 0; i < items_.size(); ++i) {
        const SettingItem &item = items_[i];
        Rect rest = {style_.rowX, viewport.y + layout::srowGap + tops_[i] + scroll, style_.rowW,
                     labelHeight(item)};
        if (rest.y + rest.h < viewport.y - 30 || rest.y > viewport.y + viewport.h + 30)
            continue;
        double hidden = motion.value(id("E", std::to_string(i)));
        kit::Enter enter = {static_cast<float>(1 - hidden), static_cast<float>(28 * hidden)};
        if (item.kind == SettingItem::Kind::Heading) {
            // A label scrolling under the top edge fades out rather than being cut through.
            float edge = kit::focus::clamp01((rest.y + kLabelTextTop - viewport.y) / 24);
            scene.pushOpacity(enter.alpha * edge);
            scene.pushTransform(Transform::translate(0, enter.dy));
            paint.glyphs.text(scene,
                              {rest.x + layout::srowPadLeft, rest.y + kLabelTextTop, rest.w - 40,
                               kit::type::labelM.line},
                              item.title, kit::type::labelM, kit::color::onSurface3);
            scene.popTransform();
            scene.popOpacity();
            continue;
        }
        kit::SettingsRow row;
        row.rest = rest;
        row.id = hitId(i);
        row.icon = item.icon;
        row.title = item.title;
        row.value = item.value;
        row.toggle = item.kind == SettingItem::Kind::Toggle;
        row.on = static_cast<float>(motion.value(id("on", item.id)));
        row.focus = static_cast<float>(motion.value(id("f", item.id)));
        row.enter = enter;
        row.enter.alpha *= 1 - kPendingFade * static_cast<float>(motion.value(id("pn", item.id)));
        kit::settingsRow(paint, row);
        float pending = static_cast<float>(motion.value(id("pn", item.id)));
        if (pending > .01f) {
            // Its change is on its way: a small dot that breathes at the row's right edge.
            float phase = kit::spinnerPhase(motion, prefix_ + ":pulse");
            float pulse = .5f + .5f * std::sin(phase * 6.2831853f);
            Color ink = kit::focus::mix(kit::color::accent, kit::color::onAccent, row.focus);
            scene.shape(
                {rest.x + rest.w - 16, rest.y + (rest.h - 8) / 2, 8, 8},
                kit::focus::withAlpha(ink, pending * (.35f + .65f * pulse) * row.enter.alpha), 4);
        }
        if (style_.marksSelected && i == focus_ && row.focus < 1) {
            // The section the page shows, while the focus is on the page.
            Node &mark = scene.shape(rest, kit::alpha::accentDim, kit::radius::lg);
            mark.opacity = (1 - row.focus) * row.enter.alpha;
        }
    }
    scene.popClip();
}

void SettingsList::buildPanel(ScreenHost &host, kit::Paint paint) {
    if (shown_ >= items_.size() || items_[shown_].kind == SettingItem::Kind::Heading)
        return;
    const SettingItem &item = items_[shown_];
    double dip = host.motion().value(prefix_ + ":pd");
    kit::DetailPanel panel;
    panel.icon = item.icon;
    panel.title = item.title;
    panel.body = item.body;
    panel.action = item.action;
    panel.chips = item.chips;
    float bodyWidth = paint.glyphs.measure(item.body, kit::type::body);
    panel.bodyLines = std::max(1, std::min(5, static_cast<int>(std::ceil(bodyWidth / 640))));
    panel.enter = {static_cast<float>(1 - dip), static_cast<float>(10 * dip)};
    kit::detailPanel(paint, panel);
}

// What the focused row is and does, along the foot of a page: its name, what it reads, what OK
// does to it, and its words under that.
void SettingsList::buildStrip(ScreenHost &host, kit::Paint paint, Rect box) {
    if (shown_ >= items_.size() || items_[shown_].kind == SettingItem::Kind::Heading)
        return;
    const SettingItem &item = items_[shown_];
    Scene &scene = paint.scene;
    double dip = host.motion().value(prefix_ + ":pd");
    const float left = box.x + 28, inner = box.w - 56;
    // The box has room for two lines of words; each more grows it upward, so its bottom edge and
    // the rows' room above stay where the owner put them.
    const int bodyLines =
        wrappedLineCount(paint.glyphs, item.body, kit::type::body, inner, kStripBodyMax);
    const float extra = static_cast<float>(std::max(0, bodyLines - kStripBodyLines)) *
                        kit::type::body.line;
    box.y -= extra;
    box.h += extra;
    scene.pushOpacity(static_cast<float>(1 - dip));
    scene.pushTransform(Transform::translate(0, static_cast<float>(10 * dip)));
    Node &card = scene.shape(box, kit::color::surface1, kit::radius::xl);
    card.ring = {1, 0, kit::alpha::outline};
    float titleWidth = std::min(paint.glyphs.measure(item.title, kit::type::title), inner * .45f);
    paint.glyphs.text(scene, {left, box.y + 20, titleWidth + 4, kit::type::title.line}, item.title,
                      kit::type::title, kit::color::onSurface);
    // The OK hint at the right end, the chips between it and the title.
    float hintWidth = 0;
    if (!item.action.empty()) {
        float capWidth = std::max(30.0f, paint.glyphs.measure("OK", kit::type::cap) + 24);
        float textWidth = paint.glyphs.measure(item.action, kit::type::labelM);
        hintWidth = capWidth + 12 + textWidth;
        float x = box.x + box.w - 28 - hintWidth, y = box.y + 23;
        scene.shape({x, y, capWidth, 30}, kit::color::surface3, kit::radius::sm);
        paint.glyphs.text(scene,
                          {x, y + (30 - kit::type::cap.line) / 2, capWidth, kit::type::cap.line},
                          "OK", kit::type::cap, kit::color::onSurface2, Node::Align::Center);
        paint.glyphs.text(scene,
                          {x + capWidth + 12, y + (30 - kit::type::labelM.line) / 2, textWidth + 8,
                           kit::type::labelM.line},
                          item.action, kit::type::labelM, kit::color::onSurface3);
    }
    float x = left + titleWidth + 24, limit = box.x + box.w - 28 - hintWidth - 24;
    for (const std::string &chip : item.chips) {
        float width = paint.glyphs.measure(chip, kit::type::labelM) + 54;
        if (x + width > limit)
            break;
        x += kit::pillChip(paint, x, box.y + 18, chip) + 10;
    }
    paint.glyphs.text(scene, {left, box.y + 66, inner, kit::type::body.line * kStripBodyMax},
                      item.body, kit::type::body, kit::color::onSurface2, Node::Align::Left,
                      kStripBodyMax);
    scene.popTransform();
    scene.popOpacity();
}
} // namespace nativetv
