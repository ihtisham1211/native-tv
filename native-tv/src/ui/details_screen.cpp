// Title details, drawn on the kit over the title's backdrop. Reads and writes go to the routes the
// JavaScript page used (details, netflix, open); every reply is checked against the request it
// answers, so a title left behind cannot fill the one on screen.
#include "ui/details_screen.hpp"
#include "ui/application.hpp"
#include "domain/models.hpp"
#include "domain/say.hpp"
#include "ui/home_layout.hpp"
#include "ui/kit/overlay.hpp"
#include "ui/kit/tuning.hpp"
#include "ui/overlay_routes.hpp"
#include <algorithm>
#include <cstdlib>

namespace nativetv {
namespace {
namespace layout = kit::layout;
using kit::easing;
using say::msg;

constexpr size_t kMaxDepth = 8;
constexpr float kTextWidth = 1100, kSimilarW = layout::cardW, kSimilarH = layout::cardH,
                kSimilarGap = layout::cardGap, kSimilarTop = 784, kActionH = 64;
constexpr size_t kOverviewPageBytes = 1100;
constexpr const char *kDetails = "title:details", *kNetflix = "title:netflix", *kOpen = "title:open";

std::string failure(const std::string &what, const Json &data) {
    std::string reason = data["error"].string();
    return reason.empty() ? what : what + ": " + reason;
}
bool succeeded(const Response &response, const Json &data) {
    return response.ok() && data.valid() && (!data.has("ok") || data["ok"].boolean());
}
// A page of an overview ends at a space where one can, so a word is never cut.
std::vector<std::string> pagesOf(const std::string &text) {
    std::vector<std::string> pages;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = std::min(text.size(), start + kOverviewPageBytes);
        if (end < text.size()) {
            size_t space = text.rfind(' ', end);
            if (space != std::string::npos && space > start)
                end = space;
            while (end < text.size() && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80)
                ++end;
        }
        pages.push_back(text.substr(start, end - start));
        start = text.find_first_not_of(' ', end);
        if (start == std::string::npos)
            break;
    }
    return pages;
}
std::string castOf(const Json &item) {
    std::string names;
    size_t count = 0;
    for (const auto &person : item["cast"].items()) {
        if (++count > 8)
            break;
        names += (names.empty() ? "" : ", ") + person["name"].string();
    }
    return names.empty() ? "" : msg("tv.details.cast", "Cast: {names}",
                                    fields({{"names", Json::text(names)}}));
}
bool sameTitle(const Json &item, const std::string &type, const std::string &id) {
    return item["id"].string() == id && (type.empty() || item["type"].string() == type);
}
kit::Enter enterOf(const Motion &motion, int block) {
    double hidden = motion.value("details:t:" + std::to_string(block));
    return {static_cast<float>(1 - hidden), static_cast<float>(18 * hidden)};
}
} // namespace

DetailsScreen::DetailsScreen(const std::string &type, const std::string &id, const Json &item,
                             bool embedded)
    : type_(type), id_(id), first_(item), embedded_(embedded) {}

// ---- Data ------------------------------------------------------------------------------------

