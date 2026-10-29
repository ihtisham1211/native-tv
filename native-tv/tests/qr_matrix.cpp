// Prints where the TV draws the QR code for a text and what every module of it is, for
// qr_capture.py to compare with a capture of the screen.
#include "qrcodegen.hpp"
#include "ui/qr_code.hpp"
#include <cstdio>

int main(int argc, char **argv) {
    if (argc != 2)
        return 2;
    const nativetv::Rect tile = {80, 490, 264, 264}; // metrics::setup::qrTile
    nativetv::QrLayout layout = nativetv::qrLayout(argv[1], tile);
    auto code = qrcodegen::QrCode::encodeText(argv[1], qrcodegen::QrCode::Ecc::MEDIUM);
    std::printf("%d %d %d %d %d\n", layout.fits ? 1 : 0, layout.modules, layout.scale,
                static_cast<int>(layout.image.x), static_cast<int>(layout.image.y));
    for (int y = 0; y < code.getSize(); ++y) {
        for (int x = 0; x < code.getSize(); ++x)
            std::putchar(code.getModule(x, y) ? '1' : '0');
        std::putchar('\n');
    }
    return 0;
}
