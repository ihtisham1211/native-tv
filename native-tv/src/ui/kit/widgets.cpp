#include "ui/kit/widgets.hpp"
#include "ui/kit/overlay.hpp"
#include <algorithm>
#include <cmath>

namespace nativetv {
namespace kit {
namespace {
constexpr Color white = {1, 1, 1, 1};
constexpr Color clear = {0, 0, 0, 0};
// The avatar initial and the focused-row ink on a light fill have no token of their own.
constexpr TypeStyle avatarStyle = {24, 32, 600, 0};
constexpr Color onFocusFaint = {19 / 255.0f, 17 / 255.0f, 24 / 255.0f, .12f};
constexpr Color onFocusRing = {19 / 255.0f, 17 / 255.0f, 24 / 255.0f, .55f};
constexpr Color onFocusKnob = {19 / 255.0f, 17 / 255.0f, 24 / 255.0f, .6f};
// Anti-aliasing is half a pixel either side of an edge, so a fill pulled in by more than a pixel
// lies wholly under the opaque part of a picture drawn over it.
constexpr float rimTuck = 1.5f;

bool invisible(const Enter &enter) {
    return enter.alpha <= .002f;
}
bool offCanvas(Rect area) {
    return area.x + area.w < -120 || area.x > canvas::width + 120 || area.y + area.h < -120 ||
           area.y > canvas::height + 120;
}
Color faded(Color value, float alpha) {
    return focus::withAlpha(value, alpha);
}
float roundRadius(Rect area) {
    return std::min(radius::pill, std::min(area.w, area.h) / 2);
}

// A component's entrance and focus scale, applied to every node added while it lives.
class Group {
  public:
    Group(Scene &scene, const Enter &enter, const Transform *scale = nullptr)
        : scene_(scene), fade_(enter.alpha < 1), lift_(enter.dy != 0), scale_(scale != nullptr) {
        if (fade_)
            scene.pushOpacity(enter.alpha);
        if (lift_)
            scene.pushTransform(Transform::translate(0, enter.dy));
        if (scale_)
            scene.pushTransform(*scale);
    }
    ~Group() {
        if (scale_)
            scene_.popTransform();
        if (lift_)
            scene_.popTransform();
        if (fade_)
            scene_.popOpacity();
    }
    Group(const Group &) = delete;
    Group &operator=(const Group &) = delete;