void DetailsScreen::enter(ScreenHost &host) {
    Json item = first_;
    if (!item.isObject()) {
        // The card Home showed carries what the page can draw before the details arrive.
        const ViewState &view = host.view();
        for (const auto &row : view.home.rows)
            for (const auto &card : row.cards)
                if (card.kind == "title" && sameTitle(card.data, type_, id_) && !item.isObject())
                    item = card.data;
        if (!item.isObject()) {
            item = Json::object();
            item.set("type", Json::text(type_));
            item.set("id", Json::text(id_));
        }
    }
    push(host, item);
}
void DetailsScreen::leave(ScreenHost &host) {
    host.services().cancel(kDetails);
    host.services().cancel(kNetflix);
    host.services().cancel(kOpen);
    host.motion().erasePrefix("details:");
}
void DetailsScreen::rebuildActions(Title &title) {
    const Json &item = title.item;
    std::string keep = title.action < title.actions.size() ? title.actions[title.action].label : "";
    title.actions.clear();
    title.actions.push_back({Action::Kind::Open, msg("tv.details.open", "Open")});
    if (!item["netflixId"].string().empty())
        title.actions.push_back({Action::Kind::Netflix, msg("tv.details.netflix", "Open on Netflix")});
    if (!item["trailer"].string().empty())
        title.actions.push_back({Action::Kind::Trailer, msg("tv.details.trailer", "Trailer")});
    if (!item["overview"].string().empty())
        title.actions.push_back({Action::Kind::Overview, msg("tv.details.overview", "Full overview")});
    if (title.error)
        title.actions.push_back({Action::Kind::Retry, msg("tv.details.retry", "Retry")});
    // A late Netflix link or a Retry appearing keeps the focus on what it was on.
    title.action = std::min(title.action, title.actions.size() - 1);
    for (size_t i = 0; i < title.actions.size(); ++i)
        if (title.actions[i].label == keep)
            title.action = i;
}
void DetailsScreen::push(ScreenHost &host, const Json &item) {
    if (stack_.size() >= kMaxDepth) {
        host.toast(msg("tv.details.goBack", "Go back before opening another title"));
        return;
    }
    Title title;
    title.item = item;
    rebuildActions(title);
    stack_.push_back(title);
    host.services().cancel(kNetflix);
    load(host);
    host.motion().erasePrefix("details:a:");
    host.motion().erasePrefix("details:s");
    focusMoved(host, true);
    playEntrance(host);
    host.invalidate();
}
void DetailsScreen::load(ScreenHost &host) {
    Title &title = top();
    if (title.loading)
        return;
    title.loading = true;
    title.error = false;
    title.status = msg("tv.details.loading", "Loading details…");
    if (title.actions.size() && title.actions[title.action].kind == Action::Kind::Retry)
        title.action = 0;
    rebuildActions(title);
    title.detailsGeneration = host.services().read(kDetails, Operation::TitleDetails, title.item);
    if (!title.detailsGeneration) {
        title.loading = false;
        title.error = true;
        title.status = msg("tv.details.unavailable", "Details are unavailable. Open or retry.");
        rebuildActions(title);
    }
    host.invalidate();
}
void DetailsScreen::response(ScreenHost &host, const Response &response) {
    if (stack_.empty())
        return;
    Title &title = top();
    Json data = response.json();
    if (response.channel == kDetails && title.detailsGeneration &&
        response.generation == title.detailsGeneration) {
        title.loading = false;
        title.detailsGeneration = 0;
        if (!succeeded(response, data) || !data["enabled"].boolean() || !data["item"].isObject()) {
            title.error = true;
            title.status = data["error"].string(
                msg("tv.details.unavailable", "Details are unavailable. Open or retry."));
        } else {
            std::string netflixId = title.item["netflixId"].string();
            title.item = data["item"];
            if (!netflixId.empty() && title.item["netflixId"].string().empty())
                title.item.set("netflixId", Json::text(netflixId));
            title.error = false;
            title.stale = data["stale"].boolean();
            title.status = title.stale ? msg("tv.details.cached", "Saved details · offline") : "";
            if (data["netflixEnabled"].boolean())
                title.netflixGeneration =
                    host.services().read(kNetflix, Operation::NetflixTitle, title.item);
        }
        rebuildActions(title);
        host.invalidate();
    } else if (response.channel == kNetflix && title.netflixGeneration &&
               response.generation == title.netflixGeneration) {
        title.netflixGeneration = 0;
        if (succeeded(response, data) && !data["netflixId"].string().empty()) {
            title.item.set("netflixId", data["netflixId"]);
            rebuildActions(title);
            focusMoved(host, false);
            host.invalidate();
        }
    } else if (response.channel == kOpen && openGeneration_ &&
               response.generation == openGeneration_) {
        openGeneration_ = 0;
        opening_ = false;
        if (!succeeded(response, data))
            host.toast(failure(msg("tv.details.openFailed", "That did not open"), data));
        host.invalidate();
    }
}

