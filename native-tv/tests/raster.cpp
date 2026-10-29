#include "graphics/svg.hpp"
#include "graphics/text.hpp"
#include "render_scenes.hpp"
#include <cstdio>
#include <cstdlib>
#include <new>
#include <memory>
#include <stdexcept>

using namespace nativetv;
namespace {
size_t allocations = 0;
} // namespace
// Counts heap allocations so the test can show a repeated frame allocates nothing.
void *operator new(size_t size) {
    ++allocations;
    if (void *memory = std::malloc(size ? size : 1))
        return memory;
    throw std::bad_alloc();
}
void operator delete(void *memory) noexcept {
    std::free(memory);
}
void operator delete(void *memory, size_t) noexcept {
    std::free(memory);
}
namespace {
int checks = 0;
void check(bool condition, const char *message) {
    ++checks;
    if (!condition)
        throw std::runtime_error(message);
}

struct Rgb {
    int r, g, b;
};
Rgb at(const soft::Framebuffer &fb, int x, int y) {
    return {fb.byte(x, y, 0), fb.byte(x, y, 1), fb.byte(x, y, 2)};
}
Rgb rgb(Color c) {
    return {int(c.r * 255 + 0.5f), int(c.g * 255 + 0.5f), int(c.b * 255 + 0.5f)};
}
Rgb over(Color top, float alpha, Color under) {
    return rgb({top.r * alpha + under.r * (1 - alpha), top.g * alpha + under.g * (1 - alpha),
                top.b * alpha + under.b * (1 - alpha), 1});
}
bool near(Rgb a, Rgb b, int tolerance) {
    return std::abs(a.r - b.r) <= tolerance && std::abs(a.g - b.g) <= tolerance &&
           std::abs(a.b - b.b) <= tolerance;
}

scenes::RecordingStore noTextures;
soft::Framebuffer render(const Scene &scene, Batcher &batcher, int width, int height, float scale,
                         const soft::TextureTable &textures) {
    batcher.begin(width / scale, height / scale, scale);
    for (const auto &node : scene.nodes)
        batcher.add(node, node.texture);
    soft::Framebuffer fb(width, height, scene.background);
    soft::draw(fb, batcher, textures, scale);
    return fb;
}
// The goldens are this rasteriser's output; NATIVETV_UPDATE_GOLDEN=1 rewrites them after a
// deliberate look change. The numeric checks beside each call keep a rewrite honest.
void golden(const char *name, const soft::Framebuffer &fb) {
    std::string path = std::string(NATIVETV_FIXTURES) + "/raster/" + name + ".ppm";
    if (std::getenv("NATIVETV_UPDATE_GOLDEN"))
        soft::writePpm(path, fb);
    check(soft::compareToPpm(path, fb) <= 1, (std::string("Golden matches: ") + name).c_str());
}

void rendererChecks() {
    Batcher batcher(noTextures);
    Scene scene;
    const Color bg = scenes::kCanvas;

    scenes::fillScene(scene);
    soft::Framebuffer fill = render(scene, batcher, 128, 80, 1, {});
    check(near(at(fill, 30, 22), rgb(scenes::kAccent), 1), "SDF fill: interior is the fill colour");
    check(near(at(fill, 8, 8), rgb(bg), 1), "SDF fill: outside the rounded corner is untouched");
    check(near(at(fill, 8, 22), over(scenes::kAccent, 0.5f, bg), 3),
          "SDF fill: an edge on a pixel centre is half covered");
    check(near(at(fill, 28, 58), over(scenes::kAccent, 0.8f, bg), 2) &&
              near(at(fill, 9, 45), rgb(bg), 3),
          "SDF fill: per-corner radii round only the chosen corners");
    check(near(at(fill, 90, 56), over(scenes::kInk, 0.5f, bg), 2) &&
              near(at(fill, 73, 56), rgb(bg), 1),
          "SDF fill: opacity multiplies and a scale about the centre shrinks the shape");
    golden("sdf-fill", fill);

    scenes::ringScene(scene);
    soft::Framebuffer ring = render(scene, batcher, 128, 80, 1, {});
    check(near(at(ring, 5, 40), rgb(scenes::kInk), 1) && near(at(ring, 6, 40), rgb(scenes::kInk), 1),
          "Ring: the band outside the edge is the ring colour");
    check(near(at(ring, 4, 40), rgb(bg), 1) && near(at(ring, 20, 40), rgb(scenes::kSurface), 1),
          "Ring: nothing beyond the outer width, fill untouched inside");
    check(near(at(ring, 72, 40), over({1, 1, 1, 1}, 0.5f, scenes::kSurface), 2) &&
              near(at(ring, 73, 40), rgb(scenes::kSurface), 1),
          "Ring: an inner outline stays inside the edge");
    golden("ring", ring);

    scenes::shadowScene(scene);
    soft::Framebuffer shadow = render(scene, batcher, 128, 80, 1, {});
    Rgb below = at(shadow, 64, 62), above = at(shadow, 64, 12), far = at(shadow, 2, 2);
    check(below.r < above.r && above.r < far.r + 1 && near(far, rgb(scene.background), 1),
          "Shadow: offset down, darker below the card than above, none far away");
    check(at(shadow, 64, 60).r < at(shadow, 64, 70).r && at(shadow, 64, 70).r < at(shadow, 64, 76).r + 1,
          "Shadow: falls off with distance");
    golden("shadow", shadow);

    scenes::gradientScene(scene);
    soft::Framebuffer gradient = render(scene, batcher, 128, 80, 1, {});
    check(near(at(gradient, 96, 40), rgb(scenes::kAccent), 6),
          "Gradient: the middle stop of a three-stop gradient is reached");
    check(at(gradient, 96, 72).r > 230 && at(gradient, 96, 8).r < 60,
          "Gradient: ends take the first and last stop, with alpha");
    check(at(gradient, 14, 14).b > at(gradient, 54, 66).b || at(gradient, 14, 14).r > at(gradient, 54, 66).r,
          "Gradient: a 135 degree gradient runs from top-left to bottom-right");
    golden("gradient", gradient);

    scenes::clipScene(scene);
    soft::Framebuffer clip = render(scene, batcher, 128, 80, 1, {});
    check(near(at(clip, 40, 45), rgb(scenes::kInk), 1) && near(at(clip, 15, 30), rgb(bg), 1) &&
              near(at(clip, 95, 30), rgb(bg), 1),
          "Clip: a shape is cut at the clip rectangle");
    check(near(at(clip, 25, 30), rgb(scenes::kAccent), 1) && near(at(clip, 100, 65), rgb(bg), 1),
          "Clip: a shape inside shows, one outside is dropped");
    check(batcher.stats().culled == 1 && batcher.batchCount() == 2,
          "Clip: the dropped shape costs nothing and the clipped ones form one draw");
    golden("clip", clip);

    // Node state: group transform, opacity and clip compose as nodes are added.
    scene.reset();
    scene.pushTransform(Transform::scaleAbout(0, 0, 2, 2));
    scene.pushOpacity(0.5f);
    scene.pushOpacity(0.5f);
    Node &grouped = scene.shape({10, 10, 20, 20}, {1, 1, 1, 1});
    scene.popOpacity();
    scene.popOpacity();
    scene.popTransform();
    batcher.begin(128, 80, 1);
    batcher.add(grouped, 0);
    const Batch &first = batcher.batch(0);
    float minX = 1e9f, maxX = -1e9f;
    for (const auto &v : first.vertices) {
        minX = std::min(minX, v.x);
        maxX = std::max(maxX, v.x);
    }
    check(std::fabs((minX + maxX) / 2 - 40) < 0.01f && first.vertices[0].color[3] == 64,
          "Scene groups: transforms scale about the pivot and opacities multiply");
    scene.pushOpacity(0.0f);
    scene.shape({0, 0, 50, 50}, {1, 1, 1, 1});
    scene.popOpacity();
    batcher.begin(128, 80, 1);
    batcher.add(scene.nodes.back(), 0);
    check(batcher.batchCount() == 0 && batcher.stats().culled == 1, "Transparent nodes are culled");
    DeviceRect rect = scissorFor({10.2f, 20.7f, 30, 5}, 1, 100);
    check(rect.x == 10 && rect.y == 74 && rect.w == 31 && rect.h == 6,
          "Scissor rounds outward and flips to GL's bottom-left origin");

    // Image fit: cover crops the texture, contain shrinks the quad.
    scenes::RecordingStore store;
    TextureId square = store.create(100, 100, TextureFormat::Rgba8), tall = store.create(100, 200, TextureFormat::Rgba8);
    Batcher imageBatcher(store);
    scene.reset();
    scene.picture({0, 0, 200, 100}, square);
    scene.picture({0, 0, 200, 100}, tall, 0, true);
    imageBatcher.begin(256, 128, 1);
    imageBatcher.add(scene.nodes[0], scene.nodes[0].texture);
    float lowV = 1e9f, highV = -1e9f;
    for (const auto &v : imageBatcher.batch(0).vertices) {
        lowV = std::min(lowV, v.v);
        highV = std::max(highV, v.v);
    }
    check(std::fabs(lowV - 0.245f) < 1e-3f && std::fabs(highV - 0.755f) < 1e-3f,
          "Cover fit crops the long side of the texture around its centre");
    imageBatcher.begin(256, 128, 1);
    imageBatcher.add(scene.nodes[1], scene.nodes[1].texture);
    minX = 1e9f;
    maxX = -1e9f;
    for (const auto &v : imageBatcher.batch(0).vertices) {
        minX = std::min(minX, v.x);
        maxX = std::max(maxX, v.x);
    }
    check(std::fabs(minX - 74) < 0.01f && std::fabs(maxX - 126) < 0.01f,
          "Contain fit letterboxes the quad to the texture's aspect");

    // Batching. Overlapping primitives keep their order; disjoint ones regroup.
    scene.reset();
    scene.shape({0, 0, 40, 40}, {1, 0, 0, 1});
    scene.picture({20, 20, 40, 40}, square);
    scene.shape({40, 40, 40, 40}, {0, 1, 0, 1});
    imageBatcher.begin(256, 128, 1);
    for (const auto &node : scene.nodes)
        imageBatcher.add(node, node.texture);
    check(imageBatcher.batchCount() == 3, "Overlapping fill, image, fill stay in painter's order");
    scene.reset();
    scene.shape({0, 0, 40, 40}, {1, 0, 0, 1});
    scene.picture({100, 20, 40, 40}, square);
    scene.shape({50, 0, 40, 40}, {0, 1, 0, 1});
    imageBatcher.begin(256, 128, 1);
    for (const auto &node : scene.nodes)
        imageBatcher.add(node, node.texture);
    check(imageBatcher.batchCount() == 2, "Disjoint fills share a draw across an image");

    // One draw samples up to four textures; a fifth starts another.
    scene.reset();
    for (int i = 0; i < 5; ++i)
        scene.picture({i * 50.f, 0, 40, 40}, i == 0 ? square : store.create(8, 8, TextureFormat::Rgba8));
    imageBatcher.begin(256, 128, 1);
    for (const auto &node : scene.nodes)
        imageBatcher.add(node, node.texture);
    check(imageBatcher.batchCount() == 2 && imageBatcher.batch(0).textureCount == kTextureSlots,
          "A draw holds four textures, the fifth opens a second draw");

    // A Home-shaped frame: counts, and the same picture as painting node by node.
    scenes::RecordingStore homeStore;
    scenes::Textures textures = scenes::makeTextures(homeStore);
    scenes::GlyphRuns glyphs(textures.glyphs);
    RunHandle runs[8] = {glyphs.make(1, 28), glyphs.make(4, 31), glyphs.make(4, 31),
                         glyphs.make(5, 30), glyphs.make(28, 24), glyphs.make(12, 72),
                         glyphs.make(40, 28), glyphs.make(14, 26)};
    scenes::SceneHost host(homeStore, &glyphs);
    Batcher home(host);
    scenes::homeScene(scene, textures, runs);
    const float eighth = 0.125f;
    home.begin(1920, 1080, eighth);
    for (const auto &node : scene.nodes)
        home.add(node, node.texture);
    size_t draws = home.batchCount();
    check(draws >= 5 && draws <= kDrawBudget,
          ("A typical Home frame batches into at most 15 draws (got " + std::to_string(draws) + ")").c_str());
    check(home.vertexCount() <= kVertexBudget && home.stats().fullScreenLayers <= kFullScreenLayerBudget,
          "A typical Home frame stays inside the vertex and full-screen layer budgets");
        check(home.stats().culled >= 1 && home.stats().nodes + home.stats().culled == scene.nodes.size(),
          "Invisible and off-screen nodes are skipped, the rest all draw");
    soft::Framebuffer batched(240, 135, scene.background), painted(240, 135, scene.background);
    soft::draw(batched, home, homeStore.table, eighth);
    Batcher single(host);
    for (const auto &node : scene.nodes) {
        single.begin(1920, 1080, eighth);
        single.add(node, node.texture);
        soft::draw(painted, single, homeStore.table, eighth);
    }
    float worst = 0;
    for (size_t i = 0; i < batched.rgb.size(); ++i)
        worst = std::max(worst, std::fabs(batched.rgb[i] - painted.rgb[i]));
    check(worst < 1e-5f, "Batched draws produce the same picture as painting in scene order");
    golden("home", batched);

    // Steady state: rebuilding and re-batching the same frame allocates nothing.
    size_t before = allocations;
    scenes::homeScene(scene, textures, runs);
    home.begin(1920, 1080, eighth);
    for (const auto &node : scene.nodes)
        home.add(node, node.texture);
    check(allocations == before, "A repeated frame reuses the node, group and batch storage");
    const int rounds = 200;
    Uint64 start = SDL_GetPerformanceCounter();
    for (int i = 0; i < rounds; ++i) {
        scenes::homeScene(scene, textures, runs);
        home.begin(1920, 1080, eighth);
        for (const auto &node : scene.nodes)
            home.add(node, node.texture);
    }
    std::printf("home frame: %zu nodes, %zu draws, %zu vertices, build+batch %.3f ms\n",
                scene.nodes.size(), home.batchCount(), home.vertexCount(),
                1000.0 * double(SDL_GetPerformanceCounter() - start) /
                    double(SDL_GetPerformanceFrequency()) / rounds);

    // Interned strings: same text, same id, stored once; ids survive growth.
    StringTable table;
    StringId menu = table.intern("menu:12");
    check(menu && table.intern(std::string("menu:12")) == menu && table.str(menu) == "menu:12" &&
              table.intern("") == 0,
          "Interning the same text returns the same id");
    std::vector<StringId> ids;
    for (int i = 0; i < 3000; ++i)
        ids.push_back(table.intern("clock " + std::to_string(i)));
    bool stable = table.size() == 3001;
    for (int i = 0; i < 3000; ++i)
        stable = stable && table.intern("clock " + std::to_string(i)) == ids[i] &&
                 table.str(ids[i]) == "clock " + std::to_string(i);
    check(stable && table.intern("menu:12") == menu, "Interned ids stay valid as the table grows");
    allocations = 0;
    for (int i = 0; i < 100; ++i)
        table.intern("clock 7");
    check(allocations == 0, "Looking up a known string allocates nothing");
    Scene churn;
    for (int i = 0; i < 5000; ++i)
        churn.intern("tick " + std::to_string(i));
    churn.reset();
    check(churn.strings.size() == 0, "A frame that interns unbounded strings starts fresh");
}
} // namespace
int main() {
    try {
        if (TTF_Init())
            throw std::runtime_error(TTF_GetError());
        std::unique_ptr<TTF_Font, decltype(&TTF_CloseFont)> font(
            TTF_OpenFont(NATIVETV_ASSETS "/Roboto-Regular.woff", 24), TTF_CloseFont);
        check(font != nullptr, "The shipped WOFF font opens on the native rasterizer");
        Node label;
        label.rect = {0, 0, 180, 55};
        label.font = 24;
        label.lineHeight = 32;
        label.value = "A long Café label that must fit one complete line";
        std::unique_ptr<SDL_Surface, decltype(&SDL_FreeSurface)> surface(
            rasterText(font.get(), label), SDL_FreeSurface);
        check(surface && surface->w == 180 && surface->h == 32,
              "Overflow is ellipsized without exposing a clipped second line");
        label.rect = {0, 0, 4, 128};
        label.value = "éééé";
        surface.reset(rasterText(font.get(), label));
        check(surface != nullptr, "Narrow text does not split UTF-8 code points");
        for (const auto *key : {"shift", "space", "left", "right", "delete", "done"}) {
            auto icon =
                rasterSvg(std::string(NATIVETV_ASSETS) + "/icons/key-" + key + ".svg", 2, true);
            bool visible = false;
            for (size_t i = 3; i < icon.rgba.size(); i += 4)
                visible = visible || icon.rgba[i] > 0;
            check(visible, "Every exported keyboard SVG produces visible pixels");
        }
        auto scrim = rasterSvg(NATIVETV_ASSETS "/carousel-scrim.svg", 2);
        check(scrim.width == 1516 && scrim.height > 850,
              "The original carousel gradient retains its source geometry");
        check(scrim.rgba[3] > scrim.rgba[(scrim.width - 1) * 4 + 3],
              "The gradient fades artwork toward the left content area");
        rendererChecks();
        std::printf("%d native raster checks passed\n", checks);
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        TTF_Quit();
        return 1;
    }
    TTF_Quit();
    return 0;
}