  private:
    Scene &scene_;
    bool fade_, lift_, scale_;
};

// A filled circle of the given box.
void packed(Scene &scene, Rect area, Color fill) {
    scene.shape(area, fill, area.h / 2);
}

// Focus ring and shadow on the node itself; both fade with focus and vanish at 0.
void applyFocus(Node &node, const focus::Look &look) {
    if (look.ringAlpha <= 0)
        return;
    node.ring = {0, look.ringWidth, faded(elevation::focusRingColor, look.ringAlpha)};
    node.shadow = {0, look.shadowDy, look.shadowBlur, 0, {0, 0, 0, look.shadowAlpha}};
}
// For a node that already carries its resting outline: the ring goes on a clear overlay, so the
// shadow stays under the opaque fill.
void focusRing(Scene &scene, Rect area, float rounding, float amount) {
    if (amount <= 0)
        return;
    Node &overlay = scene.shape(area, clear, rounding);
    overlay.ring = {0, elevation::focusRingWidth, faded(elevation::focusRingColor, amount)};
}
void restOutline(Node &node) {
    node.ring = {1, 0, alpha::outline};
}

struct Stop {
    Color color;
    float at;
};
// A CSS gradient whose last stop may lie past the box (140%): stops beyond 1 are cut there.
Gradient gradient(float angle, std::initializer_list<Stop> stops) {
    Gradient result;
    result.angle = angle;
    const Stop *previous = nullptr;
    for (const Stop &stop : stops) {
        if (result.stops >= Gradient::kMaxStops)
            break;
        if (stop.at > 1 && previous) {
            float amount = (1 - previous->at) / (stop.at - previous->at);
            result.color[result.stops] = focus::mix(previous->color, stop.color, amount);
            result.at[result.stops++] = 1;
            break;
        }
        result.color[result.stops] = stop.color;
        result.at[result.stops++] = stop.at;
        previous = &stop;
    }
    return result;
}
Color withByteAlpha(Color value, int alphaByte) {
    return faded(value, alphaByte / 255.0f);
}

void hit(Scene &scene, Rect area, const std::string &id) {
    if (!id.empty())
        scene.hits.push_back({area, id});
}
void text(Paint paint, Rect box, const std::string &value, const TypeStyle &style, Color ink,
          Node::Align align = Node::Align::Left, int maxLines = 1) {
    paint.glyphs.text(paint.scene, box, value, style, ink, align, maxLines);
}
// A title's facts line, "Film  ·  2026  ·  ★ 6.4": the face has no star, so the "★ " is drawn
// as the Material icon at the type size between two runs of text.
// A line that is too wide for its box loses whole items from its end ("Series · 2023 · ★ 7.4"),
// never half of one.
void factsText(Paint paint, Rect box, const std::string &full, const TypeStyle &style,
               Color ink) {
    const std::string star = "\xe2\x98\x85 ", separator = "  \xc2\xb7  ";
    auto width = [&](const std::string &line) {
        size_t mark = line.find(star);
        if (mark == std::string::npos)
            return paint.glyphs.measure(line, style);
        return paint.glyphs.measure(line.substr(0, mark), style) + style.px * 1.18f +
               paint.glyphs.measure(line.substr(mark + star.size()), style);
    };
    std::string facts = full;
    while (width(facts) > box.w) {
        size_t cut = facts.rfind(separator);
        if (cut == std::string::npos)
            break;
        facts.erase(cut);
    }
    size_t at = facts.find(star);
    if (at == std::string::npos) {
        text(paint, box, facts, style, ink);
        return;
    }
    std::string before = facts.substr(0, at), after = facts.substr(at + star.size());
    float x = box.x;
    if (!before.empty()) {
        text(paint, box, before, style, ink);
        x += paint.glyphs.measure(before, style);
    }
    float side = style.px;
    paint.glyphs.icon(paint.scene, {x, box.y + (box.h - side) / 2, side, side}, "star", ink);
    x += side + style.px * .18f;
    text(paint, {x, box.y, box.x + box.w - x, box.h}, after, style, ink);
}
// An icon square centred on a point.
Rect centred(float cx, float cy, float side) {
    return {cx - side / 2, cy - side / 2, side, side};
}
Rect centreOf(Rect area, float side) {
    return centred(area.x + area.w / 2, area.y + area.h / 2, side);
}
} // namespace

void factsLine(Paint paint, Rect box, const std::string &facts, const TypeStyle &style, Color ink) {
    factsText(paint, box, facts, style, ink);
}

// ---- Text and icons over the legacy nodes -----------------------------------------------------

float LegacyGlyphs::measure(const std::string &value, const TypeStyle &style) const {
    size_t characters = 0;
    for (unsigned char c : value)
        if ((c & 0xc0) != 0x80)
            ++characters;
    return characters * style.px * (.6f + style.tracking);
}
void LegacyGlyphs::text(Scene &scene, Rect box, const std::string &value, const TypeStyle &style,
                        Color color, Node::Align align, int maxLines) {
    if (value.empty())
        return;
    box.h = style.line * maxLines;
    Node &node = scene.shape(box, color);
    node.kind = Node::Kind::Text;
    node.value = value;
    node.font = static_cast<int>(style.px);
    node.lineHeight = static_cast<int>(style.line);
    node.weight = style.weight;
    node.align = align;
}
void LegacyGlyphs::icon(Scene &scene, Rect box, const std::string &name, Color tint) {
    if (name.empty())
        return;
    Node &node = scene.picture(box, 0, 0, true, tint);
    node.value = "local:icons/material/" + name + ".svg";
}

// ---- Top bar ------------------------------------------------------------------------------------

std::vector<Rect> tabRects(const Glyphs &glyphs, const std::vector<std::string> &tabs) {
    std::vector<float> widths;
    for (const auto &tab : tabs)
        widths.push_back(glyphs.measure(tab, type::nav));
    return geometry::tabRects(widths);
}
void tabPill(Paint paint, float left, float right, float hot, float alphaFactor) {
    if (right - left < 1 || alphaFactor <= .002f)
        return;
    Rect area = geometry::pill(left, right);
    paint.scene.shape(area, faded(focus::mix(color::pillRest, color::focusFill, hot), alphaFactor),
                      roundRadius(area));
}
void roundButton(Paint paint, const RoundButton &button) {
    if (invisible(button.enter) || offCanvas(button.rest))
        return;
    Scene &scene = paint.scene;
    focus::Look look = focus::lookAt(button.rest, motion::scale::circle, button.focus);
    Transform scale = focus::transformAt(button.rest, look.scale);
    Group group(scene, button.enter, look.scale != 1 ? &scale : nullptr);
    float rounding = button.rest.h / 2;
    Node &base = scene.shape(button.rest, button.fill, rounding);
    if (button.avatar)
        base.gradient =
            gradient(135, {{rgb(0xF2B27A), 0}, {rgb(0xB05E8A), .55f}, {rgb(0x5C5AA8), 1}});
    if (button.focus > 0)
        scene.shape(button.rest, faded(color::focusFill, focus::clamp01(button.focus)), rounding);
    Color ink = focus::inkAt(button.ink, color::onFocus, button.focus);
    paint.glyphs.icon(scene, centreOf(button.rest, 28), button.icon, ink);
    text(paint,
         {button.rest.x, button.rest.y + (button.rest.h - avatarStyle.line) / 2, button.rest.w,
          avatarStyle.line},
         button.text, avatarStyle, ink, Node::Align::Center);
    hit(scene, look.rect, button.id);
}
void topBar(Paint paint, const TopBar &bar) {
    if (invisible(bar.enter))
        return;
    Scene &scene = paint.scene;
    Group group(scene, bar.enter);
    tabPill(paint, bar.pillLeft, bar.pillRight, bar.pillHot, bar.pillAlpha);

    RoundButton avatar;
    avatar.rest = geometry::avatar();
    avatar.id = "nav:avatar";
    avatar.text = bar.avatarInitial;
    avatar.avatar = true;
    avatar.focus = bar.avatarFocus;
    roundButton(paint, avatar);

    std::vector<Rect> tabs = tabRects(paint.glyphs, bar.tabs);
    for (size_t i = 0; i < tabs.size(); ++i) {
        // Selected: on-surface at rest, on-focus once the pill turns to focus fill.
        Color ink = static_cast<int>(i) == bar.selected
                        ? focus::inkAt(color::onSurface, color::onFocus, bar.pillHot)
                        : color::onSurface2;
        text(paint,
             {tabs[i].x, layout::navTop + (layout::navH - type::nav.line) / 2, tabs[i].w,
              type::nav.line},
             bar.tabs[i], type::nav, ink, Node::Align::Center);
        hit(scene, tabs[i], "nav:tab:" + std::to_string(i));
    }

    RoundButton settings;
    settings.rest = geometry::settingsButton();
    settings.id = "nav:settings";
    settings.icon = "settings";
    settings.fill = bar.settingsSelected ? alpha::accentDim : color::surface3;
    settings.ink = bar.settingsSelected ? color::accent : color::onSurface;
    settings.focus = bar.settingsFocus;
    roundButton(paint, settings);

    text(paint, geometry::clock(), bar.clock, type::clock, color::onSurface, Node::Align::Right);
}

// ---- Hero and pagination -----------------------------------------------------------------------

void heroWall(Paint paint, const Hero &hero) {
    Scene &scene = paint.scene;
    geometry::HeroParallax parallax = geometry::heroParallax(hero.stackY);
    Rect screen = {0, 0, canvas::width, canvas::height};
    if (hero.wallAlpha > .002f) {
        scene.pushOpacity(hero.wallAlpha);
        {
            scene.pushTransform(Transform::translate(0, parallax.artY));
            // The art grows past the screen while it pushes in.
            scene.pushClip(screen);
            for (const HeroSlot &slot : hero.slot) {
                if (slot.opacity <= .002f)
                    continue;
                float grow = 1 + .09f * focus::clamp01(slot.kenBurns);
                scene.pushOpacity(slot.opacity);
                scene.pushTransform(Transform::scaleAbout(.7f * canvas::width, .5f * canvas::height,
                                                          grow, grow, -18 * slot.kenBurns,
                                                          -6 * slot.kenBurns));
                if (!slot.image.empty()) {
                    if (slot.artAlpha < 1)
                        scene.pushOpacity(slot.artAlpha);
                    scene.picture(screen, 0).value = slot.image;
                    if (slot.artAlpha < 1)
                        scene.popOpacity();
                }
                scene.popTransform();
                // The prototype's radial tint, as a linear fade from the bottom-left corner.
                Node &tint = scene.shape(screen, clear);
                tint.gradient =
                    gradient(55, {{faded(slot.tint, 1), 0}, {faded(slot.tint, 0), .7f}});
                scene.popOpacity();
            }
            scene.popClip();
            scene.popTransform();
        }
        // Three static layers keep the text legible over any art.
        Color shade = color::ink;
        Node &bottom = scene.shape(screen, clear);
        bottom.gradient = gradient(0, {{faded(shade, .97f), 0},
                                       {faded(shade, .78f), .22f},
                                       {faded(shade, .30f), .46f},
                                       {faded(shade, 0), .64f}});
        Node &left = scene.shape(screen, clear);
        left.gradient = gradient(90, {{faded(shade, .60f), 0}, {faded(shade, 0), .58f}});
        Node &top = scene.shape(screen, clear);
        // Strong enough that the search and settings buttons keep 3:1 over bright art.
        top.gradient = gradient(180, {{faded(shade, .72f), 0}, {faded(shade, 0), .26f}});
        scene.popOpacity();
    }
    // Rises with the rows so cards stay legible once the hero has scrolled away.
    if (parallax.scrimOpacity > .002f)
        scene.shape(screen, faded(alpha::scrimStrong, parallax.scrimOpacity));
}
void ambientWall(Paint paint, float amount, Color tint) {
    if (amount <= .002f)
        return;
    Scene &scene = paint.scene;
    Rect screen = {0, 0, canvas::width, canvas::height};
    scene.pushOpacity(amount);
    // Radial washes in the prototype: accent from the top right, the tint from the bottom left.
    Node &glow = scene.shape(screen, clear);
    glow.gradient =
        gradient(225, {{faded(color::accent, .10f), 0}, {faded(color::accent, 0), .6f}});
    Node &wash = scene.shape(screen, clear);
    wash.gradient = gradient(45, {{faded(tint, 1), 0}, {faded(tint, 0), .7f}});
    scene.popOpacity();
}
void heroText(Paint paint, const Hero &hero) {
    Scene &scene = paint.scene;
    geometry::HeroParallax parallax = geometry::heroParallax(hero.stackY);
    if (parallax.textAlpha <= .002f)
        return;
    Group page(scene, {parallax.textAlpha, parallax.textY});

    // An app's icon stands left of its lines, so they all move right by its width.
    const float dx = hero.image.empty() ? 0 : layout::heroArt + layout::heroArtGap;
    auto beside = [dx](Rect box) {
        box.x += dx;
        box.w -= dx;
        return box;
    };
    Rect source = beside(geometry::heroSource());
    if (!hero.image.empty() && !invisible(hero.text[1])) {
        Group group(scene, hero.text[1]);
        float top = geometry::heroSource().y,
              bottom = geometry::heroSubtitle().y + layout::heroSubH;
        Rect art = {canvas::safeX, top + (bottom - top - layout::heroArt) / 2, layout::heroArt,
                    layout::heroArt};
        Node &picture = scene.picture(art, 0, layout::heroArt / 2);
        picture.value = hero.image;
        picture.shadow = {0, elevation::raised.dy, elevation::raised.blur, 0,
                          {0, 0, 0, elevation::raised.alpha}};
    }
    if (!invisible(hero.text[0]) && !hero.source.empty()) {
        Group group(scene, hero.text[0]);
        float x = source.x;
        // The play mark leads a title's source line only.
        if (hero.image.empty()) {
            Rect mark = {source.x, source.y + (source.h - layout::heroMark) / 2, layout::heroMark,
                         layout::heroMark};
            scene.shape(mark, alpha::accentDim, layout::heroMark / 2);
            paint.glyphs.icon(scene, centreOf(mark, 18), "play", color::accent);
            x += layout::heroMark + layout::heroMarkGap;
        }
        text(paint, {x, source.y, source.w - (x - source.x), source.h}, hero.source,
             type::heroSource, color::onSurface2);
    }
    if (!invisible(hero.text[1])) {
        Group group(scene, hero.text[1]);
        text(paint, beside(geometry::heroTitle()), hero.title, type::display, color::onSurface);
    }
    if (!invisible(hero.text[2])) {
        Group group(scene, hero.text[2]);
        if (!hero.facts.empty())
            factsText(paint, beside(geometry::heroSubtitle()), hero.facts, type::labelL,
                      color::onSurface2);
        Rect body = beside(geometry::heroDescription());
        if (!hero.description.empty())
            text(paint, body, hero.description, type::bodyL, color::onSurface2, Node::Align::Left,
                 2);
        else if (!hero.hint.empty())
            keyHint(paint, body.x, body.y + 8, "OK", hero.hint);
    }
}
void pagination(Paint paint, const Pagination &dots) {
    if (dots.count <= 0 || dots.alpha <= .002f)
        return;
    Scene &scene = paint.scene;
    Group group(scene, {dots.alpha, dots.y});
    float progress = focus::clamp01(dots.progress);
    for (int i = 0; i < dots.count; ++i) {
        Rect slot = geometry::dotSlot(i, dots.count);
        float current =
            i < static_cast<int>(dots.current.size()) ? focus::clamp01(dots.current[i]) : 0;
        float middle = slot.y + (slot.h - layout::dot) / 2;
        if (current < 1)
            scene.shape({slot.x + (slot.w - layout::dot) / 2, middle, layout::dot, layout::dot},
                        faded(color::onSurface, .38f * (1 - current)), layout::dot / 2);
        if (current > 0) {
            Rect track = {slot.x, middle, slot.w, layout::dot};
            scene.shape(track, faded(color::onSurface, .22f * current), layout::dot / 2);
            if (progress > 0) {
                Node &fill = scene.shape({slot.x, middle, slot.w * progress, layout::dot},
                                         faded(color::onSurface, current));
                float end = progress >= .999f ? layout::dot / 2 : 0;
                fill.corners = {layout::dot / 2, end, end, layout::dot / 2};
            }
        }
    }
}

// ---- Home rows
// ------------------------------------------------------------------------------------

void row(Paint paint, const Row &row) {
    if (invisible(row.enter))
        return;
    Group group(paint.scene, row.enter);
    text(paint, geometry::rowLabel(row.index), row.label, type::title,
         faded(color::onSurface2, focus::dimmedRowAlpha(row.dim)));
}
void card(Paint paint, const PosterCard &poster) {
    focus::Look look = focus::lookAt(poster.rest, motion::scale::card, poster.focus);
    if (invisible(poster.enter) || offCanvas(look.rect))
        return;
    Scene &scene = paint.scene;
    Transform scale = focus::transformAt(poster.rest, look.scale);
    Group group(scene, poster.enter, look.scale != 1 ? &scale : nullptr);
    applyFocus(scene.shape(poster.rest, color::surface2, radius::md), look);
    if (!poster.image.empty())
        scene.picture(poster.rest, 0, radius::md).value = poster.image;
    if (!poster.title.empty()) {
        // The title sits on a veil over the lower part of the art; on focus it rises to make
        // room for the facts line under it.
        float amount = focus::clamp01(poster.focus);
        Rect lower = {poster.rest.x, poster.rest.y + poster.rest.h * .4f, poster.rest.w,
                      poster.rest.h * .6f};
        Node &veil = scene.shape(lower, clear);
        veil.corners = {0, 0, radius::md, radius::md};
        veil.gradient = gradient(180, {{{0, 0, 0, 0}, 0}, {{0, 0, 0, .78f}, 1}});
        float bottom = poster.rest.y + poster.rest.h - 10, left = poster.rest.x + 16,
              width = poster.rest.w - 32;
        float factsHeight = poster.subtitle.empty() ? 0 : type::labelM.line * amount;
        text(paint, {left, bottom - factsHeight - type::cardTitle.line, width, type::cardTitle.line},
             poster.title, type::cardTitle, white);
        if (factsHeight > 0)
            factsText(paint, {left, bottom - type::labelM.line, width, type::labelM.line},
                      poster.subtitle, type::labelM, faded(color::onSurface2, amount));
    }
    hit(scene, look.rect, poster.id);
}
void appCircle(Paint paint, const AppCircle &circle) {
    focus::Look look = focus::lookAt(circle.rest, motion::scale::app, circle.focus);
    if (invisible(circle.enter) || offCanvas(look.rect))
        return;
    Scene &scene = paint.scene;
    Transform scale = focus::transformAt(circle.rest, look.scale);
    Group group(scene, circle.enter, look.scale != 1 ? &scale : nullptr);
    float rounding = circle.rest.h / 2;
    // The ring and the shadow belong to the circle, not to its fill, which is hidden once an
    // opaque picture covers it (Node::backing).
    if (look.ringAlpha > 0)
        applyFocus(scene.shape(circle.rest, clear, rounding), look);
    // The fill and the picture share one anti-aliased edge, so a fill in the app's colour shows
    // through the picture's rim as a light outline (LG gives Netflix, YouTube and Alexa white).
    // Under a picture the fill stops short of the edge, which then comes from the picture alone.
    const float tuck = circle.image.empty() ? 0 : rimTuck;
    Rect fill = {circle.rest.x + tuck, circle.rest.y + tuck, circle.rest.w - 2 * tuck,
                 circle.rest.h - 2 * tuck};
    Node &base = scene.shape(fill, circle.color, rounding - tuck);
    base.gradient = gradient(145, {{circle.color, 0},
                                   {withByteAlpha(circle.color, 0xcc), .55f},
                                   {{0, 0, 0, 0x55 / 255.0f}, 1.4f}});
    base.backing = !circle.image.empty();
    if (!circle.image.empty())
        scene.picture(circle.rest, 0, rounding).value = circle.image;
    else
        paint.glyphs.icon(scene, centreOf(circle.rest, 52), circle.icon, white);
    // The name appears under the focused circle, rising into place; a named circle keeps it,
    // and only brightens it on focus.
    if (circle.named || circle.focus > .004f) {
        float cx = circle.rest.x + circle.rest.w / 2,
              width = circle.nameWidth > 0 ? circle.nameWidth : circle.rest.w + 104;
        float amount = focus::clamp01(circle.focus);
        float rise = circle.named ? 0 : focus::labelRise(amount);
        Color ink = circle.named ? focus::inkAt(color::onSurface2, color::onSurface, amount)
                                 : faded(color::onSurface, amount);
        // The gap under the circle is the same at every size: appNameTop is measured for the
        // 112 px circle, the Apps grid's are 156 px.
        float nameTop = circle.rest.h + (layout::appNameTop - layout::appD);
        text(paint, {cx - width / 2, circle.rest.y + nameTop + rise, width, type::labelM.line},
             circle.name, type::labelM, ink, Node::Align::Center);
    }
    hit(scene, look.rect, circle.id);
}
void inputTile(Paint paint, const InputTile &tile) {
    focus::Look look = focus::lookAt(tile.rest, motion::scale::input, tile.focus);
    if (invisible(tile.enter) || offCanvas(look.rect))
        return;
    Scene &scene = paint.scene;
    Transform scale = focus::transformAt(tile.rest, look.scale);
    Enter enter = tile.enter;
    if (tile.unused)
        enter.alpha *= .62f;
    Group group(scene, enter, look.scale != 1 ? &scale : nullptr);
    Node &base = scene.shape(tile.rest, color::surface2, radius::md);
    restOutline(base);
    if (look.ringAlpha > 0)
        base.shadow = {0, look.shadowDy, look.shadowBlur, 0, {0, 0, 0, look.shadowAlpha}};
    focusRing(scene, tile.rest, radius::md, look.ringAlpha);
    Rect box = tile.rest;
    paint.glyphs.icon(scene, {box.x + 20, box.y + 18, 34, 34}, tile.icon, color::onSurface);
    float bottom = box.y + box.h - 18;
    float subY = bottom - type::labelS.line, nameY = subY - 2 - type::inputName.line;
    text(paint, {box.x + 20, nameY, box.w - 40, type::inputName.line}, tile.name, type::inputName,
         color::onSurface);
    text(paint, {box.x + 20, subY, box.w - 40, type::labelS.line}, tile.sub, type::labelS,
         color::onSurface3);
    if (tile.live)
        scene.shape({box.x + box.w - 16 - 10, box.y + 16, 10, 10}, color::accent, 5);
    hit(scene, look.rect, tile.id);
}

// ---- Apps and Live
// -------------------------------------------------------------------------------

void pageHead(Paint paint, const PageHead &head) {
    if (invisible(head.enter))
        return;
    Group group(paint.scene, head.enter);
    float width = canvas::width - 2 * canvas::safeX;
    text(paint, geometry::pageTitle(width), head.title, type::headline, color::onSurface);
    text(paint, geometry::pageSubtitle(width), head.subtitle, type::body, color::onSurface2);
}
void appTile(Paint paint, const AppTile &tile) {
    focus::Look look = focus::lookAt(tile.rest, motion::scale::tile, tile.focus);
    if (invisible(tile.enter) || offCanvas(look.rect))
        return;
    Scene &scene = paint.scene;
    Transform scale = focus::transformAt(tile.rest, look.scale);
    Group group(scene, tile.enter, look.scale != 1 ? &scale : nullptr);
    Node &base = scene.shape(tile.rest, tile.color, radius::md);
    // A banner is one flat colour: shading would turn a white tile (Netflix, YouTube) grey.
    if (!tile.centred)
        base.gradient = gradient(150, {{tile.color, 0},
                                       {withByteAlpha(tile.color, 0xb0), .6f},
                                       {withByteAlpha(color::ink, 0x99), 1.4f}});
    applyFocus(base, look);
    if (tile.centred) {
        // No name on the tile: the app's art says it, and the page names the focused one.
        const bool light = .2126f * tile.color.r + .7152f * tile.color.g + .0722f * tile.color.b > .6f;
        paint.glyphs.icon(scene, {tile.rest.x + (tile.rest.w - 56) / 2, tile.rest.y + (tile.rest.h - 56) / 2, 56, 56},
                          tile.icon, light ? color::ink : white);
        hit(scene, look.rect, tile.id);
        return;
    }
    paint.glyphs.icon(scene, {tile.rest.x + layout::tilePad, tile.rest.y + layout::tilePad, 40, 40},
                      tile.icon, white);
    text(paint,
         {tile.rest.x + layout::tilePad,
          tile.rest.y + tile.rest.h - layout::tilePad - type::labelL.line,
          tile.rest.w - 2 * layout::tilePad, type::labelL.line},
         tile.name, type::labelL, white);
    hit(scene, look.rect, tile.id);
}

void sparkline(Paint paint, Rect area, const std::vector<float> &samples, float shift) {
    if (samples.size() < 2)
        return;
    constexpr float column = 6, stroke = 3;
    Scene &scene = paint.scene;
    float lastIndex = static_cast<float>(samples.size() - 1);
    float step = area.w / lastIndex;
    auto heightAt = [&](float x) {
        float position = std::min(lastIndex, std::max(0.0f, (x - area.x) / step + shift));
        size_t low = static_cast<size_t>(position);
        size_t high = std::min(samples.size() - 1, low + 1);
        float value = focus::mix(samples[low], samples[high], position - static_cast<float>(low));
        return area.y + area.h - stroke / 2 - focus::clamp01(value) * (area.h - stroke);
    };
    for (float x = area.x; x < area.x + area.w - .5f; x += column) {
        float left = heightAt(x), right = heightAt(x + column);
        float top = std::min(left, right), bottom = std::max(left, right);
        Node &fill = scene.shape({x, top, column, area.y + area.h - top}, clear);
        float strength = .28f * (1 - (top - area.y) / area.h);
        fill.gradient =
            gradient(180, {{faded(color::accent, strength), 0}, {faded(color::accent, 0), 1}});
        scene.shape({x, top - stroke / 2, column, std::max(stroke, bottom - top + stroke)},
                    color::accent, stroke / 2);
    }
}
void statCard(Paint paint, const StatCard &stat) {
    focus::Look look = focus::lookAt(stat.rest, motion::scale::stat, stat.focus);
    if (invisible(stat.enter) || offCanvas(look.rect))
        return;
    Scene &scene = paint.scene;
    Transform scale = focus::transformAt(stat.rest, look.scale);
    Group group(scene, stat.enter, look.scale != 1 ? &scale : nullptr);
    Rect box = stat.rest;
    Node &base = scene.shape(box, color::surface1, radius::lg);
    restOutline(base);
    if (look.ringAlpha > 0)
        base.shadow = {0, look.shadowDy, look.shadowBlur, 0, {0, 0, 0, look.shadowAlpha}};
    focusRing(scene, box, radius::lg, look.ringAlpha);

    float left = box.x + layout::statPadX, inner = box.w - 2 * layout::statPadX;
    Rect well = {left, box.y + layout::statPadTop, layout::well, layout::well};
    scene.shape(well, color::surface3, layout::well / 2);
    paint.glyphs.icon(scene, centreOf(well, 24), stat.icon, color::onSurface);
    text(paint,
         {well.x + well.w + 14, well.y + (well.h - type::labelL.line) / 2, inner - well.w - 14,
          type::labelL.line},
         stat.label, type::labelL, color::onSurface2);

    const TypeStyle &valueStyle = stat.textValue ? type::numeralText : type::numeral;
    Rect valueBox = {left, box.y + layout::statValueTop, inner, valueStyle.line};
    text(paint, valueBox, stat.value, valueStyle, color::onSurface);
    if (!stat.unit.empty()) {
        // The unit shares the numeral's baseline, 33 px lower in its own box.
        float x = left + paint.glyphs.measure(stat.value, valueStyle) + 8;
        text(paint, {x, valueBox.y + 33, left + inner - x, type::numeralUnit.line}, stat.unit,
             type::numeralUnit, color::onSurface2);
    }
    float captionY = box.y + layout::statCaptionTop;
    text(paint, {left, captionY, inner, type::labelM.line}, stat.caption, type::labelM,
         color::onSurface3);
    text(paint, {left, captionY + type::labelM.line + 6, inner, type::labelM.line}, stat.caption2,
         type::labelM, color::onSurface3);

    float bottom = box.y + box.h - layout::statBottom;
    if (stat.bar >= 0) {
        Rect track = {left, bottom - layout::barH, inner, layout::barH};
        scene.shape(track, color::surface3, layout::barH / 2);
        float fraction = focus::clamp01(stat.bar);
        if (fraction > 0) {
            Node &fill = scene.shape({track.x, track.y, track.w * fraction, track.h},
                                     stat.warn ? color::warn : color::accent);
            float end = fraction >= .999f ? layout::barH / 2 : 0;
            fill.corners = {layout::barH / 2, end, end, layout::barH / 2};
        }
    }
    sparkline(paint, {left, bottom - 56, inner, 56}, stat.spark, stat.sparkShift);
    if (!stat.days.empty()) {
        float gap = 6, count = static_cast<float>(stat.days.size());
        float width = (inner - gap * (count - 1)) / count;
        for (size_t i = 0; i < stat.days.size(); ++i)
            scene.shape({left + static_cast<float>(i) * (width + gap), bottom - 36, width, 36},
                        stat.days[i] ? faded(color::accent, .85f) : color::surface3,
                        radius::sm / 2);
    }
    hit(scene, look.rect, stat.id);
}

// ---- Settings
// ------------------------------------------------------------------------------------

void toggle(Paint paint, Rect rect, float on, float rowFocus) {
    Scene &scene = paint.scene;
    on = focus::clamp01(on);
    float rounding = rect.h / 2;
    Node &ring = scene.shape(rect, clear, rounding);
    ring.ring = {layout::toggleRing, 0, focus::inkAt(color::onSurface3, onFocusRing, rowFocus)};
    if (on > 0)
        scene.shape(rect, faded(color::accent, on), rounding);
    Color off = focus::inkAt(color::onSurface3, onFocusKnob, rowFocus);
    Color knobInk = focus::mix(off, color::onAccent, on);
    float grow = 1 + .18f * on;
    Rect knob = {rect.x + layout::toggleInset + layout::toggleTravel * on,
                 rect.y + layout::toggleInset, layout::toggleKnob, layout::toggleKnob};
    scene.shape(focus::scaledAbout(knob, grow), knobInk, layout::toggleKnob * grow / 2);
}
void settingsRow(Paint paint, const SettingsRow &item) {
    focus::Look look = focus::lookAt(item.rest, motion::scale::row, item.focus, focus::leftCentre);
    if (invisible(item.enter) || offCanvas(look.rect))
        return;
    Scene &scene = paint.scene;
    Transform scale = focus::transformAt(item.rest, look.scale, focus::leftCentre);
    Group group(scene, item.enter, look.scale != 1 ? &scale : nullptr);
    Rect box = item.rest;
    scene.shape(box, color::surface1, radius::lg);
    if (item.focus > 0)
        scene.shape(box, faded(color::focusFill, focus::clamp01(item.focus)), radius::lg);

    Rect well = {box.x + layout::srowPadLeft, box.y + (box.h - layout::srowWell) / 2,
                 layout::srowWell, layout::srowWell};
    scene.shape(well, focus::mix(color::surface3, onFocusFaint, item.focus), layout::srowWell / 2);
    paint.glyphs.icon(scene, centreOf(well, 26), item.icon,
                      focus::inkAt(color::onSurface, color::onFocus, item.focus));
    float titleX = well.x + well.w + layout::srowWellGap;
    text(paint,
         {titleX, box.y + (box.h - type::settingRow.line) / 2, box.w / 2, type::settingRow.line},
         item.title, type::settingRow, focus::inkAt(color::onSurface, color::onFocus, item.focus));
    if (item.toggle) {
        toggle(paint,
               {box.x + box.w - layout::srowPadRight - layout::toggleW,
                box.y + (box.h - layout::toggleH) / 2, layout::toggleW, layout::toggleH},
               item.on, item.focus);
    } else {
        // A stepper's value sits between two chevrons, so the owner can see it has others to go to.
        const float chevron = item.stepper ? 34.0f : 0.0f;
        float width = box.w / 3;
        const float right = box.x + box.w - layout::srowPadRight;
        const Color ink = focus::inkAt(color::onSurface2, color::onFocus, item.focus);
        text(paint, {right - chevron - width, box.y + (box.h - type::labelL.line) / 2, width, type::labelL.line},
             item.value, type::labelL, ink, Node::Align::Right);
        if (item.stepper) {
            const float valueWidth = std::min(width, paint.glyphs.measure(item.value, type::labelL));
            const float fade = .5f + .5f * focus::clamp01(item.focus);
            const float middle = box.y + box.h / 2;
            paint.glyphs.icon(scene, centreOf({right - 22, middle - 11, 22, 22}, 22), "arrowright",
                              faded(ink, fade));
            paint.glyphs.icon(scene,
                              centreOf({right - chevron - valueWidth - 30, middle - 11, 22, 22}, 22),
                              "arrowleft", faded(ink, fade));
        }
    }
    hit(scene, look.rect, item.id);
}

float pillChip(Paint paint, float x, float y, const std::string &label, bool dot,
               float alphaFactor) {
    float width = paint.glyphs.measure(label, type::labelM) + 36 + (dot ? 18 : 0);
    Scene &scene = paint.scene;
    scene.shape({x, y, width, 40}, faded(color::surface3, alphaFactor), 20);
    float textX = x + 18;
    if (dot) {
        scene.shape({x + 18, y + 16, 8, 8}, faded(color::accent, alphaFactor), 4);
        textX += 18;
    }
    text(paint,
         {textX, y + (40 - type::labelM.line) / 2, width - (textX - x) - 18 + 8, type::labelM.line},
         label, type::labelM, faded(color::onSurface, alphaFactor));
    return width;
}
void detailPanel(Paint paint, const DetailPanel &panel) {
    if (invisible(panel.enter))
        return;
    Scene &scene = paint.scene;
    Group group(scene, panel.enter);
    Rect box = geometry::detailPanel();
    restOutline(scene.shape(box, color::surface1, radius::xl));

    float left = box.x + layout::panelPadX, inner = box.w - 2 * layout::panelPadX;
    Rect well = {left, box.y + layout::panelPadY, layout::panelWell, layout::panelWell};
    scene.shape(well, alpha::accentDim, layout::panelWell / 2);
    paint.glyphs.icon(scene, centreOf(well, 38), panel.icon, color::accent);
    float titleY = well.y + well.h + 24;
    text(paint, {left, titleY, inner, type::headline.line}, panel.title, type::headline,
         color::onSurface);
    int bodyLines = std::max(1, panel.bodyLines);
    text(paint, {left, titleY + type::headline.line + 12, 700, type::body.line * bodyLines},
         panel.body, type::body, color::onSurface2, Node::Align::Left, bodyLines);

    float x = left, y = box.y + 300 + std::max(0, bodyLines - 3) * type::body.line;
    for (const auto &label : panel.chips) {
        float width = paint.glyphs.measure(label, type::labelM) + 54;
        if (x + width > left + inner && x > left) {
            x = left;
            y += 50;
        }
        x += pillChip(paint, x, y, label) + 10;
    }

    // "OK" key cap and what OK does.
    if (panel.action.empty())
        return;
    float hintY = box.y + box.h - 36 - 30;
    float capWidth = std::max(30.0f, paint.glyphs.measure("OK", type::cap) + 24);
    scene.shape({left, hintY, capWidth, 30}, color::surface3, radius::sm);
    text(paint, {left, hintY + (30 - type::cap.line) / 2, capWidth, type::cap.line}, "OK",
         type::cap, color::onSurface2, Node::Align::Center);
    text(paint,
         {left + capWidth + 12, hintY + (30 - type::labelM.line) / 2, inner - capWidth - 12,
          type::labelM.line},
         panel.action, type::labelM, color::onSurface3);
}

// ---- Loading
// ----------------------------------------------------------------------------------------

void loadingIndicator(Paint paint, const Loading &loading) {
    if (invisible(loading.enter))
        return;
    constexpr int dots = 28;
    constexpr float radiusPx = 30, sweep = 4.71238898f, turn = 6.28318531f; // 270 degrees
    Scene &scene = paint.scene;
    Group group(scene, loading.enter);
    float centreX = loading.area.x + loading.area.w / 2,
          centreY = loading.area.y + loading.area.h / 2 - (loading.label.empty() ? 0 : 22);
    // The head leads the turn; the tail fades out behind it.
    for (int i = 0; i < dots; ++i) {
        float along = static_cast<float>(i) / (dots - 1);
        float angle = loading.phase * turn + along * sweep - 1.57079633f;
        float size = 3.0f + 4.0f * along;
        float strength = along * along;
        scene.shape({centreX + radiusPx * std::cos(angle) - size / 2,
                     centreY + radiusPx * std::sin(angle) - size / 2, size, size},
                    faded(color::accent, .06f + .94f * strength), size / 2);
    }
    if (!loading.label.empty())
        text(paint, {loading.area.x, centreY + radiusPx + 22, loading.area.w, type::body.line},
             loading.label, type::body, color::onSurface2, Node::Align::Center);
}

// ---- Keyboard
// -------------------------------------------------------------------------------------

void keyCap(Paint paint, const KeyCap &key) {
    float scaleNow = focus::keyScale(key.focus, key.press);
    if (invisible(key.enter) || offCanvas(key.rest))
        return;
    Scene &scene = paint.scene;
    Transform scale = focus::transformAt(key.rest, scaleNow);
    Group group(scene, key.enter, scaleNow != 1 ? &scale : nullptr);
    scene.shape(key.rest, key.accent ? color::accent : color::surface2, radius::key);
    if (key.latched)
        scene.shape(key.rest, alpha::accentDim, radius::key);
    if (key.focus > 0)
        scene.shape(key.rest, faded(color::focusFill, focus::clamp01(key.focus)), radius::key);
    Color ink =
        focus::inkAt(key.accent ? color::onAccent : color::onSurface, color::onFocus, key.focus);
    const TypeStyle &style = key.hint ? type::labelM : type::key;
    if (key.hint)
        ink = color::onSurface3;
    if (key.latched)
        ink = focus::inkAt(color::accent, color::onFocus, key.focus);
    float middle = key.rest.y + key.rest.h / 2;
    if (key.icon.empty()) {
        text(paint, {key.rest.x, middle - style.line / 2, key.rest.w, style.line}, key.label, style,
             ink, Node::Align::Center);
    } else {
        // Icon, then the label, centred as a pair.
        float labelWidth = key.label.empty() ? 0 : paint.glyphs.measure(key.label, style);
        float gap = labelWidth > 0 ? 10 : 0;
        float x = key.rest.x + (key.rest.w - (30 + gap + labelWidth)) / 2;
        paint.glyphs.icon(scene, centred(x + 15, middle, 30), key.icon, ink);
        if (labelWidth > 0)
            text(paint, {x + 30 + gap, middle - style.line / 2, labelWidth + 24, style.line},
                 key.label, style, ink);
    }
    hit(scene, focus::scaledAbout(key.rest, scaleNow), key.id);
}

void spinner(Paint paint, Rect box, double timeMs, Color ink, float alphaFactor) {
    constexpr int dots = 12;
    constexpr double turnMs = 1250;
    constexpr float pi = 3.14159265358979f;
    if (alphaFactor <= .002f)
        return;
    float head = static_cast<float>(std::fmod(timeMs, turnMs) / turnMs * dots);
    float radius = std::min(box.w, box.h) / 2, dot = radius * .24f;
    float cx = box.x + box.w / 2, cy = box.y + box.h / 2;
    for (int i = 0; i < dots; ++i) {
        // How far behind the head this dot is, in dots: the head is full, the tail a faint ring.
        float behind = std::fmod(head - static_cast<float>(i) + dots, static_cast<float>(dots));
        float strength = std::max(.14f, 1 - behind / 9);
        float angle = -pi / 2 + 2 * pi * static_cast<float>(i) / dots;
        float x = cx + std::cos(angle) * (radius - dot), y = cy + std::sin(angle) * (radius - dot);
        packed(paint.scene, {x - dot, y - dot, 2 * dot, 2 * dot}, faded(ink, strength * alphaFactor));
    }
}
} // namespace kit
} // namespace nativetv
