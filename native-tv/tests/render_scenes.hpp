#pragma once
// Scenes shared by the software-golden tests (raster.cpp) and the GPU comparison
// (renderer_gl.cpp). Textures are flat colours so linear and nearest filtering agree.
#include "soft_raster.hpp"
#include "ui/scene.hpp"

namespace nativetv {
namespace scenes {
// Hands out ids itself, or forwards to a real store, and remembers every image either way.
class RecordingStore : public TextureStore, public BatchHost {
    TextureStore *inner_;
    TextureId next_ = 1;

  public:
    soft::TextureTable table;
    explicit RecordingStore(TextureStore *inner = nullptr) : inner_(inner) {}
    TextureId create(int width, int height, TextureFormat format, bool nearest = false) override {
        TextureId id = inner_ ? inner_->create(width, height, format, nearest) : next_++;
        soft::Image &image = table[id];
        image.width = width;
        image.height = height;
        image.alpha8 = format == TextureFormat::Alpha8;
        image.pixels.assign(size_t(width) * height * (image.alpha8 ? 1 : 4), 0);
        return id;
    }
    void update(TextureId id, int x, int y, int width, int height, const void *pixels) override {
        if (inner_)
            inner_->update(id, x, y, width, height, pixels);
        soft::Image &image = table[id];
        size_t channels = image.alpha8 ? 1 : 4;
        for (int row = 0; row < height; ++row)
            std::copy_n(static_cast<const uint8_t *>(pixels) + size_t(row) * width * channels,
                        size_t(width) * channels,
                        image.pixels.begin() + (size_t(y + row) * image.width + x) * channels);
    }
    void destroy(TextureId id) override {
        if (inner_)
            inner_->destroy(id);
        table.erase(id);
    }
    size_t uploadBudget() const override {
        return inner_ ? inner_->uploadBudget() : 1024 * 1024;
    }
    bool textureSize(TextureId id, int &width, int &height) const override {
        auto found = table.find(id);
        if (found == table.end())
            return false;
        width = found->second.width;
        height = found->second.height;
        return true;
    }
    bool textRun(RunHandle, TextRun &) const override {
        return false;
    }
};

// Text runs made of solid glyph boxes, enough to exercise the mask path.
class GlyphRuns : public TextRunSource {
    TextureId atlas_;
    std::vector<std::vector<GlyphQuad>> runs_;

