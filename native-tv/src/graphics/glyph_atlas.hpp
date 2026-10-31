#pragma once
#include "graphics/a8_atlas.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace nativetv {
struct FontMetrics {
    // Whole pixels, rounded one by one as a browser does, so line boxes match the web design.
    int ascent = 0, descent = 0, lineGap = 0;
    int naturalLineHeight() const {
        return ascent + descent + lineGap;
    }
};
struct Glyph {
    int face = 0, size = 0;
    uint32_t codepoint = 0;
    unsigned index = 0; // glyph index in the face; 0 is the font's "missing" glyph
    float advance = 0;  // unhinted, in pixels
    // Valid once GlyphAtlas::place() succeeded; bearingY is the height above the baseline.
    int bearingX = 0, bearingY = 0, width = 0, height = 0;
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    unsigned placedIn = ~0u; // atlas generation the bitmap and uv belong to
};
// Latin text from three static cuts of Google Sans Flex (a display cut for the big titles, and
// regular and medium text cuts) rasterised by FreeType into one A8 atlas on first use. Advances are unhinted so layout is identical on every FreeType.
// Metrics live as long as the object; only the bitmaps are lost when the atlas fills up and
// is emptied, which place() repairs on the next use.
class GlyphAtlas {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    A8Atlas atlas_;
    std::unordered_map<uint64_t, Glyph> glyphs_;

  public:
    // `store` may be null to lay text out without drawing it (tests, measuring tools).
    GlyphAtlas(TextureStore *store, const std::string &fontDirectory, int width = 2048,
               int height = 1024);
    ~GlyphAtlas();
    bool ok() const;
    // Weights 500 and up use the medium cut, lighter ones the regular cut; regular text of
    // 48 px and more uses the display cut.
    FontMetrics metrics(int size, int weight);
    // Advance and glyph index only, no bitmap. The reference stays valid for the lifetime of
    // the atlas.
    Glyph &glyph(int size, int weight, uint32_t codepoint);
    // Makes the bitmap resident; false only for a glyph larger than the whole atlas.
    bool place(Glyph &glyph);
    // GPOS pair kerning between two glyph indices, in pixels (negative pulls them together).
    float kerning(int size, int weight, unsigned left, unsigned right);
    A8Atlas &atlas() {
        return atlas_;
    }
};
} // namespace nativetv
