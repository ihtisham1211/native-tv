#pragma once
#include "graphics/glyph_atlas.hpp"
#include "ui/scene.hpp"
#include <SDL.h>
#include <SDL_ttf.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace nativetv {
// One texture per string through SDL_ttf; the renderer uses it until it draws layoutText().
SDL_Surface *rasterText(TTF_Font *font, const Node &node);

enum class TextAlign { Left, Center, Right };
struct TextStyle {
    int size = 24;
    int weight = 400;
    float tracking = 0; // letter spacing in em, added after every glyph as CSS does
};
struct TextOptions {
    float maxWidth = 0;   // 0 lays the text out on unbounded lines
    int maxLines = 1;     // 0 keeps every line
    float lineHeight = 0; // pixels; 0 is the font's natural line height
    TextAlign align = TextAlign::Left;
    bool ellipsis = true; // the last line ends in "…" when text was cut
    // Round glyph positions to whole pixels for text at rest; leave off while it animates so
    // it moves smoothly instead of shimmering.
    bool snap = true;
};
// Quads are in pixels from the top-left of the text box.
struct TextLayout {
    std::vector<GlyphQuad> quads;
    float width = 0, height = 0; // widest line and lines x line height, before any alignment
    int lines = 0;
    bool truncated = false;
};

float measureText(GlyphAtlas &atlas, const std::string &text, const TextStyle &style);
// The text cut to fit maxWidth, ending in "…"; unchanged when it already fits.
std::string ellipsize(GlyphAtlas &atlas, const std::string &text, const TextStyle &style,
                      float maxWidth);
// The lines layoutText() would draw, "…" included.
std::vector<std::string> wrapLines(GlyphAtlas &atlas, const std::string &text,
                                   const TextStyle &style, const TextOptions &options);
// Places the atlas bitmaps the text needs. If that empties the atlas partway, the layout is
// redone once so its own glyphs are consistent; older layouts then need a fresh call (compare
// atlas().generation()).
TextLayout layoutText(GlyphAtlas &atlas, const std::string &text, const TextStyle &style,
                      const TextOptions &options);

// The scene's text: layouts remembered across frames so unchanged text allocates nothing, and
// handed to the renderer as runs. A handle is good for the frame it was returned in. If the
// atlas fills up and is emptied after a run was laid out, run() lays it out again.
class TextRuns final : public TextRunSource {
    struct Entry {
        std::string text;
        TextStyle style;
        TextOptions options;
        unsigned generation = 0;
        TextLayout layout;
    };
    GlyphAtlas &atlas_;
    std::unordered_map<uint64_t, Entry> entries_;
    std::vector<Entry *> frame_;

    void refresh(Entry &entry) const;

  public:
    explicit TextRuns(GlyphAtlas &atlas) : atlas_(atlas) {}
    // Call before building a frame's scene: last frame's handles end, and layouts are dropped
    // wholesale if too many accumulated (a clock produces one per minute).
    void beginFrame();
    // 0 for empty text.
    RunHandle layout(const std::string &text, const TextStyle &style, const TextOptions &options);
    const TextLayout *layoutOf(RunHandle handle) const;
    bool run(RunHandle handle, TextRun &out) const override;
};
} // namespace nativetv
