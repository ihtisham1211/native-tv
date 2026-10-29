#include "graphics/batch.hpp"
#include <algorithm>
#include <cmath>

namespace nativetv {
namespace {
// Inner width of a fill: far enough that the inner edge never reaches the shape.
constexpr float kFar = 8192;
constexpr float kInvisible = 1.0f / 512;
constexpr float kPi = 3.14159265f;

uint8_t byteOf(float value) {
    return static_cast<uint8_t>(std::min(1.f, std::max(0.f, value)) * 255 + 0.5f);
}
bool intersects(const Rect &a, const Rect &b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}
Rect unite(const Rect &a, const Rect &b) {
    float x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y), x1 = std::max(a.x + a.w, b.x + b.w),
          y1 = std::max(a.y + a.h, b.y + b.h);
    return {x0, y0, x1 - x0, y1 - y0};
}
Rect grow(const Rect &r, float margin) {
    return {r.x - margin, r.y - margin, r.w + 2 * margin, r.h + 2 * margin};
}
Rect intersection(const Rect &a, const Rect &b) {
    float x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y), x1 = std::min(a.x + a.w, b.x + b.w),
          y1 = std::min(a.y + a.h, b.y + b.h);
    return {x0, y0, std::max(0.f, x1 - x0), std::max(0.f, y1 - y0)};
}
bool inside(const Rect &inner, const Rect &outer) {
    return inner.x >= outer.x && inner.y >= outer.y && inner.x + inner.w <= outer.x + outer.w &&
           inner.y + inner.h <= outer.y + outer.h;
}
bool sameRect(const Rect &a, const Rect &b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}
void setColor(Vertex &v, const Color &c, float alpha) {
    v.color[0] = byteOf(c.r);
    v.color[1] = byteOf(c.g);
    v.color[2] = byteOf(c.b);
    v.color[3] = byteOf(c.a * alpha);
}
Color mix(const Color &a, const Color &b, float f) {
    return {a.r + (b.r - a.r) * f, a.g + (b.g - a.g) * f, a.b + (b.b - a.b) * f,
            a.a + (b.a - a.a) * f};
}

struct Point {
    float x, y, t;
};
// Sutherland-Hodgman against one half-plane of the gradient parameter.
int clipByT(const Point *in, int count, Point *out, float limit, bool keepAbove) {
    int n = 0;
    for (int i = 0; i < count; ++i) {
        const Point &a = in[i], &b = in[(i + 1) % count];
        bool aIn = keepAbove ? a.t >= limit : a.t <= limit,
             bIn = keepAbove ? b.t >= limit : b.t <= limit;
        if (aIn)
            out[n++] = a;
        if (aIn != bIn) {
            float f = (limit - a.t) / (b.t - a.t);
            out[n++] = {a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f, limit};
        }
    }
    return n;
}
} // namespace

DeviceRect scissorFor(const Rect &clip, float scale, int deviceHeight) {
    int x0 = std::max(0, static_cast<int>(std::floor(clip.x * scale)));
    int y0 = std::max(0, static_cast<int>(std::floor(clip.y * scale)));
    int x1 = std::max(x0, static_cast<int>(std::ceil((clip.x + clip.w) * scale)));
    int y1 = std::min(deviceHeight, std::max(y0, static_cast<int>(std::ceil((clip.y + clip.h) * scale))));
    return {x0, deviceHeight - y1, x1 - x0, y1 - y0};
}

void Batcher::begin(float width, float height, float scale) {
    width_ = width;
    height_ = height;
    scale_ = scale;
    count_ = 0;
    primitives_ = 0;
    stats_ = BatchStats();
}
size_t Batcher::vertexCount() const {
    size_t total = 0;
    for (size_t i = 0; i < count_; ++i)
        total += batches_[i].vertices.size();
    return total;
}

