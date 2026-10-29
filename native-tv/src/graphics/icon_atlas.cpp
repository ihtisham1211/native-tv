#include "graphics/icon_atlas.hpp"
#include "nanosvg.h"
#include "nanosvgrast.h"
#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace nativetv {
const int IconAtlas::kSizes[8] = {18, 24, 26, 28, 34, 38, 40, 52};
struct IconAtlas::Source {
    std::unique_ptr<NSVGimage, decltype(&nsvgDelete)> image;
    explicit Source(NSVGimage *parsed) : image(parsed, nsvgDelete) {}
};
IconAtlas::IconAtlas(TextureStore *store, const std::string &iconDirectory, int width, int height)
    : directory_(iconDirectory), atlas_(store, width, height) {}
IconMask IconAtlas::get(const std::string &name, int size) {
    std::string key = name + "@" + std::to_string(size);
    auto cached = masks_.find(key);
    if (cached != masks_.end())
        return cached->second;
    IconMask mask;
    auto source = sources_.find(name);
    if (source == sources_.end()) {
        NSVGimage *parsed = nullptr;
        if (name.find("..") == std::string::npos)
            for (const char *folder : {"/material/", "/"})
                if ((parsed = nsvgParseFromFile((directory_ + folder + name + ".svg").c_str(), "px",
                                                96)))
                    break;
        source = sources_.emplace(name, parsed ? std::make_shared<Source>(parsed) : nullptr).first;
    }
    NSVGimage *image = source->second ? source->second->image.get() : nullptr;
    if (image && image->width > 0 && image->height > 0 && size > 0 && size <= 256) {
        float scale = static_cast<float>(size) / std::max(image->width, image->height);
        int w = std::max(1, static_cast<int>(std::ceil(image->width * scale))),
            h = std::max(1, static_cast<int>(std::ceil(image->height * scale)));
        AtlasRect area;
        if (!atlas_.allocate(w, h, area)) {
            atlas_.clear();
            masks_.clear();
            atlas_.allocate(w, h, area);
        }
        std::vector<unsigned char> rgba(static_cast<size_t>(w) * h * 4), alpha(rgba.size() / 4);
        std::unique_ptr<NSVGrasterizer, decltype(&nsvgDeleteRasterizer)> rasterizer(
            nsvgCreateRasterizer(), nsvgDeleteRasterizer);
        nsvgRasterize(rasterizer.get(), image, 0, 0, scale, rgba.data(), w, h, w * 4);
        for (size_t i = 0; i < alpha.size(); ++i)
            alpha[i] = rgba[i * 4 + 3];
        atlas_.write(area, alpha.data(), w);
        mask.valid = true;
        mask.w = w;
        mask.h = h;
        mask.texture = atlas_.texture();
        mask.uv = {static_cast<float>(area.x) / atlas_.width(),
                   static_cast<float>(area.y) / atlas_.height(),
                   static_cast<float>(w) / atlas_.width(), static_cast<float>(h) / atlas_.height()};
    }
    masks_[key] = mask;
    return mask;
}
void IconAtlas::prewarm(const std::vector<std::string> &names, const std::vector<int> &sizes) {
    for (const auto &name : names)
        for (int size : sizes)
            get(name, size);
}
} // namespace nativetv
