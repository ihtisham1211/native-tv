#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace nativetv {
struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    bool contains(float px, float py) const {
        return px >= x && py >= y && px < x + w && py < y + h;
    }
};
struct Color {
    float r = 1, g = 1, b = 1, a = 1;
};

// SCENE-V2
// Everything marked SCENE-V2 is additive: the legacy node API keeps working until the views move
// over. Coordinates are canvas pixels (1920x1080, origin top-left, y down); the renderer scales
// the canvas to the drawable.

// Handles are 32-bit and 0 means "none", so a default Node draws nothing extra.
using TextureId = uint32_t;
using RunHandle = uint32_t;
using StringId = uint32_t;

enum class TextureFormat { Alpha8, Rgba8 };

// What the glyph, icon and image modules use to get pixels onto the GPU. Renderer implements it;
// tests can stub it. Rows are tightly packed (no alignment padding).
class TextureStore {
  public:
    virtual ~TextureStore() = default;
    virtual TextureId create(int width, int height, TextureFormat format, bool nearest = false) = 0;
    virtual void update(TextureId id, int x, int y, int width, int height, const void *pixels) = 0;
    virtual void destroy(TextureId id) = 0;
    // Bytes still allowed this frame under the 1 MB upload budget. Callers defer what does not
    // fit (an upload is never refused, so the budget is advisory and the counters show overruns).
    virtual size_t uploadBudget() const = 0;
};

// A laid-out string: one quad per glyph, positions relative to the run origin (the node rect's
// top-left), alignment and ellipsis already applied by the layout. All quads sample one Alpha8
// atlas and are tinted by the node colour.
struct GlyphQuad {
    float x, y, w, h;
    float u0, v0, u1, v1;
};
struct TextRun {
    TextureId texture = 0;
    const GlyphQuad *glyphs = nullptr;
    uint32_t count = 0;
};
class TextRunSource {
  public:
    virtual ~TextRunSource() = default;
    // The glyph array must stay valid until the end of the next draw().
    virtual bool run(RunHandle handle, TextRun &out) const = 0;
};

// Scale and translate only (no rotation): x' = x * sx + tx, y' = y * sy + ty.
struct Transform {
    float sx = 1, sy = 1, tx = 0, ty = 0;
    static Transform translate(float dx, float dy) {
        return {1, 1, dx, dy};
    }
    // Scale about the canvas point (px, py), then translate by (dx, dy).
    static Transform scaleAbout(float px, float py, float sx, float sy, float dx = 0, float dy = 0) {
        return {sx, sy, px - px * sx + dx, py - py * sy + dy};
    }
    // Pivot given as fractions of a rect: (.5, .5) is the centre, (0, .5) the left-centre.
    static Transform scaleAbout(const Rect &area, float fx, float fy, float scale) {
        return scaleAbout(area.x + fx * area.w, area.y + fy * area.h, scale, scale);
    }
    // This transform first, then `outer`.
    Transform then(const Transform &outer) const {
        return {sx * outer.sx, sy * outer.sy, tx * outer.sx + outer.tx, ty * outer.sy + outer.ty};
    }
    Rect apply(const Rect &r) const {
        return {r.x * sx + tx, r.y * sy + ty, r.w * sx, r.h * sy};
    }
    bool identity() const {
        return sx == 1 && sy == 1 && tx == 0 && ty == 0;
    }
};

struct Corners {
    float topLeft = 0, topRight = 0, bottomRight = 0, bottomLeft = 0;
    bool any() const {
        return topLeft > 0 || topRight > 0 || bottomRight > 0 || bottomLeft > 0;
    }
};

// Band drawn along the node's rounded edge: `outer` px outside it and `inner` px inside it
// (focus ring: outer 3; resting outline: inner 1).
struct Ring {
    float inner = 0, outer = 0;
    Color color = {0, 0, 0, 0};
};

// Soft shadow from the same distance field. `blur` is the CSS blur radius (the falloff reaches
// zero one `blur` outside the edge). Drawn first, under the node.
struct Shadow {
    float dx = 0, dy = 0, blur = 0, spread = 0;
    Color color = {0, 0, 0, 0};
};

// Linear gradient replacing Node::color as the fill. `angle` follows CSS: 0 points up, 90 right,
// 180 down. Stops are in 0..1 along the gradient line, ascending.
struct Gradient {
    static constexpr int kMaxStops = 4;
    int stops = 0; // 0 = no gradient
    float angle = 180;
    Color color[kMaxStops];
    float at[kMaxStops] = {0, 1, 1, 1};
    static Gradient between(float angle, Color from, Color to) {
        Gradient g;
        g.stops = 2;
        g.angle = angle;
        g.color[0] = from;
        g.color[1] = to;
        g.at[1] = 1;
        return g;
    }
};

