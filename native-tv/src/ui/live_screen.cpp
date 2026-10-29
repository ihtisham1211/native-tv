// Live: the TV's own readings as cards (spec 3.4). /api/stats is read every few seconds, and what
// changes moves rather than jumps: numerals count to their new value, bars grow, and the
// processor's line slides one sample left. Nothing animates between readings, so the page draws
// only while something moves.
#include "domain/live_cards.hpp"
#include "domain/say.hpp"
#include "ui/application.hpp"
#include "ui/dashboard_screens.hpp"
#include "ui/kit/tuning.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace nativetv {
namespace {
namespace ms = kit::motion::ms;
constexpr double kPollMs = 4000;  // between readings; /api/stats is cached for 1.5 s by the daemon
constexpr double kRetryMs = 1000; // while the daemon is still warming up, or its queue was full
constexpr double kNumberMs = 600; // a numeral counting to its new reading
constexpr double kSlideMs = 700;  // the processor's line making room for a new sample
constexpr size_t kHistory = 30;   // samples under the processor card
constexpr size_t kColumns = static_cast<size_t>(kit::layout::statCols);

class LiveScreen final : public ScreenView {
  public:
    std::string name() const override {
        return "live";
    }
    // The bar's own Live tab: where it sits depends on the tabs the catalogue turned on, which
    // arrive after the screen is entered.
    int barTab() const override {
        if (host_)
            for (size_t i = 0; i < host_->view().home.tabs.size(); ++i)
                if (host_->view().home.tabs[i].key == "live")
                    return static_cast<int>(i);
        return -1;
    }

    void enter(ScreenHost &host) override {
        host_ = &host;
        Motion &motion = host.motion();
        motion.set("live:shift", 0);
        motion.play("live:E:head", Tween(1, 0, ms::base, kit::easing(kit::motion::ease::out),
                                         kit::motion::stagger::pageBase));
        for (size_t i = 0; i < kCards; ++i)
            motion.set(id("E", i), 1);
        focusCard(host, 0, true);
        barWas_ = host.barFocused();
        nextAt_ = 0;
        read(host);
    }
    void leave(ScreenHost &host) override {
        host.services().cancel("live:stats");
        host.motion().erasePrefix("live:");
    }
    void focus(ScreenHost &host) override {
        syncFocus(host);
    }

    bool key(ScreenHost &host, Key key) override {
        size_t count = view_.cards.size();
        if (key == Key::Ok) {
            nextAt_ = 0; // a reading now
            host.invalidate();
            return true;
        }
        if (!count)
            return false;
        size_t next = focus_;
        if (key == Key::Left && focus_ % kColumns)
            next = focus_ - 1;
        else if (key == Key::Right && focus_ % kColumns + 1 < kColumns && focus_ + 1 < count)
            next = focus_ + 1;
        else if (key == Key::Up && focus_ >= kColumns)
            next = focus_ - kColumns;
        else if (key == Key::Down && focus_ + kColumns < count)
            next = focus_ + kColumns;
        if (next != focus_)
            focusCard(host, next, false);
        // An Up from the top row is the bar's.
        return key != Key::Up || focus_ >= kColumns;
    }
    void click(ScreenHost &host, const std::string &hit) override {
        if (hit.compare(0, 10, "live:card:") != 0)
            return;
        size_t index = static_cast<size_t>(std::atoi(hit.c_str() + 10));
        if (index < view_.cards.size()) {
            focusCard(host, index, false);
            nextAt_ = 0;
        }
    }

    void response(ScreenHost &host, const Response &response) override {
        if (response.channel != "live:stats")
            return;
        reading_ = false;
        const double now = host.nowMs();
        Json data = response.json();
        if (!response.ok() || !data.valid() || (data.has("ok") && !data["ok"].boolean())) {
            failed_ = true;
            nextAt_ = now + kPollMs;
            host.invalidate();
            return;
        }
        failed_ = false;
        // The daemon answers with what it knows while its first reading is still running.
        nextAt_ = now + (data["loading"].boolean() ? kRetryMs : kPollMs);
        stats_ = data;
        lastGoodAt_ = now;
        double load = liveLoad(data);
        if (load >= 0) {
            loads_.push_back(static_cast<float>(load));
            if (loads_.size() > kHistory)
                loads_.erase(loads_.begin());
        }
        adopt(host);
        host.invalidate();
    }

