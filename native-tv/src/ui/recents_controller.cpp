#include "ui/application.hpp"
#include "ui/controller_helpers.hpp"
#include "ui/home_layout.hpp"
#include "ui/kit/tuning.hpp"

namespace nativetv {
void Application::recentResponse(const Json &data) {
    auto &recent = state_.recents;
    std::string selected =
        recent.selected < recent.cards.size() ? recent.cards[recent.selected].id : "";
    recent.cards.clear();
    ++recentSnapshot_;
    for (const auto &item : data["apps"].items()) {
        Card card;
        card.id = item["id"].string();
        card.title = item["title"].string(card.id);
        if (item["image"].boolean())
            card.image = "recent:" + std::to_string(recentSnapshot_) + ":" + card.id;
        // A new opaque reference refreshes snapshots without evicting unrelated artwork.
        card.data = item;
        recent.cards.push_back(card);
    }
    recent.selected = clampIndex(recent.selected, recent.cards.size());
    for (size_t i = 0; i < recent.cards.size(); ++i)
        if (recent.cards[i].id == selected)
            recent.selected = i;
    if (recent.cards.empty())
        recent.focus = RecentState::Focus::Card;
    recent.status = recent.cards.empty() ? "Apps stay here while they are open." : "";
    recentsFocus(true);
    dirty_ = true;
}
// Ring and lift follow the selected card; the row scrolls so the selection keeps the fifth slot.
void Application::recentsFocus(bool jump) {
    const auto &recent = state_.recents;
    auto glide = [&](const std::string &id, double to) {
        if (jump)
            motion_.set(id, to);
        else
            motion_.tween(id, to, kit::motion::ms::focus, kit::easing(kit::motion::ease::out));
    };
    for (size_t i = 0; i < recent.cards.size(); ++i)
        glide(home::id("rf", i),
              i == recent.selected && recent.focus != RecentState::Focus::All ? 1 : 0);
    glide("rfa", recent.focus == RecentState::Focus::All ? 1 : 0);
    glide("rfx", recent.focus == RecentState::Focus::Close ? 1 : 0);
    float pitch = 320 + 24, keep = 3;
    float room = std::max(0.0f, static_cast<float>(recent.cards.size()) * pitch - 24 -
                                    (kit::canvas::width - 2 * kit::canvas::safeX));
    double target = -std::min(room, std::max(0.0f, (static_cast<float>(recent.selected) - keep) * pitch));
    if (jump)
        motion_.set("rcx", target);
    else
        motion_.spring("rcx", target, kit::springParams(kit::motion::spring::row));
}
void Application::recentKey(Key key) {
    auto &recent = state_.recents;
    if (key == Key::Ok && recent.cards.empty() && recent.status.find("Could not") == 0) {
        recent.revision = services_.read("recents", Operation::Recents);
        recent.status = "Loading recent apps…";
        return;
    }
    if (recent.cards.empty())
        return;
    if (recent.focus == RecentState::Focus::All) {
        if (key == Key::Down)
            recent.focus = RecentState::Focus::Card;
        if (key != Key::Ok)
            return;
    } else if (key == Key::Left || key == Key::Right) {
        if (key == Key::Left && recent.selected)
            --recent.selected;
        if (key == Key::Right && recent.selected + 1 < recent.cards.size())
            ++recent.selected;
        recent.focus = RecentState::Focus::Card;
    } else if (key == Key::Up) {
        recent.focus = recent.focus == RecentState::Focus::Close ? RecentState::Focus::Card
                                                                 : RecentState::Focus::All;
    } else if (key == Key::Down)
        recent.focus = RecentState::Focus::Close;
    if ((key == Key::Ok || key == Key::Red) && !state_.busy) {
        std::string action = key == Key::Red || recent.focus == RecentState::Focus::Close ? "close"
                             : recent.focus == RecentState::Focus::All ? "closeAll"
                                                                       : "open";
        services_.cancel("recents");
        recent.revision = 0;
        execute(post(Operation::Recents,
                     fields({{"action", Json::text(action)},
                             {"id", Json::text(recent.cards[recent.selected].id)}})));
    }
}
} // namespace nativetv
