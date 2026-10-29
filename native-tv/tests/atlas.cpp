#include "graphics/a8_atlas.hpp"
#include "graphics/glyph_atlas.hpp"
#include "graphics/icon_atlas.hpp"
#include "graphics/text.hpp"
#include <cmath>
#include <cstdio>
#include <stdexcept>

using namespace nativetv;
namespace {
int checks = 0;
void check(bool condition, const char *message) {
    ++checks;
    if (!condition)
        throw std::runtime_error(message);
}
// Mirrors every texture so tests can look at what the GPU would hold.
struct StubStore final : TextureStore {
    struct Texture {
        int width = 0, height = 0, bytesPerPixel = 1;
        std::vector<uint8_t> pixels;
        bool alive = true;
    };
    std::vector<Texture> textures;
    size_t uploadedBytes = 0;
    int updates = 0;
    TextureId create(int width, int height, TextureFormat format, bool) override {
        Texture texture;
        texture.width = width;
        texture.height = height;
        texture.bytesPerPixel = format == TextureFormat::Alpha8 ? 1 : 4;
        texture.pixels.assign(static_cast<size_t>(width) * height * texture.bytesPerPixel, 0xee);
        textures.push_back(texture);
        return static_cast<TextureId>(textures.size());
    }
    void update(TextureId id, int x, int y, int width, int height, const void *pixels) override {
        Texture &texture = textures.at(id - 1);
        check(x >= 0 && y >= 0 && x + width <= texture.width && y + height <= texture.height,
              "An update stays inside its texture");
        const uint8_t *source = static_cast<const uint8_t *>(pixels);
        for (int row = 0; row < height; ++row)
            std::copy(source + static_cast<size_t>(row) * width * texture.bytesPerPixel,
                      source + static_cast<size_t>(row + 1) * width * texture.bytesPerPixel,
                      &texture.pixels[(static_cast<size_t>(y + row) * texture.width + x) *
                                      texture.bytesPerPixel]);
        uploadedBytes += static_cast<size_t>(width) * height * texture.bytesPerPixel;
        ++updates;
    }
    void destroy(TextureId id) override {
        textures.at(id - 1).alive = false;
    }
    size_t uploadBudget() const override {
        return 1 << 20;
    }
    // Sum of the coverage inside a normalised rectangle of an A8 texture.
    long coverage(TextureId id, float u0, float v0, float u1, float v1) const {
        const Texture &texture = textures.at(id - 1);
        long sum = 0;
        for (int y = static_cast<int>(std::lround(v0 * texture.height));
             y < static_cast<int>(std::lround(v1 * texture.height)); ++y)
            for (int x = static_cast<int>(std::lround(u0 * texture.width));
                 x < static_cast<int>(std::lround(u1 * texture.width)); ++x)
                sum += texture.pixels[static_cast<size_t>(y) * texture.width + x];
        return sum;
    }
};
bool near(float a, float b, float tolerance) {
    return std::fabs(a - b) <= tolerance;
}
bool validUtf8(const std::string &text) {
    for (size_t i = 0; i < text.size();) {
        unsigned char lead = static_cast<unsigned char>(text[i]);
        size_t length = lead < 0x80 ? 1 : (lead >> 5) == 6 ? 2 : (lead >> 4) == 14 ? 3 : 4;
        if (lead >= 0x80 && (lead >> 6) == 2)
            return false;
        for (size_t k = 1; k < length; ++k)
            if (i + k >= text.size() || (static_cast<unsigned char>(text[i + k]) & 0xc0) != 0x80)
                return false;
        i += length;
    }
    return true;
}

void testPacking() {
    StubStore store;
    A8Atlas atlas(&store, 256, 256);
    check(atlas.texture() == 1 && store.textures[0].pixels[100] == 0,
          "A new atlas texture is zero filled");
    std::vector<AtlasRect> rects;
    unsigned seed = 7;
    for (;;) {
        seed = seed * 1103515245u + 12345u;
        int w = 4 + static_cast<int>((seed >> 16) % 28), h = 6 + static_cast<int>((seed >> 8) % 30);
        AtlasRect rect;
        if (!atlas.allocate(w, h, rect))
            break;
        rects.push_back(rect);
    }
    check(rects.size() > 60, "Mixed sizes pack densely before the atlas reports full");
    for (size_t i = 0; i < rects.size(); ++i) {
        check(rects[i].x >= 0 && rects[i].y >= 0 && rects[i].x + rects[i].w <= 256 &&
                  rects[i].y + rects[i].h <= 256,
              "Every allocation lies inside the atlas");
        for (size_t k = i + 1; k < rects.size(); ++k) {
            // With the one-pixel gutter, neighbours never touch.
            bool apart = rects[i].x + rects[i].w + 1 <= rects[k].x ||
                         rects[k].x + rects[k].w + 1 <= rects[i].x ||
                         rects[i].y + rects[i].h + 1 <= rects[k].y ||
                         rects[k].y + rects[k].h + 1 <= rects[i].y;
            check(apart, "Allocations never overlap and keep a gutter");
        }
    }
    AtlasRect tooBig;
    check(!atlas.allocate(257, 4, tooBig) && !atlas.allocate(0, 4, tooBig),
          "Impossible sizes are refused");
    // A short item goes onto an existing shelf instead of opening a new one.
    A8Atlas shelves(nullptr, 64, 64);
    AtlasRect tall, shortOne;
    shelves.allocate(10, 30, tall);
    shelves.allocate(10, 10, shortOne);
    check(shortOne.y == tall.y && shortOne.x > tall.x, "A shorter item shares a taller shelf");
    check(shelves.usedRows() == 31, "Sharing a shelf opens no new rows");

    A8Atlas writable(&store, 32, 32);
    AtlasRect area;
    writable.allocate(3, 2, area);
    const uint8_t source[] = {1, 2, 3, 99, 4, 5, 6, 99}; // pitch 4: the 99s are padding
    size_t before = store.uploadedBytes;
    writable.write(area, source, 4);
    const auto &texture = store.textures[writable.texture() - 1];
    check(texture.pixels[area.y * 32 + area.x] == 1 &&
              texture.pixels[(area.y + 1) * 32 + area.x + 2] == 6,
          "A write lands at its rectangle");
    check(texture.pixels[area.y * 32 + area.x + 3] == 0, "Row padding is not copied");
    check(store.uploadedBytes - before == 6, "A write sends only its own pixels");
    unsigned generation = writable.generation();
    writable.clear();
    check(writable.generation() == generation + 1 && texture.pixels[area.y * 32 + area.x] == 0,
          "clear() empties the texture and bumps the generation");
    AtlasRect again;
    writable.allocate(3, 2, again);
    check(again.x == 0 && again.y == 0, "An emptied atlas packs from the top again");
}

void testGlyphs(GlyphAtlas &glyphs, StubStore &store) {
    check(glyphs.ok(), "All four Figtree weights open");
    FontMetrics metrics = glyphs.metrics(31, 400);
    check(metrics.ascent == 29 && metrics.descent == 8, "31 px Figtree has a 29/8 ascent/descent");
    Glyph &capital = glyphs.glyph(31, 400, 'H');
    check(capital.advance > 15 && capital.advance < 30 && capital.index != 0,
          "A glyph has an unhinted advance");
    check(&glyphs.glyph(31, 400, 'H') == &capital, "Glyph metrics are cached at a fixed address");
    check(glyphs.glyph(31, 400, 'H').advance != glyphs.glyph(32, 400, 'H').advance,
          "Sizes are separate entries");
    check(glyphs.glyph(31, 300, 'H').advance != glyphs.glyph(31, 600, 'H').advance,
          "Weights are separate faces");
    check(glyphs.glyph(31, 700, 'H').advance == glyphs.glyph(31, 600, 'H').advance,
          "Weight 700 uses SemiBold");
    size_t before = store.uploadedBytes;
    check(glyphs.place(capital) && capital.width > 10 && capital.height > 15 &&
              capital.bearingY > 15,
          "A placed glyph has a bitmap");
    size_t afterFirst = store.uploadedBytes;
    check(afterFirst - before == static_cast<size_t>(capital.width) * capital.height,
          "Placing uploads exactly the bitmap");
    glyphs.place(capital);
    check(store.uploadedBytes == afterFirst, "Placing twice uploads once");
    check(store.coverage(glyphs.atlas().texture(), capital.u0, capital.v0, capital.u1, capital.v1) >
              1000,
          "The atlas texture holds the glyph's coverage");
    for (uint32_t codepoint :
         {0x2026u, 0x2019u, 0x2013u, 0x2014u, 0x2022u, 0x00b0u, 0x00b7u, 0x00e9u, 0x00d7u, 0x20acu})
        check(glyphs.glyph(31, 400, codepoint).index != 0,
              "The Latin subset covers the punctuation and symbols the screens use");
    Glyph &space = glyphs.glyph(31, 400, ' ');
    check(space.advance > 5 && glyphs.place(space) && space.width == 0, "A space has no bitmap");
    Glyph &control = glyphs.glyph(31, 400, '\n');
    check(control.advance == 0, "Control characters have no advance");
    unsigned a = glyphs.glyph(31, 400, 'A').index, v = glyphs.glyph(31, 400, 'V').index,
             h = capital.index;
    check(glyphs.kerning(72, 300, a, v) < -1.0f, "GPOS pair kerning pulls A and V together");
    check(glyphs.kerning(72, 300, h, h) == 0, "Unkerned pairs add nothing");
    check(near(glyphs.kerning(72, 300, a, v) * 2, glyphs.kerning(144, 300, a, v), 0.01f),
          "Kerning scales with the size");
}

void testMeasure(GlyphAtlas &glyphs) {
    // Widths the prototype's browser measured for the tab labels (147.3, 134.8 and 118.8 less
    // the 64 px of padding), 31 px, weight 400, letter spacing -0.01em.
    TextStyle nav{31, 400, -0.01f};
    check(near(measureText(glyphs, "Home", nav), 83.3f, 0.6f),
          "\"Home\" measures like the browser");
    check(near(measureText(glyphs, "Apps", nav), 70.8f, 0.6f),
          "\"Apps\" measures like the browser");
    check(near(measureText(glyphs, "Live", nav), 54.8f, 0.6f),
          "\"Live\" measures like the browser");
    TextStyle plain{31, 400, 0};
    TextStyle spaced{31, 400, 0.1f};
    check(near(measureText(glyphs, "Home", spaced) - measureText(glyphs, "Home", plain), 4 * 3.1f,
               0.01f),
          "Tracking adds its em fraction after every glyph");
    check(measureText(glyphs, "", plain) == 0, "Empty text has no width");
    check(measureText(glyphs, "Café", plain) > measureText(glyphs, "Cafe", plain) - 1 &&
              measureText(glyphs, "Caf\xc3\xa9", plain) == measureText(glyphs, "Café", plain),
          "UTF-8 text measures by character");
    check(measureText(glyphs, "\xc3", plain) == measureText(glyphs, "\xef\xbf\xbd", plain),
          "A broken UTF-8 sequence measures as the replacement character");
}

void testEllipsisAndWrap(GlyphAtlas &glyphs) {
    TextStyle body{24, 400, 0};
    std::string title = "The Quick Brown Fox Jumps Over The Lazy Dog";
    float full = measureText(glyphs, title, body);
    check(ellipsize(glyphs, title, body, full + 1) == title, "Text that fits is unchanged");
    std::string cut = ellipsize(glyphs, title, body, full / 2);
    check(cut.size() > 6 && cut.compare(cut.size() - 3, 3, "…") == 0 &&
              measureText(glyphs, cut, body) <= full / 2 && cut != title,
          "A long title is cut and ends in an ellipsis within the width");
    size_t kept = cut.size() - 3, next = kept;
    while (title[next] == ' ')
        ++next;
    check(measureText(glyphs, title.substr(0, next + 1) + "…", body) > full / 2,
          "The cut keeps as many characters as fit");
    std::string accented = ellipsize(glyphs, "Caf\xc3\xa9 Caf\xc3\xa9 Caf\xc3\xa9", body, 90);
    check(accented.size() > 3 && accented.compare(accented.size() - 3, 3, "…") == 0 &&
              validUtf8(accented),
          "Cutting never splits a UTF-8 character");

    TextOptions two;
    two.maxWidth = full / 2 + 30;
    two.maxLines = 2;
    auto lines = wrapLines(glyphs, title, body, two);
    check(lines.size() == 2, "The title wraps onto two lines");
    check(lines[0] == "The Quick Brown Fox Jumps" || lines[0] == "The Quick Brown Fox",
          "Lines break at spaces");
    for (const auto &line : lines)
        check(measureText(glyphs, line, body) <= two.maxWidth, "Every line fits the width");
    TextOptions unlimited = two;
    unlimited.maxLines = 0;
    auto all = wrapLines(glyphs, title, body, unlimited);
    std::string joined;
    for (const auto &line : all)
        joined += (joined.empty() ? "" : " ") + line;
    check(joined == title, "Wrapping without a line limit keeps every word");
    TextOptions clamp = two;
    clamp.maxWidth = 150;
    clamp.maxLines = 2;
    auto clamped = wrapLines(glyphs, title, body, clamp);
    check(clamped.size() == 2 && clamped[1].compare(clamped[1].size() - 3, 3, "…") == 0 &&
              measureText(glyphs, clamped[1], body) <= 150,
          "The last allowed line ends in an ellipsis within the width");
    clamp.ellipsis = false;
    clamped = wrapLines(glyphs, title, body, clamp);
    check(clamped.size() == 2 && clamped[1].find("…") == std::string::npos &&
              measureText(glyphs, clamped[1], body) <= 150,
          "Without the ellipsis option the last line is just cut");

    TextOptions narrow;
    narrow.maxWidth = 60;
    narrow.maxLines = 0;
    auto broken = wrapLines(glyphs, "Supercalifragilisticexpialidocious", body, narrow);
    check(broken.size() > 2, "A word wider than the line breaks between characters");
    for (const auto &line : broken)
        check(!line.empty() && measureText(glyphs, line, body) <= 60, "Each piece fits");
    auto hard = wrapLines(glyphs, "one\ntwo\n\nfour", body, narrow);
    check(hard.size() == 4 && hard[0] == "one" && hard[2].empty() && hard[3] == "four",
          "A newline starts a new line, and blank lines stay");
    check(wrapLines(glyphs, "   ", body, narrow).size() == 1 &&
              wrapLines(glyphs, "", body, narrow).size() == 1,
          "Blank text is one empty line");
    TextOptions single;
    single.maxWidth = 100000;
    auto spaces = wrapLines(glyphs, "  leading   gaps  ", body, single);
    check(spaces.size() == 1 && spaces[0] == "  leading   gaps",
          "Spaces inside a line stay and the trailing ones go");
}

void testLayout(GlyphAtlas &glyphs, StubStore &store) {
    TextStyle display{72, 300, -0.02f};
    TextOptions options;
    options.maxWidth = 1400;
    options.lineHeight = 76;
    TextLayout title = layoutText(glyphs, "Midnight Harbour", display, options);
    check(title.lines == 1 && !title.truncated && title.height == 76 &&
              title.quads.size() == 15, // the space has no quad
          "A one-line title lays out 15 visible glyphs in a 76 px box");
    for (const auto &quad : title.quads) {
        check(quad.x >= 0 && quad.x + quad.w <= title.width + 4 && quad.y >= 0 &&
                  quad.y + quad.h <= 76 + 14 && quad.x == std::floor(quad.x),
              "Glyphs sit inside the box on whole pixels");
        check(store.coverage(glyphs.atlas().texture(), quad.u0, quad.v0, quad.u1, quad.v1) > 0,
              "Every quad points at a glyph in the atlas");
    }
    // The baseline: half-leading (76 - (68 + 18)) / 2 = -5, plus the ascent.
    FontMetrics metrics = glyphs.metrics(72, 300);
    TextLayout capital = layoutText(glyphs, "H", display, options);
    Glyph &h = glyphs.glyph(72, 300, 'H');
    float baseline = std::round(static_cast<float>(metrics.ascent) +
                                (76.0f - (metrics.ascent + metrics.descent)) / 2);
    check(capital.quads.size() == 1 && near(capital.quads[0].y + capital.quads[0].h,
                                            baseline - h.bearingY + h.height, 0.01f),
          "The baseline follows CSS half-leading");

    TextOptions centre = options;
    centre.align = TextAlign::Center;
    TextOptions right = options;
    right.align = TextAlign::Right;
    TextLayout left = layoutText(glyphs, "Home", display, options);
    TextLayout middle = layoutText(glyphs, "Home", display, centre);
    TextLayout end = layoutText(glyphs, "Home", display, right);
    check(middle.quads[0].x - left.quads[0].x > 600 && end.quads[0].x - middle.quads[0].x > 600,
          "Centre and right alignment shift the text by the free width");
    check(near(end.quads.back().x + end.quads.back().w, 1400, 20),
          "Right-aligned text ends at the edge");

    TextOptions smooth = centre;
    smooth.snap = false;
    smooth.maxWidth = 1001.5f;
    TextLayout moving = layoutText(glyphs, "Home", display, smooth);
    bool fractional = false;
    for (const auto &quad : moving.quads)
        fractional = fractional || quad.x != std::floor(quad.x);
    check(fractional, "With snapping off glyphs keep fractional positions");

    TextOptions clamp;
    clamp.maxWidth = 200;
    clamp.maxLines = 2;
    clamp.lineHeight = 30;
    TextLayout paragraph =
        layoutText(glyphs, "A very long description that cannot possibly fit", {24, 400, 0}, clamp);
    check(paragraph.lines == 2 && paragraph.truncated && paragraph.height == 60,
          "A clamped paragraph reports two lines and the truncation");
    float lowest = 0;
    for (const auto &quad : paragraph.quads)
        lowest = std::max(lowest, quad.y + quad.h);
    check(lowest > 40 && lowest < 66, "The second line is drawn below the first");
    check(layoutText(glyphs, "", display, options).quads.empty(), "Empty text has no quads");

    // Repeated layouts reuse the placed glyphs.
    size_t before = store.uploadedBytes;
    layoutText(glyphs, "Midnight Harbour", display, options);
    check(store.uploadedBytes == before, "Laying out the same text again uploads nothing");
}

void testRuns(GlyphAtlas &glyphs) {
    TextRuns runs(glyphs);
    TextStyle style{28, 500, 0.01f};
    TextOptions options;
    options.maxWidth = 300;
    runs.beginFrame();
    RunHandle first = runs.layout("Living room", style, options);
    check(first != 0 && runs.layout("", style, options) == 0,
          "Text gets a handle, empty text none");
    TextRun run;
    check(runs.run(first, run) && run.texture == glyphs.atlas().texture() && run.count == 10,
          "A run points at the atlas texture and its glyph quads");
    const GlyphQuad *address = run.glyphs;
    runs.beginFrame();
    RunHandle second = runs.layout("Living room", style, options);
    check(runs.run(second, run) && run.glyphs == address,
          "Unchanged text reuses its layout between frames");
    runs.layout("Living room", style, TextOptions());
    check(runs.layoutOf(second)->quads.data() == address && !runs.run(999, run),
          "A different option set is a separate entry, and bad handles fail");
    runs.beginFrame();
    check(runs.layoutOf(first) == nullptr, "Handles end with their frame");
}

void testAtlasFull(StubStore &store) {
    // 96 x 96 holds about ten 30 px capitals; the layout that overflows it empties the atlas
    // and must still come back consistent.
    GlyphAtlas small(&store, NATIVETV_ASSETS "/fonts", 96, 96);
    TextStyle style{30, 400, 0};
    TextOptions options;
    layoutText(small, "WXYZVUTS", style, options);
    unsigned generation = small.atlas().generation();
    TextLayout layout = layoutText(small, "Home Quick", style, options);
    check(small.atlas().generation() > generation,
          "Overflowing the atlas empties it and bumps the generation");
    check(layout.quads.size() == 9, "A layout still comes back after the atlas was emptied");
    bool allPresent = true;
    for (const auto &quad : layout.quads)
        allPresent = allPresent && store.coverage(small.atlas().texture(), quad.u0, quad.v0,
                                                  quad.u1, quad.v1) > 0;
    check(allPresent, "Every glyph of that layout is in the emptied atlas");
    Glyph &again = small.glyph(30, 400, 'W');
    check(small.place(again) && again.placedIn == small.atlas().generation(),
          "A glyph lost in the clear is placed again on demand");
    TextRuns runs(small);
    runs.beginFrame();
    RunHandle early = runs.layout("WXYZ", style, options);
    layoutText(small, "Home Quick Brown Fox", style, options);
    TextRun run;
    check(runs.run(early, run) && run.count == 4, "A run laid out before the clear still draws");
    bool stillPresent = true;
    for (uint32_t i = 0; i < run.count; ++i)
        stillPresent =
            stillPresent && store.coverage(run.texture, run.glyphs[i].u0, run.glyphs[i].v0,
                                           run.glyphs[i].u1, run.glyphs[i].v1) > 0;
    check(stillPresent, "run() laid it out again after the atlas was emptied");
}

void testIcons(StubStore &store) {
    IconAtlas icons(&store, NATIVETV_ASSETS "/icons");
    IconMask gear = icons.get("settings", 24);
    check(gear.valid && gear.w == 24 && gear.h == 24 && gear.texture != 0,
          "A Material icon rasterises at the requested size");
    float u1 = gear.uv.x + gear.uv.w, v1 = gear.uv.y + gear.uv.h;
    check(u1 <= 1 && v1 <= 1 && store.coverage(gear.texture, gear.uv.x, gear.uv.y, u1, v1) > 3000,
          "The mask holds the glyph's coverage");
    int updates = store.updates;
    IconMask again = icons.get("settings", 24);
    check(store.updates == updates && again.uv.x == gear.uv.x && again.uv.y == gear.uv.y,
          "A second request reuses the packed mask");
    IconMask larger = icons.get("settings", 52);
    check(larger.w == 52 && (larger.uv.x != gear.uv.x || larger.uv.y != gear.uv.y),
          "Each size is its own mask");
    check(icons.get("key-shift", 30).valid, "Icons below the folder root are found too");
    check(!icons.get("no-such-icon", 24).valid && !icons.get("../fonts/x", 24).valid,
          "Missing names and path escapes are invalid");
    int sizes = 0;
    for (int size : IconAtlas::kSizes) {
        IconMask mask = icons.get("tv", size);
        check(mask.valid && mask.w == size, "Every design size rasterises");
        ++sizes;
    }
    check(sizes == 8, "The design draws icons at eight sizes");
    const char *names[] = {"access", "apps",   "bell", "cpu",    "display",
                           "ha",     "hdmi",   "play", "qr",     "search",
                           "wifi",   "volume", "temp", "shield", "remote"};
    for (const char *name : names) {
        IconMask mask = icons.get(name, 40);
        check(mask.valid && store.coverage(mask.texture, mask.uv.x, mask.uv.y,
                                           mask.uv.x + mask.uv.w, mask.uv.y + mask.uv.h) > 800,
              "Each spec icon has visible pixels");
    }
    // Everything the Material folder holds fits at its commonly used size.
    IconAtlas all(&store, NATIVETV_ASSETS "/icons");
    int found = 0;
    for (const char *name :
         {"access",     "apps",   "arrowdown", "arrowup",  "backspace", "battery",  "bell",
          "bolt",       "book",   "bulb",      "camera",   "cart",      "check",    "chevron",
          "clock",      "close",  "cloud",     "cpu",      "disk",      "display",  "enter",
          "eye",        "eyeoff", "fitness",   "game",     "globe",     "ha",       "hdmi",
          "headphones", "home",   "info",      "keyboard", "language",  "lock",     "memory",
          "movie",      "music",  "network",   "news",     "palette",   "phone",    "play",
          "power",      "qr",     "refresh",   "remote",   "search",    "settings", "shield",
          "shift",      "signal", "speed",     "sync",     "temp",      "timer",    "tune",
          "tv",         "volume", "wifi"}) {
        for (int size : {24, 40})
            found += all.get(name, size).valid ? 1 : 0;
    }
    check(found == 59 * 2, "All 59 Material icons rasterise at two sizes");
    check(all.atlas().usedRows() < all.atlas().height() && all.atlas().generation() == 0,
          "They share one atlas without emptying it");
    IconAtlas warmed(&store, NATIVETV_ASSETS "/icons");
    warmed.prewarm({"home", "tv"}, {24, 52});
    int warmedUpdates = store.updates;
    check(warmed.get("home", 52).valid && warmed.get("tv", 24).valid &&
              store.updates == warmedUpdates,
          "Prewarmed masks are already in the atlas");
    IconAtlas tiny(&store, NATIVETV_ASSETS "/icons", 64, 64);
    unsigned generation = tiny.atlas().generation();
    IconMask last;
    for (const char *name : {"tv", "home", "gear", "wifi", "cpu", "qr"})
        last = tiny.get(name, 40);
    check(tiny.atlas().generation() > generation && last.valid,
          "A full icon atlas empties itself and keeps working");
}
} // namespace

int main() {
    try {
        testPacking();
        StubStore store;
        GlyphAtlas glyphs(&store, NATIVETV_ASSETS "/fonts");
        testGlyphs(glyphs, store);
        testMeasure(glyphs);
        testEllipsisAndWrap(glyphs);
        testLayout(glyphs, store);
        testRuns(glyphs);
        testAtlasFull(store);
        testIcons(store);
        std::printf("%d atlas checks passed\n", checks);
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
    return 0;
}