// ---- Keys ------------------------------------------------------------------------------------

void DetailsScreen::focusMoved(ScreenHost &host, bool jump) {
    Motion &motion = host.motion();
    const Title &title = top();
    auto glide = [&](const std::string &id, double to) {
        if (jump)
            motion.set(id, to);
        else
            motion.tween(id, to, kit::motion::ms::focus, easing(kit::motion::ease::out));
    };
    for (size_t i = 0; i < title.actions.size(); ++i)
        glide("details:a:" + std::to_string(i), !title.onSimilar && i == title.action ? 1 : 0);
    size_t count = title.item["similar"].size();
    for (size_t i = 0; i < count; ++i)
        glide("details:s:" + std::to_string(i), title.onSimilar && i == title.similar ? 1 : 0);
    double scroll = kit::geometry::rowScrollTarget(static_cast<int>(title.similar),
                                                   static_cast<int>(count), kSimilarW, kSimilarGap);
    if (jump)
        motion.set("details:sx", scroll);
    else
        motion.spring("details:sx", scroll, kit::springParams(kit::motion::spring::row));
}
void DetailsScreen::playEntrance(ScreenHost &host) {
    Timeline timeline;
    Tween rise(1, 0, kit::motion::ms::heroText, easing(kit::motion::ease::out));
    for (int block = 0; block < 4; ++block) {
        host.motion().set("details:t:" + std::to_string(block), 1);
        timeline.add("details:t:" + std::to_string(block),
                     rise.delayedBy(80.0 * (block + 1)));
    }
    timeline.play(host.motion());
}
void DetailsScreen::toggleOverview(ScreenHost &host, bool open) {
    overview_ = open;
    if (open) {
        overviewPages_ = pagesOf(top().item["overview"].string());
        overviewPage_ = 0;
    }
    host.motion().tween("details:ov", open ? 1 : 0, kit::motion::ms::fast,
                        easing(kit::motion::ease::out));
    host.invalidate();
}
void DetailsScreen::activate(ScreenHost &host) {
    Title &title = top();
    if (title.onSimilar) {
        push(host, title.item["similar"][title.similar]);
        return;
    }
    if (title.action >= title.actions.size())
        return;
    switch (title.actions[title.action].kind) {
    case Action::Kind::Retry:
        load(host);
        return;
    case Action::Kind::Overview:
        toggleOverview(host, true);
        return;
    default:
        break;
    }
    if (opening_)
        return;
    Json body = Json::object();
    Action::Kind kind = title.actions[title.action].kind;
    if (kind == Action::Kind::Trailer) {
        body.set("kind", Json::text("youtube"));
        body.set("id", title.item["trailer"]);
    } else {
        body.set("kind", Json::text(kind == Action::Kind::Netflix ? "netflix" : "title"));
        body.set("type", title.item["type"]);
        body.set("id", title.item["id"]);
    }
    openGeneration_ = host.services().write(kOpen, Operation::OpenHomeItem, body);
    if (openGeneration_)
        opening_ = true;
    else
        host.toast(msg("tv.details.busy", "Too many pending requests. Try again."));
}
bool DetailsScreen::key(ScreenHost &host, Key key) {
    if (stack_.empty())
        return false;
    if (overview_) {
        if ((key == Key::Down || key == Key::Right || key == Key::PageDown) &&
            overviewPage_ + 1 < overviewPages_.size())
            ++overviewPage_;
        if ((key == Key::Up || key == Key::Left || key == Key::PageUp) && overviewPage_)
            --overviewPage_;
        if (key == Key::Ok)
            toggleOverview(host, false);
        host.invalidate();
        return true;
    }
    Title &title = top();
    size_t similar = title.item["similar"].size();
    bool moved = true;
    switch (key) {
    case Key::Up:
        title.onSimilar = false;
        break;
    case Key::Down:
        title.onSimilar = similar > 0;
        break;
    case Key::Left:
        if (title.onSimilar && title.similar)
            --title.similar;
        else if (!title.onSimilar && title.action)
            --title.action;
        break;
    case Key::Right:
        if (title.onSimilar && title.similar + 1 < similar)
            ++title.similar;
        else if (!title.onSimilar && title.action + 1 < title.actions.size())
            ++title.action;
        break;
    case Key::Ok:
        activate(host);
        return true;
    default:
        moved = false;
    }
    if (moved)
        focusMoved(host, false);
    return moved;
}
void DetailsScreen::close(ScreenHost &host) {
    finished_ = true;
    if (embedded_)
        return;
    host.open(returnRoute(host.cameFrom()));
}
bool DetailsScreen::back(ScreenHost &host) {
    if (overview_) {
        toggleOverview(host, false);
        return true;
    }
    host.services().cancel(kDetails);
    host.services().cancel(kNetflix);
    if (!stack_.empty())
        stack_.pop_back();
    if (stack_.empty()) {
        close(host);
        return true;
    }
    // The title underneath may have been left mid-load; its reply was dropped with the channel.
    Title &title = top();
    if (title.loading) {
        title.loading = false;
        load(host);
    }
    host.motion().erasePrefix("details:a:");
    host.motion().erasePrefix("details:s");
    focusMoved(host, true);
    playEntrance(host);
    host.invalidate();
    return true;
}
void DetailsScreen::click(ScreenHost &host, const std::string &id) {
    if (stack_.empty())
        return;
    if (id == "details:back" || id == "details:overview-close") {
        back(host);
        return;
    }
    Title &title = top();
    if (id.compare(0, 15, "details:action:") == 0) {
        title.onSimilar = false;
        title.action = std::min(static_cast<size_t>(std::atoi(id.c_str() + 15)),
                                title.actions.size() - 1);
    } else if (id.compare(0, 16, "details:similar:") == 0) {
        size_t index = static_cast<size_t>(std::atoi(id.c_str() + 16));
        if (index >= title.item["similar"].size())
            return;
        title.onSimilar = true;
        title.similar = index;
    } else
        return;
    focusMoved(host, false);
    activate(host);
}
Json DetailsScreen::snapshot() const {
    Json value = Json::object();
    if (stack_.empty())
        return value;
    const Title &title = top();
    if (title.onSimilar)
        value.set("focus", Json::text(title.item["similar"][title.similar]["title"].string()));
    else if (title.action < title.actions.size())
        value.set("focus", Json::text(title.actions[title.action].label));
    value.set("titleId", title.item["id"]);
    value.set("depth", Json::numeric(static_cast<double>(stack_.size())));
    value.set("contentStatus", Json::text(title.status));
    value.set("contentState", Json::text(title.loading ? "loading" : title.error ? "error" : "ready"));
    value.set("overview", Json::flag(overview_));
    value.set("overviewPage", Json::numeric(static_cast<double>(overviewPage_)));
    return value;
}

