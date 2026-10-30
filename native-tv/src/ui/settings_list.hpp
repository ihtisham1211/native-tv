#pragma once
#include "domain/settings_items.hpp"
#include "ui/screen.hpp"
#include <string>
#include <utility>
#include <vector>

// A scrolling list of settings rows (spec 3.5), used for Settings' sections and rows and for the
// install steps of the Catalog: a clipped column of rows with toggles, the focus, and a detail
// text for the focused row that is drawn as a strip or as the kit's side panel. The owner decides
// what the rows are and what OK does; this keeps the focus, the scroll, the toggles and the
// detail's swap, all as animated values under `prefix`.
namespace nativetv {
// Where a list sits.
struct ListStyle {
    // The region the rows scroll in. Its edges clip, so it reaches past the rows on the left and
    // right far enough for a focused row's lift and ring to show whole.
    Rect viewport;
    float rowX = 0, rowW = 0; // where the rows rest, inside it
    // The section list: the row that has the focus keeps a tint while the focus is elsewhere, so
    // the page next to it still says which section it shows.
    bool marksSelected = false;
};

class SettingsList {
  public:
    // How `set` treats the motion of the rows it replaces.
    enum class Reset {
        Keep,    // a refresh: nothing moves but what changed
        Stagger, // a new page of rows: they enter one after another
        Fade     // another list in the same place: they fade in together
    };
    SettingsList(std::string prefix, ListStyle style) : prefix_(std::move(prefix)), style_(style) {}

    // Replaces the rows. A refresh keeps the focus on the row of the same id (and the scroll where
    // it was); a toggle that changed slides.
    void set(ScreenHost &host, std::vector<SettingItem> items, Reset reset);
    // Moves the region, for a page that gains a row of tabs above it.
    void moveViewport(ScreenHost &host, Rect viewport);
    const std::vector<SettingItem> &items() const {
        return items_;
    }
    size_t focus() const {
        return focus_;
    }
    const SettingItem *current() const {
        return focus_ < items_.size() ? &items_[focus_] : nullptr;
    }
    const SettingItem *shown() const {
        return shown_ < items_.size() ? &items_[shown_] : nullptr;
    }
    // The id of the focused row, and where the list is scrolled to (<= 0): what a refresh must
    // keep.
    std::string focusId() const {
        return focus_ < items_.size() ? items_[focus_].id : std::string();
    }
    float scroll(ScreenHost &host) const {
        return static_cast<float>(host.motion().value(prefix_ + ":sy"));
    }
    // Moves to the next row that can be focused; false at either end.
    bool move(ScreenHost &host, int direction, int rows = 1);
    void select(ScreenHost &host, size_t index);
    void selectId(ScreenHost &host, const std::string &id);
    void focusFirst(ScreenHost &host);
    void focusLast(ScreenHost &host);
    // False while the focus is elsewhere (the top bar, another part of the page): the row keeps its
    // place, drawn at rest.
    void show(ScreenHost &host, bool onPage);
    bool onPage() const {
        return onPage_;
    }
    // Says the detail should be read again (the focus came back from elsewhere).
    void refreshDetail(ScreenHost &host);
    void update(ScreenHost &host);
    // The rows. `pending` rows are drawn a little faded: their change is on its way.
    void build(ScreenHost &host, kit::Paint paint);
    // The focused row's words as the kit's side panel (the Catalog's steps), or as a strip.
    void buildPanel(ScreenHost &host, kit::Paint paint);
    void buildStrip(ScreenHost &host, kit::Paint paint, Rect box);
    void clear(ScreenHost &host);
    // The id of a row's pointer target, and its index back.
    std::string hitId(size_t index) const;
    bool parseHit(const std::string &id, size_t &index) const;

    // Rows from the top of the list's content: heights differ for section labels.
    static std::vector<float> layout(const std::vector<SettingItem> &items, float &total);
    static float scrollTarget(const std::vector<float> &tops, size_t focus, float total,
                              float viewportHeight);

  private:
    std::string id(const char *name, const std::string &key) const {
        return prefix_ + ":" + name + ":" + key;
    }
    void retarget(ScreenHost &host, bool jump);
    void startSwap(ScreenHost &host);

    std::string prefix_;
    ListStyle style_;
    std::vector<SettingItem> items_;
    std::vector<float> tops_;
    float total_ = 0;
    size_t focus_ = 0, shown_ = 0;
    bool onPage_ = true, swapping_ = false;
};
} // namespace nativetv