Batch &Batcher::place(Program program, const Rect &bounds, bool clipped, const Rect &clip,
                      TextureId texture, float &slot) {
    ++primitives_;
    slot = 0;
    Batch *target = nullptr;
    int targetSlot = 0;
    // Walk back from the newest batch to the first one that draws over the same pixels (nothing
    // may move past it) and join the earliest compatible batch on the way: a row of cards then
    // gathers its fills, pictures and text in one draw each instead of one set per card.
    for (size_t k = count_; k-- > 0;) {
        Batch &candidate = batches_[k];
        if (candidate.program == program && candidate.clipped == clipped &&
            (!clipped || sameRect(candidate.clip, clip))) {
            int found = -1;
            if (!texture)
                found = 0;
            for (int i = 0; i < candidate.textureCount && found < 0; ++i)
                if (candidate.textures[i] == texture)
                    found = i;
            if (found < 0 && candidate.textureCount < kTextureSlots)
                found = candidate.textureCount;
            if (found >= 0) {
                target = &candidate;
                targetSlot = found;
            }
        }
        if (intersects(candidate.bounds, bounds)) {
            bool overlaps = false;
            for (const Rect &member : candidate.members)
                if (intersects(member, bounds)) {
                    overlaps = true;
                    break;
                }
            if (overlaps)
                break;
        }
    }
    if (target) {
        if (texture && targetSlot == target->textureCount)
            target->textures[target->textureCount++] = texture;
        slot = static_cast<float>(targetSlot);
    }
    if (!target) {
        if (count_ == batches_.size())
            batches_.emplace_back();
        target = &batches_[count_++];
        target->program = program;
        target->clipped = clipped;
        target->clip = clip;
        target->vertices.clear();
        target->members.clear();
        target->bounds = bounds;
        target->textureCount = 0;
        if (texture) {
            target->textures[0] = texture;
            target->textureCount = 1;
        }
    }
    target->bounds = target->members.empty() ? bounds : unite(target->bounds, bounds);
    target->members.push_back(bounds);
    return *target;
}

// Decides whether a primitive is drawn and whether it needs a scissor. A clip that fully
// contains the primitive is dropped so it does not split the batch.
bool Batcher::admit(const Rect &bounds, const Style &style, bool layer, bool &clipped, Rect &clip) {
    Rect canvas = {0, 0, width_, height_};
    if (bounds.w <= 0 || bounds.h <= 0 || !intersects(bounds, canvas))
        return false;
    clipped = false;
    if (style.clipped) {
        if (!intersects(bounds, style.clip))
            return false;
        if (!inside(bounds, style.clip)) {
            clipped = true;
            clip = style.clip;
        }
    }
    if (layer) {
        Rect visible = intersection(bounds, clipped ? clip : canvas);
        if (visible.w * visible.h >= 0.8f * width_ * height_)
            ++stats_.fullScreenLayers;
    }
    return true;
}

Batcher::Shape Batcher::shapeOf(const Rect &box, const Corners &radii, const Style &style) const {
    Shape shape;
    shape.box = box;
    shape.centerX = box.x + box.w / 2;
    shape.centerY = box.y + box.h / 2;
    shape.halfW = box.w / 2;
    shape.halfH = box.h / 2;
    float limit = std::min(shape.halfW, shape.halfH), values[4] = {radii.topLeft, radii.topRight,
                                                                   radii.bottomRight, radii.bottomLeft};
    for (int i = 0; i < 4; ++i)
        shape.radii[i] = std::min(limit, values[i] * style.scale);
    return shape;
}