    bool update(ScreenHost &host) override {
        syncFocus(host);
        const double now = host.nowMs();
        if (!reading_ && now >= nextAt_)
            read(host);
        // "updated 12 s ago" changes by the second, but only matters to the nearest five.
        long age = static_cast<long>((now - lastGoodAt_) / 5000);
        if (lastGoodAt_ > 0 && age != ageBucket_) {
            ageBucket_ = age;
            host.invalidate();
        }
        return false;
    }

    void build(ScreenHost &host, kit::Paint paint) override {
        Motion &motion = host.motion();
        double head = motion.value("live:E:head");
        kit::PageHead page;
        page.title = view_.cards.empty() ? say::msg("tv.live.title", "Live") : view_.title;
        page.subtitle = subtitle(host);
        page.enter = {static_cast<float>(1 - head), static_cast<float>(28 * head)};
        kit::pageHead(paint, page);
        if (view_.cards.empty()) {
            if (failed_)
                paint.glyphs.text(
                    paint.scene,
                    {kit::canvas::safeX, kit::layout::gridTop + 8, 1200, kit::type::body.line},
                    say::msg("tv.live.unreachable",
                             "The TV's server did not answer. Trying again."),
                    kit::type::body, kit::color::onSurface2);
            return;
        }
        const double shift = motion.value("live:shift");
        for (size_t i = 0; i < view_.cards.size(); ++i) {
            const LiveCard &card = view_.cards[i];
            kit::StatCard item;
            item.rest = kit::geometry::statCard(static_cast<int>(i));
            item.id = "live:card:" + std::to_string(i);
            item.icon = card.icon;
            item.label = card.label;
            if (card.numeric) {
                item.value = liveNumber(card, motion.value(id("v", i)));
                item.unit = card.unit;
            } else {
                item.value = card.text;
                item.textValue = card.word;
            }
            item.caption = card.caption;
            item.caption2 = card.caption2;
            if (card.bar >= 0)
                item.bar = static_cast<float>(motion.value(id("b", i)));
            item.warn = card.warn;
            item.days = card.days;
            if (card.spark)
                item.spark = sparkline();
            item.sparkShift = static_cast<float>(shift);
            item.focus = static_cast<float>(motion.value(id("f", i)));
            double hidden = motion.value(id("E", i));
            item.enter = {static_cast<float>(1 - hidden), static_cast<float>(28 * hidden)};
            kit::statCard(paint, item);
            if (!card.chips.empty())
                chips(paint, item, card.chips);
        }
    }

    Json snapshot() const override {
        Json value = Json::object();
        value.set("focus", Json::numeric(static_cast<double>(focus_)));
        value.set("cards", Json::numeric(static_cast<double>(view_.cards.size())));
        value.set("loaded", Json::flag(!view_.cards.empty()));
        value.set("failed", Json::flag(failed_));
        value.set("samples", Json::numeric(static_cast<double>(loads_.size())));
        return value;
    }

  private:
    static constexpr size_t kCards = 8;
    static std::string id(const char *name, size_t index) {
        return std::string("live:") + name + ":" + std::to_string(index);
    }

    void read(ScreenHost &host) {
        if (host.services().read("live:stats", Operation::Metrics)) {
            reading_ = true;
        } else
            nextAt_ = host.nowMs() + kRetryMs;
    }

    // A new reading: the cards are worked out again and what moves is retargeted. The first one
    // sets every value at rest and plays the entrance.
    void adopt(ScreenHost &host) {
        Motion &motion = host.motion();
        const ViewState &state = host.view();
        bool first = view_.cards.empty();
        view_ = liveView(stats_, state.hour, state.minute, state.weekday);
        focus_ = std::min(focus_, view_.cards.empty() ? 0 : view_.cards.size() - 1);
        for (size_t i = 0; i < view_.cards.size(); ++i) {
            const LiveCard &card = view_.cards[i];
            if (first) {
                motion.set(id("v", i), card.number);
                motion.set(id("b", i), card.bar);
            } else {
                motion.tween(id("v", i), card.number, kNumberMs,
                             kit::easing(kit::motion::ease::out));
                motion.tween(id("b", i), card.bar, ms::bars, kit::easing(kit::motion::ease::out));
            }
        }
        if (first) {
            Timeline timeline;
            for (size_t i = 0; i < view_.cards.size(); ++i)
                timeline.add(id("E", i),
                             Tween(1, 0, ms::base, kit::easing(kit::motion::ease::out),
                                   kit::motion::stagger::pageBase +
                                       kit::motion::stagger::pageItem *
                                           std::min<int>(static_cast<int>(i),
                                                         kit::motion::stagger::pageItemMax)));
            timeline.play(motion);
        }
        // The line starts at the last picture and slides one sample left to the new reading.
        if (loads_.size() > 1)
            motion.play("live:shift", Tween(-1, 0, kSlideMs, kit::easing(kit::motion::ease::out)));
        syncFocus(host, true);
    }

