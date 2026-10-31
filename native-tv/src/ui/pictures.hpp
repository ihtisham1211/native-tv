#pragma once
#include "graphics/image_loader.hpp"
#include <cmath>
#include <string>

namespace nativetv {
// The texels a picture is asked for at: its drawn size plus a tenth, so a focus scale up stays
// sharp. A caller that asks ahead of the draw (Home's icons) must ask for the same size, or the
// picture is decoded twice.
inline int pictureTexels(float side) {
    return static_cast<int>(std::lround(side * 1.1f));
}
// Where the scene gets pictures from: the main loop backs it with the ImageLoader, tests with
// nothing (every picture then stays a placeholder). Ids are the opaque references the models
// use ("app:<id>", "tmdb:w500:/x.jpg", "wallpaper:night@3").
class Pictures {
  public:
    virtual ~Pictures() = default;
    // Asks for the picture at the size it will be drawn; texture 0 until it is ready.
    virtual ImageView view(const std::string &id, int width, int height, ImageFit fit,
                           double nowMs) = 0;
};
} // namespace nativetv