static void putVertex(Batch &batch, float x, float y, float u, float v, const float *shape,
                      const float *radii, float outer, float inner, float soft, float slot,
                      const Color &color, float alpha) {
    Vertex vertex;
    vertex.x = x;
    vertex.y = y;
    vertex.u = u;
    vertex.v = v;
    vertex.localX = shape ? x - shape[0] : 0;
    vertex.localY = shape ? y - shape[1] : 0;
    vertex.halfW = shape ? shape[2] : 0;
    vertex.halfH = shape ? shape[3] : 0;
    for (int i = 0; i < 4; ++i)
        vertex.radii[i] = radii ? radii[i] : 0;
    vertex.outer = outer;
    vertex.inner = inner;
    vertex.soft = soft;
    vertex.slot = slot;
    setColor(vertex, color, alpha);
    batch.vertices.push_back(vertex);
}
static void putQuad(Batch &batch, const Rect &area, const Rect &uvArea, const Rect &uv,
                    const float *shape, const float *radii, float outer, float inner, float soft,
                    float slot, const Color &color, float alpha) {
    float x0 = area.x, y0 = area.y, x1 = area.x + area.w, y1 = area.y + area.h;
    auto u = [&](float x) { return uv.x + (x - uvArea.x) / uvArea.w * uv.w; };
    auto v = [&](float y) { return uv.y + (y - uvArea.y) / uvArea.h * uv.h; };
    const float xs[6] = {x0, x1, x0, x1, x1, x0}, ys[6] = {y0, y0, y1, y0, y1, y1};
    for (int i = 0; i < 6; ++i)
        putVertex(batch, xs[i], ys[i], u(xs[i]), v(ys[i]), shape, radii, outer, inner, soft, slot,
                  color, alpha);
}

void Batcher::emitSdf(const Style &style, const Shape &shape, const Rect &area, float outer,
                      float inner, float soft, const Color &color, bool layer) {
    bool clipped;
    Rect clip;
    if (!admit(area, style, layer, clipped, clip))
        return;
    float slot;
    Batch &batch = place(Program::Sdf, area, clipped, clip, 0, slot);
    const float geometry[4] = {shape.centerX, shape.centerY, shape.halfW, shape.halfH};
    putQuad(batch, area, area, {0, 0, 1, 1}, geometry, shape.radii, outer, inner, soft, 0, color,
            style.alpha);
}

void Batcher::emitShadow(const Node &node, const Style &style, const Shape &shape) {
    const Shadow &s = node.shadow;
    if (s.color.a * style.alpha < kInvisible)
        return;
    Shape moved = shape;
    float spread = s.spread * style.scale, blur = s.blur * style.scale;
    moved.centerX += s.dx * style.scaleX;
    moved.centerY += s.dy * style.scaleY;
    moved.halfW = std::max(0.f, shape.halfW + spread);
    moved.halfH = std::max(0.f, shape.halfH + spread);
    float limit = std::min(moved.halfW, moved.halfH);
    for (float &r : moved.radii)
        r = std::min(limit, std::max(0.f, r + spread));
    moved.box = {moved.centerX - moved.halfW, moved.centerY - moved.halfH, 2 * moved.halfW,
                 2 * moved.halfH};
    emitSdf(style, moved, grow(moved.box, blur + 2 * softness()), 0, kFar,
            std::max(blur, softness()), s.color, false);
}

void Batcher::emitRing(const Node &node, const Style &style, const Shape &shape) {
    const Ring &ring = node.ring;
    if (ring.color.a * style.alpha < kInvisible || (ring.inner <= 0 && ring.outer <= 0))
        return;
    float outer = ring.outer * style.scale, inner = ring.inner * style.scale;
    emitSdf(style, shape, grow(shape.box, outer + 2 * softness()), outer, inner, softness(),
            ring.color, false);
}

