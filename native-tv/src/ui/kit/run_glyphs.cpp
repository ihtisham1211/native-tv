#include "ui/kit/run_glyphs.hpp"
#include <cmath>

namespace nativetv {
namespace kit {
namespace {
nativetv::TextStyle atlasStyle(const TypeStyle &style) {
    nativetv::TextStyle result;
    result.size = static_cast<int>(std::lround(style.px));
    result.weight = style.weight;
    result.tracking = style.tracking;
    return result;
}
TextAlign atlasAlign(Node::Align align) {
    return align == Node::Align::Center  ? TextAlign::Center
           : align == Node::Align::Right ? TextAlign::Right
                                         : TextAlign::Left;
}
} // namespace

float RunGlyphs::measure(const std::string &text, const TypeStyle &style) const {
    return measureText(atlas_, text, atlasStyle(style));
}
void RunGlyphs::text(Scene &scene, Rect box, const std::string &value, const TypeStyle &style,
                     Color color, Node::Align align, int maxLines) {
    TextOptions options;
    options.maxWidth = box.w;
    options.maxLines = maxLines;
    options.lineHeight = style.line;
    options.align = atlasAlign(align);
    RunHandle run = runs_.layout(value, atlasStyle(style), options);
    if (run)
        scene.textRun(box, run, color);
}
void RunGlyphs::icon(Scene &scene, Rect box, const std::string &name, Color tint) {
    if (name.empty())
        return;
    IconMask mask = icons_.get(name, static_cast<int>(std::lround(box.w)));
    if (mask.valid)
        scene.icon(box, mask.texture, mask.uv, tint);
}
} // namespace kit
} // namespace nativetv
