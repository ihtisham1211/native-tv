#include "graphics/a8_atlas.hpp"
#include <algorithm>
#include <cstring>

namespace nativetv {
A8Atlas::A8Atlas(TextureStore *store, int width, int height)
    : store_(store), width_(width), height_(height) {
    if (store_) {
        texture_ = store_->create(width, height, TextureFormat::Alpha8);
        zeroTexture();
    }
}
A8Atlas::~A8Atlas() {
    if (store_ && texture_)
        store_->destroy(texture_);
}
// A new texture's contents are undefined, and the gutters must read as empty.
void A8Atlas::zeroTexture() {
    if (!store_ || !texture_)
        return;
    std::vector<uint8_t> zeros(static_cast<size_t>(width_) * height_);
    store_->update(texture_, 0, 0, width_, height_, zeros.data());
}
bool A8Atlas::allocate(int w, int h, AtlasRect &out) {
    int paddedW = w + 1, paddedH = h + 1;
    if (w <= 0 || h <= 0 || paddedW > width_ || paddedH > height_)
        return false;
    // Best fit: the shortest shelf that is tall enough and has room, so small glyphs do not
    // open shelves of their own.
    Shelf *best = nullptr;
    for (auto &shelf : shelves_)
        if (shelf.height >= paddedH && width_ - shelf.used >= paddedW &&
            (!best || shelf.height < best->height))
            best = &shelf;
    if (!best) {
        if (nextShelfY_ + paddedH > height_)
            return false;
        shelves_.push_back({nextShelfY_, paddedH, 0});
        nextShelfY_ += paddedH;
        best = &shelves_.back();
    }
    out = {best->used, best->y, w, h};
    best->used += paddedW;
    return true;
}
void A8Atlas::write(const AtlasRect &area, const uint8_t *source, int sourcePitch) {
    if (!store_ || !texture_)
        return;
    if (sourcePitch != area.w) {
        tight_.resize(static_cast<size_t>(area.w) * area.h);
        for (int row = 0; row < area.h; ++row)
            std::memcpy(&tight_[static_cast<size_t>(row) * area.w],
                        source + static_cast<size_t>(row) * sourcePitch, area.w);
        source = tight_.data();
    }
    store_->update(texture_, area.x, area.y, area.w, area.h, source);
}
void A8Atlas::clear() {
    shelves_.clear();
    nextShelfY_ = 0;
    ++generation_;
    zeroTexture();
}
} // namespace nativetv
