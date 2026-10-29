#pragma once
#include "graphics/glyph_atlas.hpp"
#include "graphics/icon_atlas.hpp"
#include "graphics/text.hpp"
#include "ui/kit/widgets.hpp"

namespace nativetv {
namespace kit {
// Text as laid-out glyph runs from the Figtree atlas and icons as atlas masks: what the TV draws.
// Measuring and drawing use the same layout, so tab and chip widths match the glyphs.
class RunGlyphs final : public Glyphs {
  public:
    RunGlyphs(TextRuns &runs, GlyphAtlas &atlas, IconAtlas &icons)
        : runs_(runs), atlas_(atlas), icons_(icons) {}
    float measure(const std::string &text, const TypeStyle &style) const override;
    void text(Scene &scene, Rect box, const std::string &text, const TypeStyle &style, Color color,
              Node::Align align, int maxLines) override;
    void icon(Scene &scene, Rect box, const std::string &name, Color tint) override;

  private:
    TextRuns &runs_;
    GlyphAtlas &atlas_;
    IconAtlas &icons_;
};
} // namespace kit
} // namespace nativetv
