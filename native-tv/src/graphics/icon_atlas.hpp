#pragma once
#include "graphics/a8_atlas.hpp"
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace nativetv {
struct IconMask {
    bool valid = false;
    int w = 0, h = 0; // pixels of the mask; the glyph is drawn into a size x size box
    TextureId texture = 0;
    Rect uv; // for Scene::icon(); normalised origin and size inside the atlas texture
};
// Single-colour SVGs (the Material set in icons/material, the keyboard keys in icons/)
// rasterised with nanosvg into one A8 atlas, once per (icon, size), on first use. Drawing is
// a colour uniform times the mask, so one icon serves every theme colour.
class IconAtlas {
    struct Source;
    std::string directory_;
    A8Atlas atlas_;
    std::map<std::string, IconMask> masks_;
    std::map<std::string, std::shared_ptr<Source>> sources_;

  public:
    // The pixel sizes the design draws icons at (glyphs in rounds, wells, tiles and the
    // keyboard); exact sizes avoid minifying a larger mask, which GLES2 NPOT textures cannot
    // mipmap.
    static const int kSizes[8];
    IconAtlas(TextureStore *store, const std::string &iconDirectory, int width = 1024,
              int height = 512);
    // `name` is a file below the directory without ".svg"; the material folder is tried
    // first. A missing icon is remembered as invalid.
    IconMask get(const std::string &name, int size);
    // Rasterises every name at every size now, so the first frame that shows them does not.
    // Warm only the pairs the design uses: all names at all kSizes would not fit one atlas.
    void prewarm(const std::vector<std::string> &names, const std::vector<int> &sizes);
    A8Atlas &atlas() {
        return atlas_;
    }
};
} // namespace nativetv