// ---- Drawing ---------------------------------------------------------------------------------

namespace {
// The backdrop, with one diagonal fade that keeps the lines readable at the upper left and the
// row of similar titles at the bottom: two full-screen layers in all.
void backdrop(kit::Paint paint, const Json &item) {
    Scene &scene = paint.scene;
    Rect screen = {0, 0, kit::canvas::width, kit::canvas::height};
    std::string art = artwork(item, true);
    if (art.empty())
        kit::ambientWall(paint, 1, home::hsl(static_cast<float>(home::hashOf(item["id"].string()) % 360),
                                             .45f, .115f));
    else
        scene.picture(screen, 0).value = art;
    Color shade = kit::color::ink;
    Node &fade = scene.shape(screen, {0, 0, 0, 0});
    fade.gradient.stops = 4;
    fade.gradient.angle = 45;
    fade.gradient.color[0] = kit::focus::withAlpha(shade, .97f);
    fade.gradient.color[1] = kit::focus::withAlpha(shade, .90f);
    fade.gradient.color[2] = kit::focus::withAlpha(shade, .50f);
    fade.gradient.color[3] = kit::focus::withAlpha(shade, 0);
    fade.gradient.at[0] = 0;
    fade.gradient.at[1] = .38f;
    fade.gradient.at[2] = .60f;
    fade.gradient.at[3] = .92f;
}
void overviewSheet(kit::Paint paint, const std::string &title, const std::string &body,
                   size_t page, size_t pages, float open) {
    Scene &scene = paint.scene;
    kit::veil(paint, .82f * open);
    // What is under the sheet cannot be clicked through it.
    scene.hits.push_back({{0, 0, kit::canvas::width, kit::canvas::height}, "details:sheet"});
    scene.pushOpacity(open);
    scene.pushTransform(Transform::translate(0, 24 * (1 - open)));
    Rect panel = {260, 150, 1400, 780};
    scene.shape(panel, kit::color::surface1, kit::radius::xl).ring = {1, 0, kit::alpha::outline};
    paint.glyphs.text(scene, {panel.x + 56, panel.y + 48, panel.w - 112, kit::type::headline.line},
                      title, kit::type::headline, kit::color::onSurface);
    paint.glyphs.text(scene, {panel.x + 56, panel.y + 48 + kit::type::headline.line + 24,
                              panel.w - 112, kit::type::bodyL.line * 12},
                      body, kit::type::bodyL, kit::color::onSurface2, Node::Align::Left, 12);
    if (pages > 1)
        paint.glyphs.text(scene, {panel.x + 56, panel.y + panel.h - 88, 400, kit::type::labelM.line},
                          msg("tv.details.page", "{page} of {pages}",
                              fields({{"page", Json::text(std::to_string(page + 1))},
                                      {"pages", Json::text(std::to_string(pages))}})),
                          kit::type::labelM, kit::color::onSurface3);
    kit::PillButton close;
    close.rest = {panel.x + panel.w - 56 - 200, panel.y + panel.h - 56 - 56, 200, 56};
    close.id = "details:overview-close";
    close.label = msg("tv.details.close", "Close");
    close.focus = 1;
    kit::pillButton(paint, close);
    scene.popTransform();
    scene.popOpacity();
}
} // namespace

