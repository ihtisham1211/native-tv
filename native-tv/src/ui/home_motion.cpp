// Home's choreography: what moves when focus, tab, hero or data change. The view (home_view.cpp)
// only reads the values set here.
#include "ui/application.hpp"
#include "ui/controller_helpers.hpp"
#include "ui/home_layout.hpp"
#include "ui/kit/tuning.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>

namespace nativetv {
namespace {
using kit::easing;
using kit::springParams;
namespace ms = kit::motion::ms;
namespace ease = kit::motion::ease;
namespace stagger = kit::motion::stagger;

constexpr uint32_t kBeatMs = 5000;
constexpr uint32_t kHeroSettleMs = 400; // a held key moves on without changing the hero each step
constexpr uint32_t kTabSettleMs = 190;  // the focus stays on a tab this long before its page opens
constexpr uint32_t kPageSwapMs = 110;   // the old page has all but left
constexpr uint32_t kHeroWaitMs = 1200;  // longest wait for the incoming art
constexpr uint32_t kIconWaitMs = 300;   // longest wait for the apps' icons before their row enters
constexpr int kHeroWidth = 1600, kHeroHeight = 900;

bool due(uint32_t now, uint32_t at) {
    return static_cast<int32_t>(now - at) >= 0;
}
bool isTitle(const Card &card) {
    return card.kind == "title" || card.kind == "youtube" || card.kind == "netflix";
}
std::string artOf(const Card &card) {
    return isTitle(card) ? (card.backdrop.empty() ? card.image : card.backdrop) : "";
}
// A deep, quiet colour per card: the prototype tints the wall with the artwork's darkest tone,
// and the server sends no palette, so the tone comes from the app's colour or the title. An app
// has no picture of its own behind its lines, so its tint is stronger.
Color tintOf(const Card &card) {
    float strength = isTitle(card) ? .22f : .42f;
    std::string color = card.data["color"].string();
    if (color.size() == 7 && color[0] == '#') {
        Color base = kit::rgb(static_cast<unsigned>(std::strtoul(color.c_str() + 1, nullptr, 16)));
        return {base.r * strength + .02f, base.g * strength + .02f, base.b * strength + .03f, 1};
    }
    return home::hsl(static_cast<float>(home::hashOf(card.id + card.title) % 360), .45f,
                     isTitle(card) ? .115f : .2f);
}
// The pick on show behind a focused app, as a wallpaper.
constexpr float kWallpaperAlpha = .22f;
size_t rowIndex(const std::vector<CardRow> &rows, const char *key) {
    for (size_t i = 0; i < rows.size(); ++i)
        if (rows[i].key == key)
            return i;
    return rows.size();
}
// Where focus lands when Home opens: the top picks, so the hero opens on artwork; the apps row
// above them is one press up.
size_t defaultRow(const HomeState &home) {
    if (home.tabs.empty() || home.tabs[home.tab].key != "home")
        return 0;
    size_t trending = rowIndex(home.rows, "trending");
    return trending < home.rows.size() ? trending : 0;
}
// The row that names the card in the hero: the focused one when it holds the card (a title can
// sit in two rows), otherwise the first that does.
std::string rowTitleOf(const ViewState &state, const Card &card) {
    auto holds = [&](const CardRow &row) {
        for (const auto &candidate : row.cards)
            if (candidate.id == card.id && candidate.kind == card.kind)
                return true;
        return false;
    };
    const auto &rows = state.home.rows;
    if (state.home.row < rows.size() && holds(rows[state.home.row]))
        return rows[state.home.row].title;
    for (const auto &row : rows)
        if (holds(row))
            return row.title;
    return "";
}
HeroText linesOf(const ViewState &state, const Card &card) {
    HeroText text;
    text.title = card.title;
    if (isTitle(card)) {
        text.source = rowTitleOf(state, card);
        if (text.source.empty())
            text.source = card.kind == "youtube" ? "YouTube" : "Top picks for you";
        text.facts = card.subtitle.empty() && card.kind == "youtube" ? "YouTube" : card.subtitle;
        text.description = card.data["overview"].string();
    } else if (card.kind == "dev" || card.kind == "ch") {
        text.source = card.kind == "dev" ? "Input" : "Channel";
        text.facts = card.subtitle;
        text.hint = card.kind == "dev" ? "Switch" : "Watch";
    } else {
        text.source = card.id == "lg-home" ? "LG Home" : "Your apps";
        text.facts = card.data["description"].string();
        text.image = card.image;
        text.hint = "Open";
    }
    return text;
}
// How many of a row's items start on the screen; the ones past it have no entrance, they are
// scrolled to.
size_t shownOf(const CardRow &row, bool named) {
    float pitch = home::itemWidth(row) + home::itemGap(row, named);
    size_t fit = static_cast<size_t>((kit::canvas::width - kit::canvas::safeX) / pitch) + 1;
    return std::min(row.cards.size(), fit);
}
// Whether the icons of the apps on the first screen of a page of `rows` have arrived (or can
// not). Asking is also what starts their fetch, at the size the circles draw them at.
bool iconsReady(Pictures *pictures, const std::vector<CardRow> &rows, bool grid, bool named,
                double now) {
    if (!pictures)
        return true;
    bool ready = true;
    for (size_t r = 0; r < rows.size() && r < 3; ++r) {
        const CardRow &row = rows[r];
        if (!home::isApps(row))
            continue;
        size_t shown = grid ? std::min(row.cards.size(), home::kGridColumns * 3) : shownOf(row, named);
        int texels = pictureTexels(grid ? kit::layout::tileW : kit::layout::appD);
        for (size_t c = 0; c < shown; ++c) {
            if (row.cards[c].image.empty())
                continue;
            ImageView view = pictures->view(row.cards[c].image, texels, texels, ImageFit::Cover, now);
            ready = ready && (view.texture != 0 || view.failed);
        }
    }
    return ready;
}
} // namespace

// ---- Data and layout -------------------------------------------------------------------------

// The page is shown from the start: nothing of the last visit's animation survives.
void Application::homeStart() {
    auto &home = state_.home;
    home.arranging = false;
    home.editing = home.editHide = false;
    flow_.entered = false;
    flow_.rowsEntered = 0;
    flow_.pendingTab = -1;
    flow_.tabPending = flow_.textPending = flow_.heroWaiting = flow_.waiting = false;
    flow_.shown = home.tab;
    flow_.lastKeyAt = now_;
    flow_.idle = false;
    flow_.focusId.clear();
    flow_.wantId.clear();
    if (!flow_.beatAt)
        flow_.beatAt = now_ + kBeatMs;
    for (const char *prefix :
         {"f:", "rx", "rd", "E", "ha", "hx", "dot", "pgx", "dim", "gy", "sy", "lift"})
        motion_.erasePrefix(prefix);
    home.art = HeroArt();
    home.hero = Card();
    home.spotlight.clear();
    home.spotIndex = 0;
    bool hero = !home.tabs.empty() && home.tabs[home.tab].key == "home";
    motion_.set("wall", hero ? 1 : 0);
    motion_.set("dots", 1);
    for (const char *name : {"hk:0", "hk:1", "hp"}) {
        motion_.set(name, 0);
        motion_.ambient(name);
    }
    for (int i = 0; i < 3; ++i)
        motion_.set(home::id("hx", i), 1);
}
// Called whenever the rows, tabs or catalogue changed under the page.
void Application::homeData() {
    auto &home = state_.home;
    if (home.tabs.empty())
        return;
    // The spotlight: the top picks, the pictures the hero cycles through.
    home.spotlight.clear();
    size_t trending = rowIndex(home.rows, "trending");
    if (trending < home.rows.size() && home.tabs[home.tab].key == "home")
        for (const Card &card : home.rows[trending].cards) {
            if (home.spotlight.size() == home::kSpotlightMax)
                break;
            if (isTitle(card))
                home.spotlight.push_back(card);
        }
    home.spotIndex = home.spotlight.empty() ? 0 : std::min(home.spotIndex, home.spotlight.size() - 1);
    std::vector<std::string> titles;
    for (const auto &tab : home.tabs)
        titles.push_back(tab.title);
    if (titles != flow_.tabTitles) {
        flow_.tabTitles = titles;
        homeRetargetPill(true);
    }
    if (!flow_.entered && !home.rows.empty() && flow_.pendingTab < 0) {
        // The page enters once the top picks are in, so the rows do not reorder under their own
        // entrance (a feed that stays silent costs 600 ms), and once the apps' icons are in, so
        // a circle does not enter as a colour and change under the eye (300 ms at most). Until
        // then nothing of it is drawn.
        if (!flow_.waiting) {
            flow_.waiting = true;
            flow_.waitFrom = now_;
            flow_.rowsEntered = 0;
            motion_.set("pgx", 1);
        }
        const bool homeTab = home.tabs[home.tab].key == "home";
        const bool feed = !homeTab || state_.discover.valid() || due(now_, flow_.waitFrom + 600);
        const bool icons = flow_.rowsEntered ||
                           iconsReady(pictures_, home.rows, home.grid,
                                      home.tabs[home.tab].key == "apps", now_) ||
                           due(now_, flow_.waitFrom + kIconWaitMs);
        if (feed && icons) {
            flow_.entered = true;
            flow_.waiting = false;
            home.row = defaultRow(home);
            homePlayEntrance(flow_.rowsEntered);
            homeFocus(true);
        } else if (icons && homeTab && !flow_.rowsEntered && home::isApps(home.rows[0])) {
            // The apps lead Home and the feed's rows follow them: they enter without the feed,
            // and the focus will not be on them, so their name is already dim.
            homePlayEntrance(0, 1);
            motion_.set(home::id("rd", 0), 1);
        }
    } else if (flow_.entered && flow_.pendingTab < 0) {
        // Rows that came late (a feed past its 600 ms) enter like the others.
        if (home.rows.size() > flow_.rowsEntered)
            homePlayEntrance(flow_.rowsEntered);
        flow_.rowsEntered = std::min(flow_.rowsEntered, home.rows.size());
        homeFocus(); // rows changed under the focus: its targets follow the card it kept
    }
}
// Plays the entrance of rows [firstRow, endRow); the apps lead Home, so they can enter before the
// rows that follow them.
void Application::homePlayEntrance(size_t firstRow, size_t endRow) {
    auto &home = state_.home;
    Timeline timeline;
    Tween rise(1, 0, ms::base, easing(ease::out));
    motion_.set("pgx", 0); // the page was held hidden until now
    if (home.grid) {
        size_t count = home.rows.empty() ? 0 : home.rows[0].cards.size();
        for (size_t i = 0; i < count && i < 30; ++i) // every tile that can be on screen
            timeline.add(home::id("EC", 0, i),
                         rise.delayedBy(stagger::pageBase +
                                        stagger::pageItem *
                                            std::min(static_cast<int>(i), stagger::pageItemMax)));
        flow_.rowsEntered = home.rows.size();
    } else {
        const bool named = home.tabs[home.tab].key == "apps";
        endRow = std::min(endRow, home.rows.size());
        for (size_t r = firstRow; r < endRow && r < 3; ++r) {
            double rowDelay = stagger::homeBase + stagger::homeRow * static_cast<double>(r);
            timeline.add(home::id("ER", r), rise.delayedBy(rowDelay));
            for (size_t c = 0; c < shownOf(home.rows[r], named); ++c)
                timeline.add(home::id("EC", r, c),
                             rise.delayedBy(rowDelay +
                                            stagger::homeCell *
                                                std::min(static_cast<int>(c), stagger::homeCellMax)));
        }
        flow_.rowsEntered = endRow;
    }
    timeline.play(motion_);
}

// ---- Focus ----------------------------------------------------------------------------------

void Application::homeRetargetPill(bool jump) {
    auto &home = state_.home;
    std::vector<Rect> boxes = kit::tabRects(*glyphs_, flow_.tabTitles);
    bool onTab = home.navigation && home.nav < home.tabs.size();
    size_t target = onTab ? home.nav : home.tab;
    // A registered screen keeps the pill under its own tab.
    if (!onTab && screen_ && screen_->barTab() >= 0)
        target = static_cast<size_t>(screen_->barTab());
    if (target < boxes.size()) {
        float left = boxes[target].x, right = boxes[target].x + boxes[target].w;
        if (jump || motion_.value("pill:1") < 1) {
            motion_.set("pill:0", left);
            motion_.set("pill:1", right);
        } else {
            // The edge that leads the move is the stiffer spring: the pill stretches, then settles.
            bool rightward = right > motion_.value("pill:1") + .5;
            motion_.spring("pill:1", right,
                           springParams(rightward ? kit::motion::spring::pillLead
                                                  : kit::motion::spring::pillTrail));
            motion_.spring("pill:0", left,
                           springParams(rightward ? kit::motion::spring::pillTrail
                                                  : kit::motion::spring::pillLead));
        }
    }
    motion_.tween("pill:hot", onTab ? 1 : 0, ms::fast, easing(ease::out));
}
// Retargets every value that follows the focus: ring and lift, row scroll, stack, dim, the pill
// and the bar's buttons. A held key only moves targets, so each value glides on from where it is.
void Application::homeFocus(bool jump) {
    auto &home = state_.home;
    if (home.tabs.empty())
        return;
    auto glide = [&](const std::string &name, double to) {
        if (jump)
            motion_.set(name, to);
        else
            motion_.tween(name, to, ms::focus, easing(ease::out));
    };
    std::string now;
    if (home.navigation) {
        if (home.nav == home.tabs.size())
            now = "f:av";
        else if (home.nav == home.tabs.size() + 1)
            now = "f:gr";
        else if (home.nav == home.tabs.size() + 2)
            now = "f:sr";
    } else if (home.editing && home.editHide)
        now = "f:eh"; // the Hide from Home button above the page
    else if (home.row < home.rows.size() && flow_.pendingTab < 0)
        now = home::id("f", home.row, home.column);
    if (now != flow_.focusId) {
        if (!flow_.focusId.empty())
            glide(flow_.focusId, 0);
        if (!now.empty())
            glide(now, 1);
        flow_.focusId = now;
    }
    bool grid = home.grid;
    for (size_t r = 0; r < home.rows.size(); ++r)
        glide(home::id("rd", r), !home.navigation && r != home.row ? 1 : 0);
    if (!home.navigation && home.row < home.rows.size()) {
        const CardRow &row = home.rows[home.row];
        if (grid) {
            double target = home::gridScrollTarget(home.column, row.cards.size());
            if (jump)
                motion_.set("gy", target);
            else
                motion_.spring("gy", target, springParams(kit::motion::spring::list));
        } else {
            std::string name = home::id("rx", home.row);
            bool named = home.tabs[home.tab].key == "apps";
            double target = kit::geometry::rowScrollTarget(
                static_cast<int>(home.column), static_cast<int>(row.cards.size()),
                home::itemWidth(row), home::itemGap(row, named));
            if (jump)
                motion_.set(name, target);
            else
                motion_.spring(name, target, springParams(kit::motion::spring::row));
        }
    }
    // The apps row and the top picks share the hero's screen; the stack rises from the picks on.
    const bool kindRows = home.tabs[home.tab].key == "apps" && !grid;
    double stack = home.navigation || grid ? 0
                   : kindRows ? home::kindStackTarget(home.row, home.rows.size())
                              : kit::geometry::stackTarget(static_cast<int>(home.row) -
                                                           static_cast<int>(defaultRow(home)));
    if (jump)
        motion_.set("sy", stack);
    else
        motion_.spring("sy", stack, springParams(kit::motion::spring::page));
    homeRetargetPill(jump);
}

void Application::homeKeyMoved(Key key) {
    auto &home = state_.home;
    if (home.navigation && home.nav < home.tabs.size() && home.nav != flow_.shown &&
        (key == Key::Left || key == Key::Right)) {
        flow_.tabPending = true;
        flow_.commitAt = now_ + kTabSettleMs;
    } else if (key == Key::Left || key == Key::Right || key == Key::Up)
        flow_.tabPending = false;
    homeFocus();
}

// ---- Arranging the kinds ----------------------------------------------------------------------

// While a row is lifted, Up and Down move it past its neighbours (the focus and the row's scroll
// travel with it), OK saves the kinds' order, Left and Right do nothing.
void Application::homeArrange(Key key) {
    auto &home = state_.home;
    if (key == Key::Up || key == Key::Down) {
        if ((key == Key::Up && home.row == 0) || (key == Key::Down && home.row + 1 >= home.rows.size()))
            return;
        size_t to = key == Key::Up ? home.row - 1 : home.row + 1;
        std::swap(home.rows[home.row], home.rows[to]);
        double fromScroll = motion_.value(home::id("rx", home.row)),
               toScroll = motion_.value(home::id("rx", to));
        motion_.set(home::id("rx", home.row), toScroll);
        motion_.set(home::id("rx", to), fromScroll);
        home.row = to;
        // The ring stays on the same card at its new index; the stack slides one pitch.
        double stack = motion_.value("sy");
        homeFocus(true);
        motion_.set("sy", stack);
        homeFocus();
        return;
    }
    if (key == Key::Ok) {
        // The server keeps every kind: one with no app has no row here and holds its place, the
        // rows on screen take the order they were left in.
        Json kinds = Json::array();
        size_t next = 0;
        for (const Json &kind : state_.catalogue["kinds"].items()) {
            bool shown = false;
            for (const CardRow &row : home.rows)
                shown = shown || row.group == kind["key"].string();
            kinds.add(Json::text(shown && next < home.rows.size() ? home.rows[next++].group
                                                                  : kind["key"].string()));
        }
        if (!kinds.size())
            for (const CardRow &row : home.rows)
                kinds.add(Json::text(row.group));
        home.arranging = false;
        execute(post(Operation::SetHomeLayout, fields({{"kinds", kinds}})));
        homeFocus();
    }
}
// Back while arranging: the rows return to the saved order, the focus with the lifted row.
void Application::homeArrangeCancel() {
    auto &home = state_.home;
    std::string lifted = home.row < home.rows.size() ? home.rows[home.row].group : "";
    const size_t column = home.column; // the rebuild clamps it against the row now at this index
    std::map<std::string, double> scrolls;
    for (size_t i = 0; i < home.rows.size(); ++i)
        scrolls[home.rows[i].group] = motion_.value(home::id("rx", i));
    home.arranging = false;
    rebuildHome();
    for (size_t i = 0; i < home.rows.size(); ++i) {
        if (home.rows[i].group == lifted) {
            home.row = i;
            home.column = clampIndex(column, home.rows[i].cards.size());
        }
        if (scrolls.count(home.rows[i].group))
            motion_.set(home::id("rx", i), scrolls[home.rows[i].group]);
    }
    homeFocus();
}

// ---- Tabs and pages ---------------------------------------------------------------------------

// Starts the old page leaving; its rows are swapped for the new tab's once it has.
void Application::homeSelectTab(size_t tab) {
    auto &home = state_.home;
    flow_.tabPending = false;
    if (tab >= home.tabs.size() || (tab == flow_.shown && flow_.pendingTab < 0))
        return;
    flow_.pendingTab = static_cast<int>(tab);
    flow_.swapAt = now_ + kPageSwapMs;
    // The Apps page's icons (a larger size than Home's) are asked for while the old page leaves,
    // so its entrance does not wait for them.
    if (home.tabs[tab].key == "apps")
        iconsReady(pictures_,
                   homeRows("apps", state_.catalogue, state_.discover, state_.guide),
                   state_.catalogue["layout"]["sort"]["app"].string() != "kind", true, now_);
    motion_.tween("pgx", 1, ms::pageOut, easing(ease::out));
    motion_.tween("wall", home.tabs[tab].key == "home" ? 1 : 0, ms::page, easing(ease::out));
    dirty_ = true;
}

void Application::homeUpdate() {
    auto &home = state_.home;
    if (home.tabs.empty())
        return;
    if (!flow_.entered)
        homeData();
    if (flow_.tabPending && due(now_, flow_.commitAt))
        homeSelectTab(home.nav);
    if (flow_.pendingTab >= 0 && due(now_, flow_.swapAt)) {
        home.tab = static_cast<size_t>(flow_.pendingTab);
        flow_.pendingTab = -1;
        flow_.shown = home.tab;
        home.row = home.column = 0;
        home.hero = Card();
        home.art.shownId.clear();
        flow_.wantId.clear();
        flow_.heroWaiting = false;
        for (const char *prefix : {"f:", "rx", "rd", "E"})
            motion_.erasePrefix(prefix);
        for (const char *name : {"gy", "sy", "pgx"})
            motion_.set(name, 0);
        flow_.entered = flow_.waiting = false;
        flow_.focusId.clear();
        rebuildHome();
        home.row = defaultRow(home);
        homeFocus(true);
    }
    // A refresh, a mutation while editing or a jump between rows can leave the focus on a card the row
    // no longer has; it comes back to the nearest one rather than resting on nothing.
    if (!home.rows.empty() && !home.navigation && !home.editing && !home.arranging) {
        const size_t row = clampIndex(home.row, home.rows.size());
        const size_t column = clampIndex(home.column, home.rows[row].cards.size());
        if (row != home.row || column != home.column) {
            home.row = row;
            home.column = column;
            homeFocus();
        }
    }
    // Hero: what the focused card, or the spotlight, asks for; the change waits for the art.
    bool heroTab = home.tabs[home.tab].key != "apps" && flow_.pendingTab < 0;
    for (const char *name : {"hp", "hk:0", "hk:1"})
        if (!heroTab && motion_.moving(name))
            motion_.set(name, motion_.value(name));
    if (heroTab && flow_.entered && !home.rows.empty()) {
        const Card *focus = nullptr;
        if (!home.navigation && home.row < home.rows.size() &&
            home.column < home.rows[home.row].cards.size())
            focus = &home.rows[home.row].cards[home.column];
        // The hero follows the focus on titles and inputs; the slideshow runs while focus is on the
        // bar, the top picks or an app, which has no preview of its own while there are picks.
        size_t trending = rowIndex(home.rows, "trending");
        const bool picks = !home.spotlight.empty();
        const bool appFocus = focus && focus->kind == "app" && picks;
        bool onSpotlight = home.navigation || home.row == trending || appFocus;
        Card want;
        if (focus && !appFocus)
            want = *focus;
        else if (picks)
            want = home.spotlight[home.spotIndex];
        else if (!home.hero.title.empty())
            want = home.hero;
        else if (!home.rows[0].cards.empty())
            want = home.rows[0].cards[0];
        std::string wantId = want.kind + ":" + want.id, shownId = home.art.shownId;
        if (wantId != flow_.wantId) {
            flow_.wantId = wantId;
            flow_.want = want;
            flow_.wantSince = now_;
            flow_.heroWaiting = true;
        }
        if (wantId == shownId)
            flow_.heroWaiting = false;
        else if (flow_.heroWaiting && (shownId.empty() || due(now_, flow_.wantSince + kHeroSettleMs))) {
            std::string art = artOf(flow_.want);
            bool ready =
                art.empty() || !pictures_ ||
                pictures_->view(art, kHeroWidth, kHeroHeight, ImageFit::Cover, now_).texture != 0 ||
                due(now_, flow_.wantSince + kHeroSettleMs + kHeroWaitMs);
            if (ready) {
                flow_.heroWaiting = false;
                homeRetargetHero(flow_.want, shownId.empty());
            }
        }
        // The slideshow: with nobody pressing keys the hero moves on when its progress is full.
        if (home.spotlight.size() > 1 && onSpotlight && !motion_.moving("hp") &&
            !shownId.empty()) {
            if (!flow_.idle && due(now_, flow_.lastKeyAt + static_cast<uint32_t>(ms::heroAdvance))) {
                home.spotIndex = (home.spotIndex + 1) % home.spotlight.size();
                // On the top picks the focus travels with the slideshow.
                if (!home.navigation && home.row == trending &&
                    home.spotIndex < home.rows[trending].cards.size()) {
                    home.column = home.spotIndex;
                    homeFocus();
                }
            }
            motion_.play("hp", Tween(0, 1, ms::heroAdvance, Easing::linear()));
        }
    }
    if (flow_.textPending && due(now_, flow_.textSwapAt))
        homeSwapText();
    // The Ken Burns of a layer that has been replaced stops once it is invisible.
    for (int layer = 0; layer < 2; ++layer)
        if (motion_.value(home::id("ha", layer)) <= 0 && motion_.moving(home::id("hk", layer)))
            motion_.set(home::id("hk", layer), 0);
}

// ---- Hero -------------------------------------------------------------------------------------

void Application::homeRetargetHero(const Card &card, bool instant) {
    auto &home = state_.home;
    auto &art = home.art;
    std::string path = artOf(card);
    float artAlpha = 1;
    // An app or input has no picture: the pick on show stays behind it, dimmed to a wallpaper.
    if (path.empty() && !home.spotlight.empty()) {
        path = artOf(home.spotlight[std::min(home.spotIndex, home.spotlight.size() - 1)]);
        artAlpha = kWallpaperAlpha;
    }
    art.shownId = card.kind + ":" + card.id;
    home.hero = card;
    // The slideshow resumes from whichever card the hero now shows.
    for (size_t i = 0; i < home.spotlight.size(); ++i)
        if (home.spotlight[i].id == card.id && home.spotlight[i].kind == card.kind)
            home.spotIndex = i;
    int outgoing = art.newest, incoming = 1 - outgoing;
    bool hadArt = !art.image[outgoing].empty();
    bool samePicture = !path.empty() && path == art.image[outgoing];
    if (!path.empty() || hadArt) {
        art.newest = incoming;
        art.image[incoming] = path;
        art.artAlpha[incoming] = artAlpha;
        art.tint[incoming] = tintOf(card);
        art.wallTint = art.tint[incoming];
        std::string in = home::id("ha", incoming), out = home::id("ha", outgoing);
        if (instant || !hadArt) {
            motion_.set(in, 1);
            motion_.set(out, 0);
            art.image[outgoing].clear();
        } else {
            motion_.play(in, Tween(0, 1, ms::hero, easing(ease::inOut)));
            // The old layer stays opaque underneath until the new one covers it. The same
            // picture dimming to a wallpaper, or back, fades across instead, so its brightness
            // never jumps.
            motion_.play(out, samePicture ? Tween(1, 0, ms::hero, easing(ease::inOut))
                                          : Tween(1, 0, 60, Easing::linear(), ms::hero));
        }
        std::string push = home::id("hk", incoming);
        if (samePicture && !instant) {
            // The push carries on from where the picture was.
            double at = motion_.value(home::id("hk", outgoing));
            motion_.set(push, at);
            motion_.play(push, Tween(static_cast<float>(at), 1,
                                     static_cast<float>(ms::kenburns * (1 - at)), Easing::linear()));
        } else
            motion_.play(push, Tween(0, 1, ms::kenburns, Easing::linear()));
    } else {
        art.tint[outgoing] = tintOf(card);
        art.wallTint = art.tint[outgoing];
    }
    motion_.play("hp", Tween(0, 1, ms::heroAdvance, Easing::linear()));
    // The dots describe the top picks: they fade while the hero shows an app or an input.
    if (instant)
        motion_.set("dots", isTitle(card) ? 1 : 0);
    else
        motion_.tween("dots", isTitle(card) ? 1 : 0, ms::fast, easing(ease::out));
    for (size_t i = 0; i < home::kSpotlightMax; ++i) {
        bool current = i < home.spotlight.size() && home.spotlight[i].id == card.id &&
                       home.spotlight[i].kind == card.kind;
        if (instant)
            motion_.set(home::id("dot", i), current ? 1 : 0);
        else
            motion_.tween(home::id("dot", i), current ? 1 : 0, ms::fast, easing(ease::out));
    }
    HeroText next = linesOf(state_, card);
    if (instant || art.text == HeroText() || art.text == next) {
        art.text = next;
        if (instant)
            for (int i = 0; i < 3; ++i)
                motion_.play(home::id("hx", i),
                             Tween(1, 0, ms::heroText, easing(ease::out),
                                   stagger::homeBase + stagger::heroText[i]));
        flow_.textPending = false;
        return;
    }
    // The old lines leave first (up and out), then the new ones rise in one after another.
    for (int i = 0; i < 3; ++i)
        motion_.play(home::id("hx", i), Tween(0, -1, ms::heroTextOut, easing(ease::out)));
    flow_.textSwapAt = now_ + 170;
    flow_.textPending = true;
}
void Application::homeSwapText() {
    auto &home = state_.home;
    flow_.textPending = false;
    home.art.text = linesOf(state_, home.hero);
    for (int i = 0; i < 3; ++i)
        motion_.play(home::id("hx", i),
                     Tween(1, 0, ms::heroText, easing(ease::out), stagger::heroText[i]));
}

// ---- Idle, beat -------------------------------------------------------------------------------

// The server's heartbeat, as the old Home page sent it: the reply says whether the build, the
// layout or the wallpapers changed, and three silent beats raise the "not answering" banner.
void Application::homeBeat(const Response *reply) {
    if (!reply) {
        if (!active_ || state_.screen == Screen::Splash || !state_.catalogue.valid() ||
            !due(now_, flow_.beatAt))
            return;
        flow_.beatAt = now_ + kBeatMs;
        services_.write("beat", Operation::HomeBeat, Json::object());
        return;
    }
    Json data = reply->json();
    if (!reply->ok() || !data["ok"].boolean()) {
        if (++flow_.beatFails >= 3 && !state_.serverSilent) {
            state_.serverSilent = true;
            motion_.tween("fault", 1, ms::fast, easing(ease::out));
        }
        return;
    }
    flow_.beatFails = 0;
    bool changed = false;
    if (state_.serverSilent) {
        state_.serverSilent = false;
        motion_.tween("fault", 0, ms::fast, easing(ease::out));
        changed = true;
    }
    // A new release of the server or a saved layout: the data is read again.
    std::string build = data["build"].string(), layout = data["layoutVersion"].string();
    if (!build.empty()) {
        changed = changed || (!flow_.build.empty() && build != flow_.build);
        flow_.build = build;
    }
    if (!layout.empty()) {
        changed = changed || (!flow_.layoutVersion.empty() && layout != flow_.layoutVersion);
        flow_.layoutVersion = layout;
    }
    Json wallpapers = data["wallpapers"];
    if (wallpapers.isObject() && state_.catalogue.isObject() &&
        wallpapers.dump() != state_.catalogue["wallpapers"].dump()) {
        state_.catalogue.set("wallpapers", wallpapers);
        syncWallpaper();
    }
    if (changed)
        refresh();
    dirty_ = true;
}
} // namespace nativetv
