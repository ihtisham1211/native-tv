#include "ui/settings_list.hpp"
#include "ui/application.hpp"
#include "ui/kit/tuning.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace nativetv {
namespace {
namespace ms = kit::motion::ms;
namespace layout = kit::layout;
constexpr float kLabelHeight = 40, kLabelGap = 12;
constexpr size_t kEntranceRows = 12; // rows past the first screenful have no entrance
constexpr int kPanelOutMs = 80, kPanelInMs = 200;

float labelHeight(const SettingItem &item) {
    return item.kind == SettingItem::Kind::Heading ? kLabelHeight : layout::srowH;
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

// The list follows once focus passes the 4th row, as the kit's fixed-pitch list does, and keeps
// the section label with the first row under it.
float SettingsList::scrollTarget(const std::vector<float> &tops, size_t focus, float total) {
    if (focus >= tops.size())
        return 0;
    float anchor = tops[focus];
    if (focus && tops[focus - 1] < anchor && anchor - tops[focus - 1] <= kLabelHeight)
        anchor = tops[focus - 1];
    float visible = layout::listH - layout::srowGap;
    float room = std::max(0.0f, total - visible);
    float wanted = anchor - 3 * kit::geometry::settingsRowPitch();
    return -kit::geometry::clamp(wanted, 0, room);
}

void SettingsList::set(ScreenHost &host, std::vector<SettingItem> items, bool fresh) {
    Motion &motion = host.motion();
    std::string focusedTitle = current() ? current()->title : "";
    std::vector<SettingItem> before = std::move(items_);
    items_ = std::move(items);
    tops_ = layout(items_, total_);
    if (fresh) {
        motion.erasePrefix(prefix_ + ":");
        focus_ = 0;
        while (focus_ < items_.size() && !items_[focus_].focusable())
            ++focus_;
        Timeline timeline;
        for (size_t i = 0; i < items_.size() && i < kEntranceRows; ++i)
            timeline.add(id("E", i),
                         Tween(1, 0, ms::base, kit::easing(kit::motion::ease::out),
                               kit::motion::stagger::pageBase +
                                   kit::motion::stagger::pageItem *
                                       std::min<int>(static_cast<int>(i),
                                                     kit::motion::stagger::pageItemMax)));
        timeline.play(motion);
    } else {
        // The focus follows its row, which may have moved; a row that went leaves it where it was.
        size_t found = items_.size();
        for (size_t i = 0; i < items_.size(); ++i)
            if (items_[i].focusable() && items_[i].title == focusedTitle) {
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
    for (size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].kind != SettingItem::Kind::Toggle)
            continue;
        double target = items_[i].on ? 1 : 0;
        bool same = !fresh && i < before.size() && before[i].title == items_[i].title;
        if (same)
            motion.tween(id("on", i), target, ms::fast, kit::easing(kit::motion::ease::out));
        else
            motion.set(id("on", i), target);
    }
    shown_ = focus_;
    swapping_ = false;
    motion.set(prefix_ + ":pd", 0);
    for (size_t i = 0; i < items_.size(); ++i)
        motion.set(id("f", i), !fresh && onPage_ && i == focus_ ? 1 : 0);
    if (fresh && onPage_ && focus_ < items_.size())
        motion.tween(id("f", focus_), 1, ms::focus, kit::easing(kit::motion::ease::out),
                     kit::motion::stagger::pageBase);
    retarget(host, fresh);
    host.invalidate();
}

void SettingsList::retarget(ScreenHost &host, bool jump) {
    double target = scrollTarget(tops_, focus_, total_);
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
        motion.tween(id("f", focus_), 0, ms::focus, kit::easing(kit::motion::ease::out));
        motion.tween(id("f", index), 1, ms::focus, kit::easing(kit::motion::ease::out));
    }
    focus_ = index;
    retarget(host, false);
    startSwap(host);
    host.invalidate();
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

void SettingsList::show(ScreenHost &host, bool onPage) {
    if (onPage == onPage_)
        return;
    onPage_ = onPage;
    if (focus_ < items_.size())
        host.motion().tween(id("f", focus_), onPage ? 1 : 0, ms::focus,
                            kit::easing(kit::motion::ease::out));
    host.invalidate();
}

// The panel dips for the first 80 ms of a change of row, swaps its words while it is hidden, and
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

void SettingsList::build(ScreenHost &host, kit::Paint paint, const std::string &empty) {
    Motion &motion = host.motion();
    Scene &scene = paint.scene;
    if (items_.empty()) {
        if (!empty.empty())
            paint.glyphs.text(scene,
                              {kit::canvas::safeX, layout::listTop + 24, 840, kit::type::body.line},
                              empty, kit::type::body, kit::color::onSurface2);
        return;
    }
    const Rect viewport = kit::geometry::settingsViewport();
    const float scroll = static_cast<float>(motion.value(prefix_ + ":sy"));
    scene.pushClip(viewport);
    for (size_t i = 0; i < items_.size(); ++i) {
        const SettingItem &item = items_[i];
        Rect rest = {kit::canvas::safeX, viewport.y + layout::srowGap + tops_[i] + scroll,
                     layout::listW, labelHeight(item)};
        if (rest.y + rest.h < viewport.y - 30 || rest.y > viewport.y + viewport.h + 30)
            continue;
        double hidden = motion.value(id("E", i));
        kit::Enter enter = {static_cast<float>(1 - hidden), static_cast<float>(28 * hidden)};
        if (item.kind == SettingItem::Kind::Heading) {
            // A label scrolling under the top edge fades out rather than being cut through.
            float edge = kit::focus::clamp01((rest.y + 14 - viewport.y) / 24);
            scene.pushOpacity(enter.alpha * edge);
            scene.pushTransform(Transform::translate(0, enter.dy));
            paint.glyphs.text(scene,
                              {rest.x + layout::srowPadLeft, rest.y + 14, layout::listW - 40,
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
        row.on = static_cast<float>(motion.value(id("on", i)));
        row.focus = static_cast<float>(motion.value(id("f", i)));
        row.enter = enter;
        kit::settingsRow(paint, row);
    }
    scene.popClip();

    if (shown_ >= items_.size() || items_[shown_].kind == SettingItem::Kind::Heading)
        return;
    const SettingItem &item = items_[shown_];
    double dip = motion.value(prefix_ + ":pd");
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
} // namespace nativetv
