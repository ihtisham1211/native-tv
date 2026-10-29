#pragma once
#include "ui/kit/widgets.hpp"

namespace nativetv {
// The nativeTv mark in `box` (square): the stem and arch in the page's ink, the dot in green.
void drawMark(kit::Paint paint, Rect box, float alpha = 1);
// The boot cover: Home's ink, the mark fading in over the boot fade, three dots breathing under
// it. It needs nothing from the server, so it is drawn before the daemon answers.
void drawSplash(kit::Paint paint, double nowMs);
} // namespace nativetv
