#pragma once
#include "domain/settings_items.hpp"
#include "ui/screen.hpp"
#include <string>
#include <utility>
#include <vector>

// The list-and-panel page of the design (spec 3.5), used by Settings and by the install steps of
// the Catalog: a clipped list of rows with toggles on the left, and the detail panel of the focused
// row on the right. The owner decides what the rows are and what OK does; this keeps the focus,
// the scroll, the toggles and the panel's swap, all as animated values under `prefix`.
namespace nativetv {
class SettingsList {
  public:
    explicit SettingsList(std::string prefix) : prefix_(std::move(prefix)) {}

    // Replaces the rows. `fresh` replays the entrance and starts at the first row; otherwise the
    // rows stay where they are, the focus follows the row of the same title, and a toggle that
    // changed slides.
    void set(ScreenHost &host, std::vector<SettingItem> items, bool fresh);
    const std::vector<SettingItem> &items() const {
        return items_;
    }
    size_t focus() const {
        return focus_;
    }
    const SettingItem *current() const {
        return focus_ < items_.size() ? &items_[focus_] : nullptr;
    }
    // Moves to the next row that can be focused; false at either end.
    bool move(ScreenHost &host, int direction, int rows = 1);
    void select(ScreenHost &host, size_t index);
    void focusFirst(ScreenHost &host);
    // False while the focus is on the top bar: the row keeps its place, drawn at rest.
    void show(ScreenHost &host, bool onPage);
    void update(ScreenHost &host);
    // `empty` is drawn in place of the list when there are no rows.
    void build(ScreenHost &host, kit::Paint paint, const std::string &empty);
    void clear(ScreenHost &host);
    // The id of a row's pointer target, and its index back.
    std::string hitId(size_t index) const;
    bool parseHit(const std::string &id, size_t &index) const;

    // Rows from the top of the list's content: heights differ for section labels.
    static std::vector<float> layout(const std::vector<SettingItem> &items, float &total);
    static float scrollTarget(const std::vector<float> &tops, size_t focus, float total);

  private:
    std::string id(const char *name, size_t index) const {
        return prefix_ + ":" + name + ":" + std::to_string(index);
    }
    void retarget(ScreenHost &host, bool jump);
    void startSwap(ScreenHost &host);

    std::string prefix_;
    std::vector<SettingItem> items_;
    std::vector<float> tops_;
    float total_ = 0;
    size_t focus_ = 0, shown_ = 0;
    bool onPage_ = true, swapping_ = false;
};
} // namespace nativetv
