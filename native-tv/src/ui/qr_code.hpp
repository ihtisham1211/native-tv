#pragma once
#include "ui/scene.hpp"
#include <string>

namespace nativetv {
// Where a QR code goes inside a tile so a phone can read it: the renderer draws "qr:<text>" as one
// nearest-filtered texture of (modules + 8) texels, the code with four quiet modules on each side,
// so every module is a whole number of screen pixels when the rect is a multiple of that size.
struct QrLayout {
    bool fits = false; // false: the text is too long to draw at a readable module size
    int modules = 0;   // the code's side
    int scale = 0;     // pixels per module
    Rect image;        // integer rect, centred in the tile
    std::string value; // the Image node's path
};
constexpr int kQrMinScale = 3; // a module of at least 3 px; smaller reads badly off a TV
QrLayout qrLayout(const std::string &text, Rect tile);
} // namespace nativetv
