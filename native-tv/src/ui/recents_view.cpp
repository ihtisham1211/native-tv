// The recent apps, drawn over the page they were opened from: a veil, one row of app cards,
// Close under the focused one and Close all above the row.
#include "ui/application.hpp"
#include "ui/home_layout.hpp"
#include <algorithm>

namespace nativetv {
namespace {
constexpr float kCardW = 320, kCardH = 180, kGap = 24, kCardsTop = 340;

// A pill that fills with the focus colour: Close all and Close.
void pill(kit::Paint paint, Rect rest, const std::string &label, const std::string &id,
          float focus) {
    kit::focus::Look look = kit::focus::lookAt(rest, 1.06f, focus);
    Scene &scene = paint.scene;
    Transform scale = kit::focus::transformAt(rest, look.scale);
    scene.pushTransform(scale);
    Node &base = scene.shape(rest, kit::focus::mix(kit::color::surface2, kit::color::focusFill, focus),
                             rest.h / 2);
    if (focus > 0) {
        base.ring = {0, kit::elevation::focusRingWidth,
                     kit::focus::withAlpha(kit::elevation::focusRingColor, focus * .6f)};
        base.shadow = {0, look.shadowDy, look.shadowBlur, 0, {0, 0, 0, look.shadowAlpha}};
    }
    paint.glyphs.text(scene, {rest.x, rest.y + (rest.h - kit::type::labelL.line) / 2, rest.w,
                              kit::type::labelL.line},
                      label, kit::type::labelL,
                      kit::focus::inkAt(kit::color::onSurface, kit::color::onFocus, focus),
                      Node::Align::Center);
    scene.popTransform();
    scene.hits.push_back({look.rect, id});
}
} // namespace

void drawRecents(kit::Paint paint, const Frame &frame) {
    const RecentState &recent = frame.state.recents;
    const Motion &motion = frame.motion;
    Scene &scene = paint.scene;
    float open = static_cast<float>(std::min(1.0, motion.value("rec")));
    if (open <= .002f)
        return;
    scene.shape({0, 0, kit::canvas::width, kit::canvas::height},
                kit::focus::withAlpha(kit::color::ink, .97f * open));
    scene.pushOpacity(open);
    scene.pushTransform(Transform::translate(0, 24 * (1 - open)));
    paint.glyphs.text(scene, kit::geometry::pageTitle(1000), "Recent apps", kit::type::headline,
                      kit::color::onSurface);
    if (recent.cards.empty()) {
        std::string line = recent.status.find("Loading") == 0     ? recent.status
                           : recent.status.find("Could not") == 0 ? "Recent apps are unavailable"
                                                                  : "Nothing is open";
        paint.glyphs.text(scene, kit::geometry::pageSubtitle(1400), line, kit::type::body,
                          kit::color::onSurface2);
        if (recent.status.find("Loading") != 0 && !recent.status.empty() &&
            recent.status != "Apps stay here while they are open.")
            paint.glyphs.text(scene, {kit::canvas::safeX, 280, 1400, kit::type::body.line},
                              recent.status, kit::type::body, kit::color::onSurface3);
    } else {
        paint.glyphs.text(scene, kit::geometry::pageSubtitle(1000),
                          std::to_string(recent.cards.size()) + " open", kit::type::body,
                          kit::color::onSurface2);
        pill(paint, {kit::canvas::width - kit::canvas::safeX - 200, 176, 200, 56}, "Close all",
             "recents-all", static_cast<float>(motion.value("rfa")));
    }
    const float scroll = static_cast<float>(motion.value("rcx"));
    for (size_t i = 0; i < recent.cards.size(); ++i) {
        const Card &app = recent.cards[i];
        Rect rest = {kit::canvas::safeX + static_cast<float>(i) * (kCardW + kGap) + scroll,
                     kCardsTop, kCardW, kCardH};
        if (rest.x + rest.w < -60 || rest.x > kit::canvas::width + 60)
            continue;
        float focus = static_cast<float>(motion.value(home::id("rf", i)));
        kit::PosterCard card;
        card.rest = rest;
        card.id = "recent:" + std::to_string(i);
        card.image = app.image;
        card.focus = focus;
        kit::card(paint, card);
        if (app.image.empty()) {
            // No snapshot: the app's initial on the card.
            size_t end = std::min<size_t>(1, app.title.size());
            while (end < app.title.size() &&
                   (static_cast<unsigned char>(app.title[end]) & 0xc0) == 0x80)
                ++end;
            scene.pushTransform(kit::focus::transformAt(
                rest, kit::focus::scaleAt(kit::motion::scale::card, focus)));
            paint.glyphs.text(scene, {rest.x, rest.y + (rest.h - kit::type::display.line) / 2,
                                      rest.w, kit::type::display.line},
                              app.title.substr(0, end), kit::type::display, kit::color::onSurface2,
                              Node::Align::Center);
            scene.popTransform();
        }
        paint.glyphs.text(scene, {rest.x, rest.y + rest.h + 28, rest.w, kit::type::labelL.line},
                          app.title, kit::type::labelL,
                          kit::focus::withAlpha(kit::color::onSurface, .6f + .4f * focus));
        if (focus > .004f)
            pill(paint, {rest.x + (rest.w - 140) / 2, rest.y + rest.h + 72, 140, 48}, "Close",
                 "recent-close:" + std::to_string(i), static_cast<float>(motion.value("rfx")));
    }
    scene.popTransform();
    scene.popOpacity();
    paint.glyphs.text(scene, {kit::canvas::safeX, 980, 1400, kit::type::labelM.line},
                      "Back to return  ·  Red closes the focused app", kit::type::labelM,
                      kit::color::onSurface3);
}
} // namespace nativetv
