#include "graphics/glyph_atlas.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H
#include <vector>

namespace nativetv {
namespace {
const int kWeights[4] = {300, 400, 500, 600};
const char *const kFiles[4] = {"Figtree-Light.ttf", "Figtree-Regular.ttf", "Figtree-Medium.ttf",
                               "Figtree-SemiBold.ttf"};
int faceForWeight(int weight) {
    int best = 0;
    for (int i = 1; i < 4; ++i)
        if (std::abs(kWeights[i] - weight) < std::abs(kWeights[best] - weight))
            best = i;
    return best;
}
int bitCount(unsigned value) {
    int count = 0;
    for (; value; value &= value - 1)
        ++count;
    return count;
}
// FreeType only reads the legacy 'kern' table, which Figtree does not have, and the shaping
// engine behind SDL_ttf is not on every TV. This reads the one thing Latin text needs from
// GPOS: the x advance of the first glyph of a PairPos pair (format 1 glyph pairs and format 2
// class pairs, directly or through an Extension lookup). Every read is bounds checked, so a
// damaged font yields no kerning rather than a crash.
// ponytail: uses the first 'kern' feature whatever its script; per-script features if
// localisation returns.
class PairKerning {
    std::vector<uint8_t> data_;
    std::vector<std::vector<size_t>> lookups_;
    unsigned u16(size_t at) const {
        return at + 2 <= data_.size() ? (data_[at] << 8) | data_[at + 1] : 0;
    }
    int s16(size_t at) const {
        return static_cast<int16_t>(u16(at));
    }
    uint32_t u32(size_t at) const {
        return (u16(at) << 16) | u16(at + 2);
    }
    int coverage(size_t at, unsigned glyph) const {
        unsigned format = u16(at), count = u16(at + 2);
        if (format == 1) {
            unsigned low = 0, high = count;
            while (low < high) {
                unsigned middle = (low + high) / 2, value = u16(at + 4 + 2 * middle);
                if (value == glyph)
                    return static_cast<int>(middle);
                if (value < glyph)
                    low = middle + 1;
                else
                    high = middle;
            }
        } else if (format == 2) {
            for (unsigned i = 0; i < count; ++i) {
                size_t record = at + 4 + 6 * i;
                if (glyph >= u16(record) && glyph <= u16(record + 2))
                    return static_cast<int>(u16(record + 4) + glyph - u16(record));
            }
        }
        return -1;
    }
    unsigned glyphClass(size_t at, unsigned glyph) const {
        unsigned format = u16(at);
        if (format == 1) {
            unsigned start = u16(at + 2), count = u16(at + 4);
            return glyph >= start && glyph < start + count ? u16(at + 6 + 2 * (glyph - start)) : 0;
        }
        if (format == 2)
            for (unsigned i = 0, count = u16(at + 2); i < count; ++i) {
                size_t record = at + 4 + 6 * i;
                if (glyph >= u16(record) && glyph <= u16(record + 2))
                    return u16(record + 4);
            }
        return 0;
    }
    // True when this subtable decides the pair (even to zero), which ends the lookup.
    bool pair(size_t at, unsigned left, unsigned right, int &units) const {
        int covered = coverage(at + u16(at + 2), left);
        if (covered < 0)
            return false;
        unsigned format1 = u16(at + 4), format2 = u16(at + 6);
        size_t size1 = 2 * bitCount(format1), size2 = 2 * bitCount(format2);
        auto xAdvance = [&](size_t record) {
            return format1 & 4 ? s16(record + 2 * bitCount(format1 & 3)) : 0;
        };
        if (u16(at) == 1) {
            if (static_cast<unsigned>(covered) >= u16(at + 8))
                return false;
            size_t set = at + u16(at + 10 + 2 * covered), step = 2 + size1 + size2;
            for (unsigned i = 0, count = u16(set); i < count; ++i)
                if (u16(set + 2 + i * step) == right) {
                    units = xAdvance(set + 4 + i * step);
                    return true;
                }
            return false;
        }
        unsigned class1 = glyphClass(at + u16(at + 8), left),
                 class2 = glyphClass(at + u16(at + 10), right);
        if (class1 >= u16(at + 12) || class2 >= u16(at + 14))
            return false;
        units = xAdvance(at + 16 + (class1 * u16(at + 14) + class2) * (size1 + size2));
        return true;
    }

