#pragma once
// A CPU rasteriser that draws the batcher's triangles the way the GLSL in shaders.cpp does, so
// the batches can be checked against goldens without a GPU and the GPU against the batches.
// Keep coverage() and the fragment functions in step with shaders.cpp.
#include "graphics/batch.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace nativetv {
namespace soft {
struct Image {
    int width = 0, height = 0;
    bool alpha8 = false;
    std::vector<uint8_t> pixels;
};
using TextureTable = std::map<TextureId, Image>;

struct Framebuffer {
    int width, height;
    std::vector<float> rgb;
    Framebuffer(int w, int h, Color background) : width(w), height(h), rgb(size_t(w) * h * 3) {
        for (size_t i = 0; i < rgb.size(); i += 3) {
            rgb[i] = background.r;
            rgb[i + 1] = background.g;
            rgb[i + 2] = background.b;
        }
    }
    uint8_t byte(int x, int y, int channel) const {
        float v = rgb[(size_t(y) * width + x) * 3 + channel];
        return static_cast<uint8_t>(std::min(1.f, std::max(0.f, v)) * 255 + 0.5f);
    }
};

inline float smoothstep(float a, float b, float x) {
    float t = std::min(1.f, std::max(0.f, (x - a) / (b - a)));
    return t * t * (3 - 2 * t);
}
inline float edge(float x, float s) {
    return 1 - smoothstep(-s, s, x);
}
inline float coverage(const Vertex &v) {
    float r = v.localX < 0 ? (v.localY < 0 ? v.radii[0] : v.radii[3])
                           : (v.localY < 0 ? v.radii[1] : v.radii[2]);
    float qx = std::fabs(v.localX) - (v.halfW - r), qy = std::fabs(v.localY) - (v.halfH - r);
    float d = std::hypot(std::max(qx, 0.f), std::max(qy, 0.f)) + std::min(std::max(qx, qy), 0.f) - r;
    return std::min(1.f, std::max(0.f, edge(d - v.outer, v.soft) - edge(d + v.inner, v.soft)));
}

struct Fragment {
    float r, g, b, a;
};
inline Fragment shade(Program program, const Vertex &v, const float *color, const Batch &batch,
                      const TextureTable &textures) {
    float cr = color[0], cg = color[1], cb = color[2], ca = color[3];
    if (program == Program::Sdf)
        return {cr, cg, cb, ca * coverage(v)};
    int slot = std::min(kTextureSlots - 1, static_cast<int>(v.slot + 0.5f));
    float tr = 1, tg = 1, tb = 1, ta = 1;
    auto found = slot < batch.textureCount ? textures.find(batch.textures[slot]) : textures.end();
    if (found != textures.end() && found->second.width > 0) {
        const Image &image = found->second;
        int x = std::min(image.width - 1, std::max(0, static_cast<int>(std::floor(v.u * image.width))));
        int y = std::min(image.height - 1, std::max(0, static_cast<int>(std::floor(v.v * image.height))));
        if (image.alpha8) {
            tr = tg = tb = 0;
            ta = image.pixels[size_t(y) * image.width + x] / 255.f;
        } else {
            const uint8_t *p = &image.pixels[(size_t(y) * image.width + x) * 4];
            tr = p[0] / 255.f;
            tg = p[1] / 255.f;
            tb = p[2] / 255.f;
            ta = p[3] / 255.f;
        }
    }
    if (program == Program::Mask)
        return {cr, cg, cb, ca * ta};
    return {tr * cr, tg * cg, tb * cb, ta * ca * coverage(v)};
}

inline Vertex lerp3(const Vertex &a, const Vertex &b, const Vertex &c, float wa, float wb, float wc) {
    Vertex out = a;
    auto mixf = [&](float Vertex::*field) { out.*field = wa * a.*field + wb * b.*field + wc * c.*field; };
    mixf(&Vertex::x);
    mixf(&Vertex::y);
    mixf(&Vertex::u);
    mixf(&Vertex::v);
    mixf(&Vertex::localX);
    mixf(&Vertex::localY);
    return out;
}

// Draws every batch in order. `scale` is device pixels per canvas pixel.
inline void draw(Framebuffer &fb, const Batcher &batcher, const TextureTable &textures, float scale) {
    for (size_t k = 0; k < batcher.batchCount(); ++k) {
        const Batch &batch = batcher.batch(k);
        int clipX0 = 0, clipY0 = 0, clipX1 = fb.width, clipY1 = fb.height;
        if (batch.clipped) {
            DeviceRect s = scissorFor(batch.clip, scale, fb.height);
            clipX0 = s.x;
            clipX1 = s.x + s.w;
            clipY0 = fb.height - (s.y + s.h);
            clipY1 = fb.height - s.y;
        }
        for (size_t i = 0; i + 2 < batch.vertices.size(); i += 3) {
            Vertex a = batch.vertices[i], b = batch.vertices[i + 1], c = batch.vertices[i + 2];
            float ax = a.x * scale, ay = a.y * scale, bx = b.x * scale, by = b.y * scale,
                  cx = c.x * scale, cy = c.y * scale;
            float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
            if (area == 0)
                continue;
            if (area < 0) {
                std::swap(b, c);
                std::swap(bx, cx);
                std::swap(by, cy);
                area = -area;
            }
            int x0 = std::max(clipX0, static_cast<int>(std::floor(std::min({ax, bx, cx})))),
                x1 = std::min(clipX1, static_cast<int>(std::ceil(std::max({ax, bx, cx})))),
                y0 = std::max(clipY0, static_cast<int>(std::floor(std::min({ay, by, cy})))),
                y1 = std::min(clipY1, static_cast<int>(std::ceil(std::max({ay, by, cy}))));
            const float vx[3] = {ax, bx, cx}, vy[3] = {ay, by, cy};
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x) {
                    float px = x + 0.5f, py = y + 0.5f, w[3];
                    bool covered = true;
                    for (int e = 0; e < 3; ++e) {
                        // Evaluate each edge from its lexicographically first endpoint, so a
                        // neighbour sharing it gets exactly the negated value and a pixel on
                        // the edge belongs to one triangle, never both or neither.
                        int p = e, q = (e + 1) % 3;
                        bool reversed = vx[q] < vx[p] || (vx[q] == vx[p] && vy[q] < vy[p]);
                        if (reversed)
                            std::swap(p, q);
                        float value = (vx[q] - vx[p]) * (py - vy[p]) - (vy[q] - vy[p]) * (px - vx[p]);
                        w[e] = reversed ? -value : value;
                        float dy = vy[(e + 1) % 3] - vy[e], dx = vx[(e + 1) % 3] - vx[e];
                        bool owner = dy > 0 || (dy == 0 && dx < 0);
                        covered = covered && (w[e] > 0 || (w[e] == 0 && owner));
                    }
                    if (!covered)
                        continue;
                    // Edge e is opposite vertex (e + 2) % 3: weights follow the vertices.
                    Vertex v = lerp3(a, b, c, w[1] / area, w[2] / area, w[0] / area);
                    float color[4];
                    for (int i = 0; i < 4; ++i)
                        color[i] = (w[1] * a.color[i] + w[2] * b.color[i] + w[0] * c.color[i]) /
                                   (area * 255.f);
                    Fragment f = shade(batch.program, v, color, batch, textures);
                    float *dst = &fb.rgb[(size_t(y) * fb.width + x) * 3];
                    dst[0] = f.r * f.a + dst[0] * (1 - f.a);
                    dst[1] = f.g * f.a + dst[1] * (1 - f.a);
                    dst[2] = f.b * f.a + dst[2] * (1 - f.a);
                }
        }
    }
}