void Batcher::emitGradient(const Style &style, const Shape &shape, const Gradient &gradient) {
    float pad = 2 * softness();
    Rect area = grow(shape.box, pad);
    bool clipped;
    Rect clip;
    if (!admit(area, style, true, clipped, clip))
        return;
    float angle = gradient.angle * kPi / 180, dx = std::sin(angle), dy = -std::cos(angle),
          length = std::fabs(shape.box.w * dx) + std::fabs(shape.box.h * dy);
    if (length < 1e-3f)
        length = 1;
    auto parameter = [&](float x, float y) {
        return ((x - shape.centerX) * dx + (y - shape.centerY) * dy) / length + 0.5f;
    };
    int last = gradient.stops - 1;
    auto colorAt = [&](float t) {
        if (t <= gradient.at[0])
            return gradient.color[0];
        for (int i = 1; i <= last; ++i)
            if (t <= gradient.at[i] || i == last) {
                float span = gradient.at[i] - gradient.at[i - 1];
                float f = span > 0 ? std::min(1.f, (t - gradient.at[i - 1]) / span) : 1;
                return mix(gradient.color[i - 1], gradient.color[i], f);
            }
        return gradient.color[last];
    };
    const float xs[4] = {area.x, area.x + area.w, area.x + area.w, area.x},
                ys[4] = {area.y, area.y, area.y + area.h, area.y + area.h};
    Point corners[4];
    float lowest = 1e9f, highest = -1e9f;
    for (int i = 0; i < 4; ++i) {
        corners[i] = {xs[i], ys[i], parameter(xs[i], ys[i])};
        lowest = std::min(lowest, corners[i].t);
        highest = std::max(highest, corners[i].t);
    }
    // Breakpoints are the stops that fall inside the quad; between two of them the colour is
    // linear in t, so vertex colours reproduce it exactly. Stops within the antialiasing margin
    // of the edge are ignored.
    float margin = pad / length, cuts[Gradient::kMaxStops + 2];
    int cutCount = 0;
    cuts[cutCount++] = -1e9f;
    for (int i = 0; i <= last; ++i)
        if (gradient.at[i] > lowest + margin && gradient.at[i] < highest - margin)
            cuts[cutCount++] = gradient.at[i];
    cuts[cutCount++] = 1e9f;
    float slot;
    Batch &batch = place(Program::Sdf, area, clipped, clip, 0, slot);
    const float geometry[4] = {shape.centerX, shape.centerY, shape.halfW, shape.halfH};
    for (int region = 0; region + 1 < cutCount; ++region) {
        Point lower[8], upper[8];
        int n = clipByT(corners, 4, lower, cuts[region], true);
        if (n >= 3)
            n = clipByT(lower, n, upper, cuts[region + 1], false);
        if (n < 3)
            continue;
        for (int i = 1; i + 1 < n; ++i)
            for (const Point *p : {&upper[0], &upper[i], &upper[i + 1]})
                putVertex(batch, p->x, p->y, 0, 0, geometry, shape.radii, 0, kFar, softness(), 0,
                          colorAt(p->t), style.alpha);
    }
}

void Batcher::emitImage(const Style &style, const Shape &shape, const Rect &uvArea,
                        const Rect &uv, TextureId texture, const Color &tint) {
    Rect area = grow(shape.box, 2 * softness());
    bool clipped;
    Rect clip;
    if (!admit(area, style, true, clipped, clip))
        return;
    float slot;
    Batch &batch = place(Program::Image, area, clipped, clip, texture, slot);
    const float geometry[4] = {shape.centerX, shape.centerY, shape.halfW, shape.halfH};
    putQuad(batch, area, uvArea, uv, geometry, shape.radii, 0, kFar, softness(), slot, tint,
            style.alpha);
}

void Batcher::emitMask(const Style &style, const Rect &area, const Rect &uv, TextureId texture,
                       const Color &tint) {
    bool clipped;
    Rect clip;
    if (!admit(area, style, false, clipped, clip))
        return;
    float slot;
    Batch &batch = place(Program::Mask, area, clipped, clip, texture, slot);
    putQuad(batch, area, area, uv, nullptr, nullptr, 0, 0, 0, slot, tint, style.alpha);
}

void Batcher::emitRun(const Node &node, const Transform &transform, const Style &style) {
    TextRun run;
    if (!node.run || !host_.textRun(node.run, run) || !run.texture || !run.count)
        return;
    Rect bounds;
    bool first = true;
    for (uint32_t i = 0; i < run.count; ++i) {
        const GlyphQuad &g = run.glyphs[i];
        Rect r = transform.apply({node.rect.x + g.x, node.rect.y + g.y, g.w, g.h});
        bounds = first ? r : unite(bounds, r);
        first = false;
    }
    bool clipped;
    Rect clip;
    if (!admit(bounds, style, false, clipped, clip))
        return;
    float slot;
    Batch &batch = place(Program::Mask, bounds, clipped, clip, run.texture, slot);
    for (uint32_t i = 0; i < run.count; ++i) {
        const GlyphQuad &g = run.glyphs[i];
        Rect r = transform.apply({node.rect.x + g.x, node.rect.y + g.y, g.w, g.h});
        if (r.w <= 0 || r.h <= 0 || r.x >= width_ || r.y >= height_ || r.x + r.w <= 0 || r.y + r.h <= 0)
            continue;
        putQuad(batch, r, {r.x, r.y, r.w, r.h}, {g.u0, g.v0, g.u1 - g.u0, g.v1 - g.v0}, nullptr,
                nullptr, 0, 0, 0, slot, node.color, style.alpha);
    }
}