    std::vector<float> sparkline() const {
        float peak = 25; // a quiet TV's few percent would sit on the floor otherwise
        for (float sample : loads_)
            peak = std::max(peak, sample);
        std::vector<float> samples;
        for (float sample : loads_)
            samples.push_back(sample / peak);
        return samples;
    }

    std::string subtitle(ScreenHost &host) const {
        if (view_.cards.empty())
            return failed_ ? say::msg("tv.live.noAnswer", "Waiting for the TV")
                           : say::msg("tv.live.reading", "Reading the TV…");
        long seconds = static_cast<long>((host.nowMs() - lastGoodAt_) / 1000);
        Json vars = Json::object();
        vars.set("n", Json::text(std::to_string(seconds / 5 * 5)));
        std::string age = seconds < 8 ? say::msg("tv.live.updatedNow", "updated just now")
                                      : say::msg("tv.live.updatedAgo", "updated {n} s ago", vars);
        return view_.subtitle + (view_.subtitle.empty() ? "" : " · ") + age;
    }

    // Chips along the foot of a card, scaled and lifted with it.
    void chips(kit::Paint paint, const kit::StatCard &card,
               const std::vector<std::string> &labels) {
        Scene &scene = paint.scene;
        kit::focus::Look look = kit::focus::lookAt(card.rest, kit::motion::scale::stat, card.focus);
        scene.pushOpacity(card.enter.alpha);
        scene.pushTransform(Transform::translate(0, card.enter.dy));
        scene.pushTransform(kit::focus::transformAt(card.rest, look.scale));
        float x = card.rest.x + kit::layout::statPadX,
              y = card.rest.y + card.rest.h - kit::layout::statBottom - 40;
        for (const std::string &label : labels)
            x += kit::pillChip(paint, x, y, label, false) + 10;
        scene.popTransform();
        scene.popTransform();
        scene.popOpacity();
    }

    void focusCard(ScreenHost &host, size_t index, bool jump) {
        Motion &motion = host.motion();
        if (!jump && index == focus_)
            return;
        motion.tween(id("f", focus_), 0, ms::focus, kit::easing(kit::motion::ease::out));
        focus_ = index;
        bool visible = !host.barFocused();
        if (jump)
            motion.set(id("f", focus_), visible ? 1 : 0);
        else
            motion.tween(id("f", focus_), visible ? 1 : 0, ms::focus,
                         kit::easing(kit::motion::ease::out));
        host.invalidate();
    }
    // The page draws no focus while the top bar has it.
    void syncFocus(ScreenHost &host, bool force = false) {
        bool bar = host.barFocused();
        if (!force && bar == barWas_)
            return;
        barWas_ = bar;
        host.motion().tween(id("f", focus_), bar ? 0 : 1, ms::focus,
                            kit::easing(kit::motion::ease::out));
        host.invalidate();
    }

    ScreenHost *host_ = nullptr;
    Json stats_;
    LiveView view_;
    std::vector<float> loads_;
    size_t focus_ = 0;
    bool reading_ = false, failed_ = false, barWas_ = false;
    double nextAt_ = 0, lastGoodAt_ = 0;
    long ageBucket_ = -1;
};
} // namespace

std::unique_ptr<ScreenView> makeLiveScreen(const std::string &route) {
    return route == "live" || route == "tab:live" ? std::unique_ptr<ScreenView>(new LiveScreen())
                                                  : nullptr;
}
} // namespace nativetv
