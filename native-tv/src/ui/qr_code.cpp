#include "ui/qr_code.hpp"
#include "qrcodegen.hpp"
#include <algorithm>
#include <cmath>

namespace nativetv {
QrLayout qrLayout(const std::string &text, Rect tile) {
    QrLayout layout;
    layout.value = "qr:" + text;
    try {
        layout.modules =
            qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::MEDIUM).getSize();
    } catch (const std::exception &) {
        return layout;
    }
    const int texels = layout.modules + 8;
    layout.scale = static_cast<int>(std::min(tile.w, tile.h)) / texels;
    if (layout.scale < kQrMinScale)
        return layout;
    const float side = static_cast<float>(texels * layout.scale);
    layout.image = {std::floor(tile.x + (tile.w - side) / 2), std::floor(tile.y + (tile.h - side) / 2),
                    side, side};
    layout.fits = true;
    return layout;
}
} // namespace nativetv
