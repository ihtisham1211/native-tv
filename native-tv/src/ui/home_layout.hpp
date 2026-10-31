#pragma once
#include "domain/models.hpp"
#include "ui/kit/geometry.hpp"
#include <algorithm>
#include <cmath>
#include <string>

// What Home's controller and view agree on: how wide a row's items are and the ids of the
// animated values. Ids stay under 16 characters so building one allocates nothing.
namespace nativetv {
namespace home {
constexpr size_t kGridColumns = static_cast<size_t>(kit::layout::tileCols);
constexpr float kGridPitch = kit::layout::tilePitchY;
constexpr size_t kSpotlightMax = 6;

// Hue in degrees, saturation and lightness in 0..1.
inline Color hsl(float hue, float saturation, float lightness) {
    float chroma = (1 - std::fabs(2 * lightness - 1)) * saturation, h = hue / 60;
    float x = chroma * (1 - std::fabs(std::fmod(h, 2.0f) - 1)), m = lightness - chroma / 2;
    float r = 0, g = 0, b = 0;
    if (h < 1) {
        r = chroma;
        g = x;
    } else if (h < 2) {
        r = x;
        g = chroma;
    } else if (h < 3) {
        g = chroma;
        b = x;
    } else if (h < 4) {
        g = x;
        b = chroma;
    } else if (h < 5) {
        r = x;
        b = chroma;
    } else {
        r = chroma;
        b = x;
    }
    return {r + m, g + m, b + m, 1};
}
inline unsigned hashOf(const std::string &text) {
    unsigned hash = 2166136261u;
    for (unsigned char c : text)
        hash = (hash ^ c) * 16777619u;
    return hash;
}
inline bool isApps(const CardRow &row) {
    return row.key == "app";
}
inline float itemWidth(const CardRow &row) {
    return isApps(row) ? kit::layout::appD : kit::layout::cardW;
}
// `named`: a row of apps that carries every name (the Apps page), so the circles stand wider.
inline float itemGap(const CardRow &row, bool named = false) {
    return named && isApps(row) ? kit::layout::appGapNamed : kit::layout::cardGap;
}
inline std::string id(const char *name, size_t row, size_t column) {
    return std::string(name) + ":" + std::to_string(row) + "." + std::to_string(column);
}
inline std::string id(const char *name, size_t index) {
    return std::string(name) + ":" + std::to_string(index);
}
// The pointer's id for an item: "card:<row>:<column>".
inline std::string hit(size_t row, size_t column) {
    return "card:" + std::to_string(row) + ":" + std::to_string(column);
}
// Row n's items on the shared canvas, before the row's own scroll and the stack's offset.
inline Rect item(const CardRow &row, size_t rowIndex, size_t column, bool named = false) {
    float width = itemWidth(row), gap = itemGap(row, named);
    return {kit::canvas::safeX + static_cast<float>(column) * (width + gap),
            kit::geometry::rowItemsY(static_cast<int>(rowIndex)), width,
            isApps(row) ? kit::layout::appD : kit::layout::cardH};
}
// The apps grid's scroll target while focus is on `column`: three rows fit under the page
// title, so the grid follows once focus passes the second row, and stops when the last row is
// in view.
inline float gridScrollTarget(size_t column, size_t count) {
    size_t rows = (count + kGridColumns - 1) / kGridColumns, at = column / kGridColumns;
    float visible = 3;
    float room = rows > visible ? (static_cast<float>(rows) - visible) * kGridPitch : 0;
    float wanted = at > 1 ? static_cast<float>(at - 1) * kGridPitch : 0;
    return -(wanted > room ? room : wanted);
}
// The Apps page by kind starts at layout::appsTop and holds this many rows whole.
constexpr size_t kKindRowsVisible =
    static_cast<size_t>((kit::canvas::height - kit::layout::appsTop) / kit::layout::rowPitch);
// Its scroll target while focus is on kind row `row` of `rows`: the rows keep their place until
// focus passes the second, then the page follows so the next row stays in view, and the last rows
// end flush with the screen.
inline float kindStackTarget(size_t row, size_t rows) {
    size_t last = rows > kKindRowsVisible ? rows - kKindRowsVisible : 0;
    size_t first = row > 1 ? row - 1 : 0;
    return -kit::layout::rowPitch * static_cast<float>(std::min(first, last));
}
} // namespace home
} // namespace nativetv