  public:
    explicit GlyphRuns(TextureId atlas) : atlas_(atlas) {}
    RunHandle make(int glyphs, float size) {
        std::vector<GlyphQuad> quads;
        for (int i = 0; i < glyphs; ++i)
            quads.push_back({i * size * 0.6f, 0, size * 0.5f, size, 0, 0, 1, 1});
        runs_.push_back(quads);
        return static_cast<RunHandle>(runs_.size());
    }
    bool run(RunHandle handle, TextRun &out) const override {
        if (!handle || handle > runs_.size())
            return false;
        out.texture = atlas_;
        out.glyphs = runs_[handle - 1].data();
        out.count = static_cast<uint32_t>(runs_[handle - 1].size());
        return true;
    }
};

struct Textures {
    TextureId hero = 0, poster[6] = {}, appIcon[8] = {}, glyphs = 0, icons = 0;
};
inline TextureId flat(TextureStore &store, int size, uint8_t r, uint8_t g, uint8_t b) {
    TextureId id = store.create(size, size, TextureFormat::Rgba8, false);
    std::vector<uint8_t> pixels(size_t(size) * size * 4);
    for (size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i] = r;
        pixels[i + 1] = g;
        pixels[i + 2] = b;
        pixels[i + 3] = 255;
    }
    store.update(id, 0, 0, size, size, pixels.data());
    return id;
}
inline TextureId solidMask(TextureStore &store, int size) {
    TextureId id = store.create(size, size, TextureFormat::Alpha8, false);
    std::vector<uint8_t> pixels(size_t(size) * size, 255);
    store.update(id, 0, 0, size, size, pixels.data());
    return id;
}
inline Textures makeTextures(TextureStore &store) {
    Textures t;
    t.hero = flat(store, 4, 40, 60, 110);
    for (int i = 0; i < 6; ++i)
        t.poster[i] = flat(store, 4, uint8_t(60 + i * 25), uint8_t(120 - i * 10), uint8_t(90 + i * 8));
    for (int i = 0; i < 8; ++i)
        t.appIcon[i] = flat(store, 4, uint8_t(200 - i * 15), uint8_t(80 + i * 14), 160);
    t.glyphs = solidMask(store, 4);
    t.icons = solidMask(store, 4);
    return t;
}

// Answers the batcher from a RecordingStore and, optionally, a GlyphRuns.
struct SceneHost : BatchHost {
    const RecordingStore &store;
    const GlyphRuns *runs;
    explicit SceneHost(const RecordingStore &s, const GlyphRuns *r = nullptr) : store(s), runs(r) {}
    bool textureSize(TextureId id, int &width, int &height) const override {
        return store.textureSize(id, width, height);
    }
    bool textRun(RunHandle handle, TextRun &out) const override {
        return runs && runs->run(handle, out);
    }
};

const Color kCanvas = {0.047f, 0.043f, 0.055f, 1};
const Color kSurface = {0.125f, 0.122f, 0.149f, 1};
const Color kInk = {0.945f, 0.933f, 0.957f, 1};
const Color kAccent = {0.659f, 0.78f, 0.98f, 1};

// 128x80 canvases for the goldens.
inline void fillScene(Scene &scene) {
    scene.reset();
    scene.background = kCanvas;
    scene.shape({8.5f, 8.25f, 48, 28}, kAccent, 12);
    scene.shape({64, 10, 52, 20}, kInk, 10);
    Node &mixed = scene.shape({8, 44, 48, 28}, {0.659f, 0.78f, 0.98f, 0.8f});
    mixed.corners = {14, 0, 14, 0};
    Rect area = {70, 44, 40, 24};
    Node &faded = scene.shape(area, kInk, 8);
    faded.transform = Transform::scaleAbout(area, 0.5f, 0.5f, 0.75f);
    faded.opacity = 0.5f;
}
inline void ringScene(Scene &scene) {
    scene.reset();
    scene.background = kCanvas;
    Node &focused = scene.shape({8, 12, 48, 56}, kSurface, 12);
    focused.ring = {0, 3, kInk};
    Node &resting = scene.shape({72, 12, 48, 56}, kSurface, 12);
    resting.ring = {1, 0, {1, 1, 1, 0.5f}};
}
inline void shadowScene(Scene &scene) {
    scene.reset();
    scene.background = {0.23f, 0.23f, 0.27f, 1};
    Node &card = scene.shape({32, 18, 64, 36}, kSurface, 10);
    card.shadow = {0, 12, 24, 0, {0, 0, 0, 0.62f}};
}
inline void gradientScene(Scene &scene) {
    scene.reset();
    scene.background = kCanvas;
    Node &diagonal = scene.shape({6, 6, 56, 68}, kInk, 10);
    diagonal.gradient = Gradient::between(135, kAccent, {0.4f, 0.31f, 0.64f, 1});
    Node &vertical = scene.shape({70, 6, 52, 68}, {0, 0, 0, 1});
    vertical.gradient.stops = 3;
    vertical.gradient.angle = 180;
    vertical.gradient.color[0] = {0, 0, 0, 0};
    vertical.gradient.color[1] = kAccent;
    vertical.gradient.color[2] = {1, 1, 1, 1};
    vertical.gradient.at[0] = 0;
    vertical.gradient.at[1] = 0.5f;
    vertical.gradient.at[2] = 1;
}
inline void clipScene(Scene &scene) {
    scene.reset();
    scene.background = kCanvas;
    scene.pushClip({20, 10, 60, 50});
    scene.shape({10, 20, 100, 20}, kAccent, 8);
    scene.shape({30, 30, 20, 20}, kInk, 4);
    scene.shape({90, 60, 20, 10}, kInk);
    scene.popClip();
    scene.shape({90, 6, 30, 10}, kSurface, 3);
}

// Glyph and icon origins sit 0.3 canvas px off the grid: at the tests' 1/8 scale integer origins
// would put edges exactly on pixel centres, where GPUs differ on which triangle owns the pixel.
// A Home-shaped frame at 1920x1080: hero art, scrims, top bar, hero text, a row of posters with
// the focused one raised, and a row of app circles. 6 posters share 2 draws only if batching
// reorders around the focused card.
inline void homeScene(Scene &scene, const Textures &t, const RunHandle (&runs)[8]) {
    scene.reset();
    scene.background = kCanvas;
    scene.picture({0, 0, 1920, 1080}, t.hero).stretch = true;
    Node &up = scene.shape({0, 0, 1920, 1080}, kCanvas);
    up.gradient.stops = 4;
    up.gradient.angle = 0;
    const float alphas[4] = {0.97f, 0.78f, 0.3f, 0};
    const float at[4] = {0, 0.22f, 0.46f, 0.64f};
    for (int i = 0; i < 4; ++i) {
        up.gradient.color[i] = {0.047f, 0.043f, 0.055f, alphas[i]};
        up.gradient.at[i] = at[i];
    }
    Node &across = scene.shape({0, 0, 1920, 1080}, kCanvas);
    across.gradient = Gradient::between(90, {0.047f, 0.043f, 0.055f, 0.6f}, {0.047f, 0.043f, 0.055f, 0});
    across.gradient.at[1] = 0.58f;
    scene.shape({80, 68, 56, 56}, kSurface, 28);
    scene.textRun({96.3f, 80.3f, 0, 0}, runs[0], kInk);
    scene.shape({168, 60, 148, 72}, kInk, 36);
    scene.textRun({190.3f, 80.3f, 0, 0}, runs[1], {0.07f, 0.06f, 0.09f, 1});
    scene.textRun({338.3f, 80.3f, 0, 0}, runs[2], kInk);
    scene.shape({1656, 68, 56, 56}, {0.945f, 0.933f, 0.957f, 0.2f}, 28);
    scene.icon({1672.3f, 84.3f, 24, 24}, t.icons, {0, 0, 1, 1}, kInk);
    scene.textRun({1740.3f, 78.3f, 0, 0}, runs[3], kInk);
    scene.textRun({80.3f, 416.3f, 0, 0}, runs[4], kInk);
    scene.textRun({80.3f, 458.3f, 0, 0}, runs[5], kInk);
    scene.textRun({80.3f, 560.3f, 0, 0}, runs[6], kInk);
    for (int i = 0; i < 5; ++i)
        scene.shape({1700.f + i * 24, 430, i == 0 ? 36.f : 8.f, 8}, i == 0 ? kInk : kSurface, 4);
    int focused = 1;
    for (int pass = 0; pass < 2; ++pass)
        for (int i = 0; i < 6; ++i) {
            if ((i == focused) != (pass == 1))
                continue;
            Rect card = {80.f + i * 456, 700, 432, 243};
            Transform raise = pass ? Transform::scaleAbout(card, 0.5f, 0.5f, 1.1f) : Transform();
            scene.pushTransform(raise);
            Node &poster = scene.picture(card, t.poster[i], 12);
            if (pass) {
                poster.shadow = {0, 22, 56, 0, {0, 0, 0, 0.62f}};
                poster.ring = {0, 3, kInk};
            }
            scene.textRun({card.x + 16.3f, card.y + 190.3f, 0, 0}, runs[7], kInk);
            scene.popTransform();
        }
    for (int i = 0; i < 8; ++i) {
        Rect circle = {80.f + i * 128, 960, 96, 96};
        Node &base = scene.shape(circle, kSurface, 48);
        base.gradient = Gradient::between(135, kSurface, {0.17f, 0.17f, 0.2f, 1});
        scene.picture({circle.x + 24, circle.y + 24, 48, 48}, t.appIcon[i], 12);
    }
    Node &dim = scene.shape({0, 0, 1920, 1080}, {0, 0, 0, 0.66f});
    dim.opacity = 0;
}
} // namespace scenes
} // namespace nativetv
