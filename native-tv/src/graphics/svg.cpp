#include "graphics/svg.hpp"
#include "nanosvg.h"
#include "nanosvgrast.h"
#include <cmath>
#include <memory>

namespace nativetv {
SvgPixels rasterSvg(const std::string &path, float scale, bool iconMask) {
    SvgPixels result;
    std::unique_ptr<NSVGimage, decltype(&nsvgDelete)> image(
        nsvgParseFromFile(path.c_str(), "px", 96), nsvgDelete);
    if (!image || image->width <= 0 || image->height <= 0 || scale <= 0 ||
        image->width * scale > 2048 || image->height * scale > 2048)
        return result;
    std::unique_ptr<NSVGrasterizer, decltype(&nsvgDeleteRasterizer)> raster(nsvgCreateRasterizer(),
                                                                            nsvgDeleteRasterizer);
    if (!raster)
        return result;
    result.width = static_cast<int>(std::ceil(image->width * scale));
    result.height = static_cast<int>(std::ceil(image->height * scale));
    result.rgba.resize(static_cast<size_t>(result.width) * result.height * 4);
    nsvgRasterize(raster.get(), image.get(), 0, 0, scale, result.rgba.data(), result.width,
                  result.height, result.width * 4);
    if (iconMask)
        for (size_t i = 0; i < result.rgba.size(); i += 4)
            result.rgba[i] = result.rgba[i + 1] = result.rgba[i + 2] = 255;
    return result;
}
} // namespace nativetv
