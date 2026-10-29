#pragma once
#include <cstdlib>
#include <cstring>
#include <string>

namespace nativetv {
// Switches for measuring the renderer on a TV, read once from NATIVETV_DEBUG_SKIP (comma list,
// off by default): shadows, rings, images, text, icons, gradients, fills; wallimg, wallgrad,
// wallfill (or wall) for the same kinds when they cover most of the canvas; flat (no SDF math in
// the fragment shaders), noblend, finish (glFinish after the draws so submitMs is GPU time),
// half and quarter (render into a smaller viewport of the same swap).
struct DebugSkip {
    bool shadows = false, rings = false, images = false, text = false, icons = false,
         gradients = false, fills = false, wallImages = false, wallGradients = false,
         wallFills = false, flat = false, noCore = false, trace = false,
         noBlend = false, finish = false;
    // Fragment shader variant bits for fragmentSource().
    unsigned shaderVariant() const {
        return flat ? 1u : 0u;
    }
    float viewport = 1;
    bool any() const {
        return shadows || rings || images || text || icons || gradients || fills || wallImages ||
               wallGradients || wallFills;
    }
};
inline DebugSkip parseDebugSkip(const char *list) {
    DebugSkip skip;
    if (!list)
        return skip;
    std::string text = list;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(',', start);
        if (end == std::string::npos)
            end = text.size();
        std::string name = text.substr(start, end - start);
        start = end + 1;
        if (name == "shadows") skip.shadows = true;
        else if (name == "rings") skip.rings = true;
        else if (name == "images") skip.images = true;
        else if (name == "text") skip.text = true;
        else if (name == "icons") skip.icons = true;
        else if (name == "gradients") skip.gradients = true;
        else if (name == "fills") skip.fills = true;
        else if (name == "wallimg") skip.wallImages = true;
        else if (name == "wallgrad") skip.wallGradients = true;
        else if (name == "wallfill") skip.wallFills = true;
        else if (name == "wall") skip.wallImages = skip.wallGradients = skip.wallFills = true;
        else if (name == "flat") skip.flat = true;
        else if (name == "trace") skip.trace = true;
        else if (name == "nocore") skip.noCore = true;
        else if (name == "noblend") skip.noBlend = true;
        else if (name == "finish") skip.finish = true;
        else if (name == "half") skip.viewport = .5f;
        else if (name == "quarter") skip.viewport = .25f;
    }
    return skip;
}
inline DebugSkip &debugSkip() {
    static DebugSkip skip = parseDebugSkip(std::getenv("NATIVETV_DEBUG_SKIP"));
    return skip;
}
} // namespace nativetv