inline void writePpm(const std::string &path, const Framebuffer &fb) {
    FILE *file = std::fopen(path.c_str(), "wb");
    if (!file)
        throw std::runtime_error("Cannot write " + path);
    std::fprintf(file, "P6\n%d %d\n255\n", fb.width, fb.height);
    for (int y = 0; y < fb.height; ++y)
        for (int x = 0; x < fb.width; ++x)
            for (int c = 0; c < 3; ++c)
                std::fputc(fb.byte(x, y, c), file);
    std::fclose(file);
}
// Largest per-channel difference, or 255 when the file is missing or a different size.
inline int compareToPpm(const std::string &path, const Framebuffer &fb) {
    FILE *file = std::fopen(path.c_str(), "rb");
    if (!file)
        return 255;
    int w = 0, h = 0, maxval = 0;
    int read = std::fscanf(file, "P6 %d %d %d", &w, &h, &maxval);
    std::fgetc(file);
    int worst = 255;
    if (read == 3 && w == fb.width && h == fb.height && maxval == 255) {
        worst = 0;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                for (int c = 0; c < 3; ++c)
                    worst = std::max(worst, std::abs(std::fgetc(file) - fb.byte(x, y, c)));
    }
    std::fclose(file);
    return worst;
}
} // namespace soft
} // namespace nativetv
