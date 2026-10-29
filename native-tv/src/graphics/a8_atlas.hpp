#pragma once
#include "ui/scene.hpp"
#include <cstdint>
#include <vector>

namespace nativetv {
struct AtlasRect {
    int x = 0, y = 0, w = 0, h = 0;
};
// One single-channel GPU texture with shelf packing. Each write goes straight to the texture
// as a small sub-rectangle, so the bytes sent per frame follow the new glyphs and icons, not
// the size of the atlas. The store must outlive the atlas; with no store the atlas only packs.
class A8Atlas {
    struct Shelf {
        int y, height, used;
    };
    TextureStore *store_;
    TextureId texture_ = 0;
    int width_, height_, nextShelfY_ = 0;
    unsigned generation_ = 0;
    std::vector<Shelf> shelves_;
    std::vector<uint8_t> tight_;
    void zeroTexture();

  public:
    A8Atlas(TextureStore *store, int width, int height);
    ~A8Atlas();
    A8Atlas(const A8Atlas &) = delete;
    A8Atlas &operator=(const A8Atlas &) = delete;
    // Reserves w x h pixels plus a one-pixel empty gutter on the right and bottom, so linear
    // filtering never reads a neighbour. False when nothing fits; the caller decides whether
    // to clear().
    bool allocate(int w, int h, AtlasRect &out);
    // `source` rows are `sourcePitch` bytes apart.
    void write(const AtlasRect &area, const uint8_t *source, int sourcePitch);
    // Empties the atlas and its texture and bumps generation(); every rect handed out before
    // is invalid.
    void clear();
    TextureId texture() const {
        return texture_;
    }
    int width() const {
        return width_;
    }
    int height() const {
        return height_;
    }
    unsigned generation() const {
        return generation_;
    }
    // Rows from the top that hold anything; a measure of how full the atlas is.
    int usedRows() const {
        return nextShelfY_;
    }
};
} // namespace nativetv