struct Node {
    // Rectangle: rounded fill (+ ring, shadow, gradient). Image: legacy path in `value`, or
    // `texture` (v2). Mask: Alpha8 `texture` tinted by `color` (icons). Run: `run` glyphs tinted
    // by `color`. Text: legacy string in `value`. Scrim: legacy, kept for compatibility.
    enum class Kind { Rectangle, Text, Image, Scrim, Run, Mask };
    Kind kind = Kind::Rectangle;
    Rect rect;
    Color color;
    std::string value;
    int font = 28;
    int lineHeight = 0, weight = 400;
    enum class Align { Left, Center, Right };
    Align align = Align::Left;
    float radius = 0;
    bool contain = false;
    bool stretch = false;
    // A fill drawn under a picture (the next node) for the time it has none or is see-through:
    // Application::resolvePictures hides it once an opaque picture is on show, where it would
    // only tint the picture's edge and its fade.
    bool backing = false;
    // SCENE-V2
    Corners corners;            // overrides `radius` when any corner is set
    Gradient gradient;
    Ring ring;
    Shadow shadow;
    TextureId texture = 0;      // Image (v2) and Mask
    Rect uv = {0, 0, 1, 1};     // sub-rectangle of the texture (icon atlas cell)
    RunHandle run = 0;          // Run
    Transform transform;        // this node's own; applied before `group`
    Transform group;            // from Scene::pushTransform, filled in by the Scene
    float opacity = 1;          // multiplies every colour alpha of the node; applied with groupOpacity
    float groupOpacity = 1;     // from Scene::pushOpacity
    Rect clip;                  // canvas rect, after transforms; from Scene::pushClip
    bool clipped = false;
    Corners radii() const {
        return corners.any() ? corners : Corners{radius, radius, radius, radius};
    }
};
struct Hit {
    Rect rect;
    std::string id;
};

// Strings kept once and referred to by id, so a frame that repeats last frame's strings (hit ids,
// text-run keys) allocates nothing. Ids stay valid until clear().
class StringTable {
    std::vector<std::string> strings_ = {std::string()};
    std::vector<uint32_t> slots_ = std::vector<uint32_t>(256, 0);
    size_t find(const char *text, size_t length, uint32_t hash) const;

  public:
    StringId intern(const char *text, size_t length);
    StringId intern(const std::string &text) {
        return intern(text.data(), text.size());
    }
    const std::string &str(StringId id) const {
        return strings_[id < strings_.size() ? id : 0];
    }
    size_t size() const {
        return strings_.size() - 1;
    }
    void clear();
};

struct Scene {
    std::vector<Node> nodes;
    std::vector<Hit> hits;
    Color background = {0, 0, 0, 1};
    StringTable strings;
    // Legacy builders (views today).
    void rect(Rect area, Color color, float radius = 0);
    void text(Rect area, const std::string &value, int size = 28,
              Color color = {0.925f, 0.906f, 0.875f, 1}, int lineHeight = 0, int weight = 400,
              Node::Align align = Node::Align::Left);
    void image(Rect area, const std::string &path, float radius = 0, bool contain = false,
               Color tint = {});
    void scrim(Rect area);
    void button(Rect area, const std::string &label, const std::string &id, bool selected,
                bool light = false);

    // SCENE-V2
    // Builders. Each returns the new node so the caller can set ring, shadow, gradient,
    // corners, transform or opacity; the reference is valid only until the next node is added.
    // Nodes pick up the pushed group state below at the moment they are added.
    Node &shape(Rect area, Color fill, float radius = 0);
    Node &picture(Rect area, TextureId texture, float radius = 0, bool contain = false,
                  Color tint = {});
    Node &icon(Rect area, TextureId atlas, Rect uv, Color tint);
    Node &textRun(Rect origin, RunHandle run, Color color);

    // Group state: affects the nodes added until the matching pop. Transforms and opacities
    // compose; a clip intersects with the one below it. Clips are canvas rects, so push them
    // after any transform that applies to them.
    void pushTransform(const Transform &transform);
    void popTransform();
    void pushOpacity(float opacity);
    void popOpacity();
    void pushClip(Rect area);
    void popClip();

    // Start a frame: drops last frame's nodes and hits but keeps their storage. The string table
    // is cleared only when it has grown past a few thousand entries (changing clock text).
    void reset();
    StringId intern(const std::string &text) {
        return strings.intern(text);
    }

  private:
    Node &add(Node::Kind kind, Rect area);
    std::vector<Transform> transforms_;
    std::vector<float> opacities_;
    std::vector<Rect> clips_;
};
} // namespace nativetv