void Batcher::add(const Node &node, TextureId texture) {
    Transform transform = node.transform.then(node.group);
    Style style;
    style.alpha = node.opacity * node.groupOpacity;
    style.scaleX = transform.sx;
    style.scaleY = transform.sy;
    style.scale = std::sqrt(std::fabs(transform.sx * transform.sy));
    style.clipped = node.clipped;
    style.clip = node.clip;
    size_t before = primitives_;
    if (style.alpha >= kInvisible) {
        Rect box = transform.apply(node.rect);
        Corners radii = node.radii();
        switch (node.kind) {
        case Node::Kind::Rectangle: {
            Shape shape = shapeOf(box, radii, style);
            emitShadow(node, style, shape);
            if (node.gradient.stops >= 2)
                emitGradient(style, shape, node.gradient);
            else if (node.color.a * style.alpha >= kInvisible)
                emitSdf(style, shape, grow(shape.box, 2 * softness()), 0, kFar, softness(),
                        node.color, true);
            emitRing(node, style, shape);
            break;
        }
        case Node::Kind::Image: {
            Shape outline = shapeOf(box, radii, style);
            emitShadow(node, style, outline);
            int width = 0, height = 0;
            if (texture && host_.textureSize(texture, width, height) && width > 0 && height > 0) {
                Rect content = box, uv = node.uv;
                float ratio = (width * uv.w) / (height * uv.h), target = box.w / box.h;
                if (node.contain) {
                    if (ratio > target) {
                        content.h = box.w / ratio;
                        content.y += (box.h - content.h) / 2;
                    } else {
                        content.w = box.h * ratio;
                        content.x += (box.w - content.w) / 2;
                    }
                } else if (!node.stretch) {
                    if (ratio > target) {
                        float kept = uv.w * target / ratio;
                        uv.x += (uv.w - kept) / 2;
                        uv.w = kept;
                    } else {
                        float kept = uv.h * ratio / target;
                        uv.y += (uv.h - kept) / 2;
                        uv.h = kept;
                    }
                }
                emitImage(style, shapeOf(content, radii, style), content, uv, texture, node.color);
            }
            emitRing(node, style, outline);
            break;
        }
        case Node::Kind::Text: {
            // Legacy string: the renderer rasterised it; the texture is the text's own size and
            // the node rect only clips it.
            int width = 0, height = 0;
            if (texture && host_.textureSize(texture, width, height)) {
                Rect content = {box.x, box.y, width * style.scaleX, height * style.scaleY};
                Style clipped = style;
                Rect limit = clipped.clipped ? intersection(box, style.clip) : box;
                clipped.clipped = true;
                clipped.clip = limit;
                emitImage(clipped, shapeOf(content, Corners(), style), content, {0, 0, 1, 1},
                          texture, node.color);
            }
            break;
        }
        case Node::Kind::Scrim: {
            // Legacy shade: black fading out to the right, and in toward the bottom.
            Shape shape = shapeOf(box, Corners(), style);
            Gradient across = Gradient::between(90, {0, 0, 0, 1}, {0, 0, 0, 0});
            across.at[1] = 0.78f;
            Gradient up = Gradient::between(180, {0, 0, 0, 0}, {0, 0, 0, 1});
            up.at[0] = 0.38f;
            emitGradient(style, shape, across);
            emitGradient(style, shape, up);
            break;
        }
        case Node::Kind::Run:
            emitRun(node, transform, style);
            break;
        case Node::Kind::Mask:
            if (texture)
                emitMask(style, box, node.uv, texture, node.color);
            break;
        }
    }
    if (primitives_ == before)
        ++stats_.culled;
    else
        ++stats_.nodes;
}
} // namespace nativetv