  public:
    explicit PairKerning(std::vector<uint8_t> table) : data_(std::move(table)) {
        size_t features = u16(6), lookupList = u16(8);
        if (data_.size() < 10 || !features || !lookupList)
            return;
        std::vector<unsigned> indices;
        for (unsigned i = 0, count = u16(features); i < count; ++i)
            if (u32(features + 2 + 6 * i) == 0x6b65726e) { // 'kern'
                size_t feature = features + u16(features + 6 + 6 * i);
                for (unsigned j = 0, n = u16(feature + 2); j < n; ++j)
                    indices.push_back(u16(feature + 4 + 2 * j));
                break;
            }
        std::sort(indices.begin(), indices.end());
        for (unsigned index : indices) {
            size_t lookup = lookupList + u16(lookupList + 2 + 2 * index);
            unsigned type = u16(lookup);
            std::vector<size_t> subtables;
            for (unsigned k = 0, n = u16(lookup + 4); k < n; ++k) {
                size_t subtable = lookup + u16(lookup + 6 + 2 * k);
                if (type == 9 && u16(subtable + 2) == 2)
                    subtable += u32(subtable + 4);
                else if (type != 2)
                    continue;
                subtables.push_back(subtable);
            }
            lookups_.push_back(subtables);
        }
    }
    int units(unsigned left, unsigned right) const {
        int total = 0;
        for (const auto &lookup : lookups_)
            for (size_t subtable : lookup) {
                int value = 0;
                if (pair(subtable, left, right, value)) {
                    total += value;
                    break;
                }
            }
        return total;
    }
};
} // namespace

struct GlyphAtlas::Impl {
    struct Face {
        FT_Face ft = nullptr;
        int size = 0;
        std::unique_ptr<PairKerning> kerning;
        std::unordered_map<uint32_t, int> pairs;
    };
    FT_Library library = nullptr;
    Face faces[4];
    ~Impl() {
        for (auto &face : faces)
            if (face.ft)
                FT_Done_Face(face.ft);
        if (library)
            FT_Done_FreeType(library);
    }
    FT_Face use(int index, int size) {
        Face &face = faces[index];
        if (face.size != size) {
            FT_Set_Pixel_Sizes(face.ft, 0, static_cast<FT_UInt>(size));
            face.size = size;
        }
        return face.ft;
    }
};

GlyphAtlas::GlyphAtlas(TextureStore *store, const std::string &fontDirectory, int width, int height)
    : impl_(new Impl), atlas_(store, width, height) {
    if (FT_Init_FreeType(&impl_->library))
        return;
    for (int i = 0; i < 4; ++i) {
        Impl::Face &face = impl_->faces[i];
        if (FT_New_Face(impl_->library, (fontDirectory + "/" + kFiles[i]).c_str(), 0, &face.ft)) {
            face.ft = nullptr;
            continue;
        }
        FT_ULong length = 0;
        if (FT_Load_Sfnt_Table(face.ft, FT_MAKE_TAG('G', 'P', 'O', 'S'), 0, nullptr, &length) ==
                0 &&
            length) {
            std::vector<uint8_t> table(length);
            if (FT_Load_Sfnt_Table(face.ft, FT_MAKE_TAG('G', 'P', 'O', 'S'), 0, table.data(),
                                   &length) == 0)
                face.kerning.reset(new PairKerning(std::move(table)));
        }
    }
}
GlyphAtlas::~GlyphAtlas() = default;
bool GlyphAtlas::ok() const {
    for (const auto &face : impl_->faces)
        if (!face.ft)
            return false;
    return true;
}
FontMetrics GlyphAtlas::metrics(int size, int weight) {
    FontMetrics result;
    FT_Face ft = impl_->faces[faceForWeight(weight)].ft;
    if (!ft)
        return result;
    double scale = static_cast<double>(size) / ft->units_per_EM;
    result.ascent = static_cast<int>(std::lround(ft->ascender * scale));
    result.descent = static_cast<int>(std::lround(-ft->descender * scale));
    result.lineGap = std::max(
        0, static_cast<int>(std::lround((ft->height - (ft->ascender - ft->descender)) * scale)));
    return result;
}
Glyph &GlyphAtlas::glyph(int size, int weight, uint32_t codepoint) {
    int face = faceForWeight(weight);
    uint64_t key =
        (static_cast<uint64_t>(face) << 48) | (static_cast<uint64_t>(size) << 32) | codepoint;
    auto found = glyphs_.find(key);
    if (found != glyphs_.end())
        return found->second;
    Glyph glyph;
    glyph.face = face;
    glyph.size = size;
    glyph.codepoint = codepoint;
    FT_Face ft = impl_->faces[face].ft;
    if (ft && codepoint >= 0x20) {
        impl_->use(face, size);
        glyph.index = FT_Get_Char_Index(ft, codepoint);
        if (FT_Load_Glyph(ft, glyph.index, FT_LOAD_NO_HINTING) == 0)
            glyph.advance = static_cast<float>(ft->glyph->linearHoriAdvance) / 65536.0f;
    }
    return glyphs_.emplace(key, glyph).first->second;
}
bool GlyphAtlas::place(Glyph &glyph) {
    if (glyph.placedIn == atlas_.generation())
        return true;
    FT_Face ft = impl_->faces[glyph.face].ft;
    if (!ft || !glyph.index) {
        // Control characters and characters the font lacks draw nothing; the missing glyph's
        // box is not worth an atlas slot.
        glyph.width = glyph.height = 0;
        glyph.placedIn = atlas_.generation();
        return true;
    }
    impl_->use(glyph.face, glyph.size);
    if (FT_Load_Glyph(ft, glyph.index, FT_LOAD_NO_HINTING | FT_LOAD_RENDER))
        return false;
    const FT_Bitmap &bitmap = ft->glyph->bitmap;
    glyph.bearingX = ft->glyph->bitmap_left;
    glyph.bearingY = ft->glyph->bitmap_top;
    glyph.width = static_cast<int>(bitmap.width);
    glyph.height = static_cast<int>(bitmap.rows);
    if (glyph.width && glyph.height) {
        AtlasRect area;
        if (!atlas_.allocate(glyph.width, glyph.height, area)) {
            atlas_.clear();
            if (!atlas_.allocate(glyph.width, glyph.height, area))
                return false;
        }
        atlas_.write(area, bitmap.buffer, std::abs(bitmap.pitch));
        glyph.u0 = static_cast<float>(area.x) / atlas_.width();
        glyph.v0 = static_cast<float>(area.y) / atlas_.height();
        glyph.u1 = static_cast<float>(area.x + area.w) / atlas_.width();
        glyph.v1 = static_cast<float>(area.y + area.h) / atlas_.height();
    }
    glyph.placedIn = atlas_.generation();
    return true;
}
float GlyphAtlas::kerning(int size, int weight, unsigned left, unsigned right) {
    Impl::Face &face = impl_->faces[faceForWeight(weight)];
    if (!face.kerning || !left || !right)
        return 0;
    auto found = face.pairs.find(left << 16 | right);
    int units = found != face.pairs.end()
                    ? found->second
                    : (face.pairs[left << 16 | right] = face.kerning->units(left, right));
    return static_cast<float>(units) * size / face.ft->units_per_EM;
}
} // namespace nativetv
