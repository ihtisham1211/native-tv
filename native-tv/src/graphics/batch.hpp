#pragma once
#include "graphics/shaders.hpp"
#include "ui/scene.hpp"
#include <cstdint>
#include <vector>

namespace nativetv {
// Per-frame limits from the GLES2 budget for the B3 (design/tv NOTES section 6.2).
constexpr uint32_t kDrawBudget = 15;      // typical frame
constexpr uint32_t kDrawCap = 40;         // never exceed
constexpr uint32_t kFullScreenLayerBudget = 4;
constexpr uint32_t kVertexBudget = 8192;
constexpr size_t kUploadBudgetBytes = 1024 * 1024;
constexpr size_t kTextureBudgetBytes = 64 * 1024 * 1024;

// One vertex layout for every program; the programs read what they need (shaders.cpp).
struct Vertex {
    float x, y, u, v;               // canvas position, texture coordinate
    float localX, localY, halfW, halfH; // offset from the shape centre, shape half extent
    float radii[4];                 // top-left, top-right, bottom-right, bottom-left
    float outer, inner, soft, slot; // see kCoverage in shaders.cpp; slot indexes Batch::textures
    uint8_t color[4];
};
static_assert(sizeof(Vertex) == 68, "attribute offsets in Renderer assume a tight vertex");

// Triangles that share a program, a clip and up to kTextureSlots textures: one draw call.
struct Batch {
    Program program = Program::Sdf;
    bool clipped = false;
    Rect clip;
    int textureCount = 0;
    TextureId textures[kTextureSlots] = {};
    std::vector<Vertex> vertices;
    Rect bounds;               // union of members
    std::vector<Rect> members; // one per primitive, for reorder tests
};

// What the batcher needs from its owner to size and expand nodes.
class BatchHost {
  public:
    virtual ~BatchHost() = default;
    virtual bool textureSize(TextureId id, int &width, int &height) const = 0;
    virtual bool textRun(RunHandle handle, TextRun &out) const = 0;
};

struct BatchStats {
    uint32_t nodes = 0;            // nodes that produced geometry
    uint32_t culled = 0;           // nodes skipped (off-screen, transparent, unresolved)
    uint32_t fullScreenLayers = 0; // primitives covering most of the canvas
};

// Turns scene nodes into batches. A primitive joins the latest batch it can reach without
// crossing a batch that draws over the same pixels, so a row of cards (fill, poster, title,
// fill, poster, title, ...) costs one draw per kind instead of one per node. Order only ever
// changes between primitives that do not overlap, so the picture is the same as painting in
// scene order. Storage is reused between frames.
class Batcher {
    const BatchHost &host_;
    std::vector<Batch> batches_;
    size_t count_ = 0;
    float width_ = 1920, height_ = 1080, scale_ = 1;
    BatchStats stats_;
    size_t primitives_ = 0;
    struct Shape {
        Rect box; // transformed box the distance field is built on
        float centerX, centerY, halfW, halfH;
        float radii[4];
    };
    struct Style {
        float alpha, scale, scaleX, scaleY;
        bool clipped;
        Rect clip;
    };
    Batch &place(Program program, const Rect &bounds, bool clipped, const Rect &clip,
                 TextureId texture, float &slot);
    bool admit(const Rect &bounds, const Style &style, bool layer, bool &clipped, Rect &clip);
    Shape shapeOf(const Rect &box, const Corners &radii, const Style &style) const;
    void emitSdf(const Style &style, const Shape &shape, const Rect &area, float outer,
                 float inner, float soft, const Color &color, bool layer);
    void emitGradient(const Style &style, const Shape &shape, const Gradient &gradient);
    void emitImage(const Style &style, const Shape &shape, const Rect &uvArea, const Rect &uv,
                   TextureId texture, const Color &tint);
    void emitMask(const Style &style, const Rect &area, const Rect &uv, TextureId texture,
                  const Color &tint);
    void emitRun(const Node &node, const Transform &transform, const Style &style);
    void emitShadow(const Node &node, const Style &style, const Shape &shape);
    void emitRing(const Node &node, const Style &style, const Shape &shape);
    float softness() const {
        return 0.5f / scale_;
    }

  public:
    explicit Batcher(const BatchHost &host) : host_(host) {}
    // Canvas size in canvas px; scale is device px per canvas px (edges stay one device pixel
    // wide whatever the drawable size).
    void begin(float width, float height, float scale);
    // texture overrides node.texture (legacy nodes, whose texture the renderer resolved).
    void add(const Node &node, TextureId texture);
    size_t batchCount() const {
        return count_;
    }
    const Batch &batch(size_t index) const {
        return batches_[index];
    }
    size_t vertexCount() const;
    const BatchStats &stats() const {
        return stats_;
    }
};

// Canvas clip to GL scissor (bottom-left origin, device px), rounded outward so a clip never
// shaves a pixel the shape covers.
struct DeviceRect {
    int x, y, w, h;
};
DeviceRect scissorFor(const Rect &clip, float scale, int deviceHeight);
} // namespace nativetv