void DetailsScreen::build(ScreenHost &host, kit::Paint paint) {
    if (stack_.empty())
        return;
    const Title &title = top();
    const Motion &motion = host.motion();
    Scene &scene = paint.scene;
    const Json &item = title.item;
    scene.background = kit::color::ink;
    backdrop(paint, item);

    float y = 190;
    auto group = [&](int block) {
        kit::Enter enter = enterOf(motion, block);
        scene.pushOpacity(enter.alpha);
        scene.pushTransform(Transform::translate(0, enter.dy));
    };
    auto endGroup = [&] {
        scene.popTransform();
        scene.popOpacity();
    };
    // Block 0: what it is.
    group(0);
    Rect mark = {kit::canvas::safeX, y + (layout::heroSourceH - layout::heroMark) / 2,
                 layout::heroMark, layout::heroMark};
    scene.shape(mark, kit::alpha::accentDim, layout::heroMark / 2);
    paint.glyphs.icon(scene, {mark.x + 5, mark.y + 5, 18, 18}, "play", kit::color::accent);
    paint.glyphs.text(scene,
                      {mark.x + mark.w + layout::heroMarkGap, y,
                       kTextWidth - mark.w - layout::heroMarkGap, layout::heroSourceH},
                      item["type"].string() == "tv" ? msg("tv.details.series", "Series")
                                                    : msg("tv.details.film", "Film"),
                      kit::type::heroSource, kit::color::onSurface2);
    y += layout::heroSourceH + 12;
    const std::string name = item["title"].string();
    bool longTitle = paint.glyphs.measure(name, kit::type::display) > kTextWidth;
    const kit::TypeStyle &titleStyle = longTitle ? kit::type::headline : kit::type::display;
    float titleHeight = longTitle ? titleStyle.line * 2 : titleStyle.line;
    paint.glyphs.text(scene, {kit::canvas::safeX, y, kTextWidth, titleHeight}, name, titleStyle,
                      kit::color::onSurface, Node::Align::Left, longTitle ? 2 : 1);
    y += titleHeight + 12;
    paint.glyphs.text(scene, {kit::canvas::safeX, y, kTextWidth, kit::type::bodyL.line},
                      titleFacts(item), kit::type::bodyL, kit::color::onSurface2);
    y += kit::type::bodyL.line + 20;
    endGroup();
    // Block 1: the story.
    group(1);
    std::string overview = item["overview"].string();
    paint.glyphs.text(scene, {kit::canvas::safeX, y, kTextWidth - 40, kit::type::body.line * 3},
                      overview.empty() ? msg("tv.details.noOverview", "No overview available")
                                       : overview,
                      kit::type::body, overview.empty() ? kit::color::onSurface3 : kit::color::onSurface2,
                      Node::Align::Left, 3);
    y += kit::type::body.line * 3 + 12;
    std::string cast = castOf(item);
    if (!cast.empty())
        paint.glyphs.text(scene, {kit::canvas::safeX, y, kTextWidth - 40, kit::type::labelM.line}, cast,
                          kit::type::labelM, kit::color::onSurface3);
    y += kit::type::labelM.line + 28;
    endGroup();
    // Block 2: what can be done.
    group(2);
    float x = kit::canvas::safeX;
    for (size_t i = 0; i < title.actions.size(); ++i) {
        kit::PillButton button;
        button.label = title.actions[i].label;
        float width = std::max(160.0f, paint.glyphs.measure(button.label, kit::type::title) + 72);
        button.rest = {x, y, width, kActionH};
        button.id = "details:action:" + std::to_string(i);
        button.primary = title.actions[i].kind == Action::Kind::Open;
        button.focus = static_cast<float>(motion.value("details:a:" + std::to_string(i)));
        kit::pillButton(paint, button);
        x += width + 16;
    }
    y += kActionH + 16;
    if (!title.status.empty())
        paint.glyphs.text(scene, {kit::canvas::safeX, y, kTextWidth, kit::type::labelM.line},
                          title.status, kit::type::labelM,
                          title.error ? kit::color::bad : kit::color::onSurface3);
    endGroup();
    // Block 3: more like this.
    size_t count = item["similar"].size();
    if (count) {
        group(3);
        paint.glyphs.text(scene, {kit::canvas::safeX, kSimilarTop - 36, 600, kit::type::labelM.line},
                          msg("tv.details.more", "More like this"), kit::type::labelM,
                          kit::color::onSurface2);
        float scroll = static_cast<float>(motion.value("details:sx"));
        for (size_t i = 0; i < count; ++i) {
            Rect rest = {kit::canvas::safeX + static_cast<float>(i) * (kSimilarW + kSimilarGap) + scroll,
                         kSimilarTop, kSimilarW, kSimilarH};
            if (rest.x + rest.w < -140 || rest.x > kit::canvas::width + 140)
                continue;
            Json similar = item["similar"][i];
            kit::PosterCard card;
            card.rest = rest;
            card.id = "details:similar:" + std::to_string(i);
            card.image = artwork(similar);
            card.title = similar["title"].string();
            card.focus = static_cast<float>(motion.value("details:s:" + std::to_string(i)));
            kit::card(paint, card);
        }
        endGroup();
    }
    kit::keyHint(paint, kit::canvas::safeX, 1002, "Back", msg("tv.details.return", "Return"),
                 "details:back");
    float open = static_cast<float>(motion.value("details:ov"));
    if (open > .002f)
        overviewSheet(paint, item["title"].string(),
                      overviewPage_ < overviewPages_.size() ? overviewPages_[overviewPage_] : "",
                      overviewPage_, overviewPages_.size(), open);
}

std::unique_ptr<ScreenView> makeDetailsScreen(const std::string &route) {
    if (route.compare(0, 8, "details:") != 0)
        return nullptr;
    size_t split = route.find(':', 8);
    std::string type = split == std::string::npos ? "" : route.substr(8, split - 8),
                id = split == std::string::npos ? route.substr(8) : route.substr(split + 1);
    return std::unique_ptr<ScreenView>(new DetailsScreen(type, id, Json(), false));
}
} // namespace nativetv
