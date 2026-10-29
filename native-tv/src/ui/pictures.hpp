#pragma once
#include "graphics/image_loader.hpp"
#include <string>

namespace nativetv {
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
