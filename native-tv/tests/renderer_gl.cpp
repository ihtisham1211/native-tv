// Draws the shared scenes through the real Renderer (GLSL, VBO, scissor) and compares the pixels
// with the CPU rasteriser's, so a shader or submission change the goldens cannot see still
// fails. Exits 77 (skipped) when the machine cannot make a GL context.
#include "graphics/renderer.hpp"
#include "render_scenes.hpp"
#include <SDL_image.h>
#include <cstdio>
#include <cstdlib>
#include <memory>

using namespace nativetv;
namespace {
constexpr int kSkipped = 77;
constexpr int kWindow = 256;
// Small scenes are authored on a 128x80 canvas; the renderer's canvas is 1920x1080, so they are
// magnified 15x and drawn into a 128x72 viewport, which lands on the same device pixels.
constexpr float kMagnify = 15;

void magnify(Scene &scene) {
    Transform up = Transform::scaleAbout(0, 0, kMagnify, kMagnify);
    for (auto &node : scene.nodes) {
        node.transform = node.transform.then(up);
        if (node.clipped)
            node.clip = {node.clip.x * kMagnify, node.clip.y * kMagnify, node.clip.w * kMagnify,
                         node.clip.h * kMagnify};
    }
}

struct Capture {
    int width = 0, height = 0;
    std::vector<uint8_t> rgb;
};
Capture readPpm(const std::string &path) {
    Capture capture;
    FILE *file = std::fopen(path.c_str(), "rb");
    if (!file)
        throw std::runtime_error("No capture written");
    int maxval;
    if (std::fscanf(file, "P6 %d %d %d", &capture.width, &capture.height, &maxval) != 3)
        throw std::runtime_error("Bad capture header");
    std::fgetc(file);
    capture.rgb.resize(size_t(capture.width) * capture.height * 3);
    if (std::fread(capture.rgb.data(), 1, capture.rgb.size(), file) != capture.rgb.size())
        throw std::runtime_error("Short capture");
    std::fclose(file);
    return capture;
}

int checks = 0;
// Differences allowed between the GPU and the CPU raster: 8-bit rounding and the GPU's own
// smoothstep and interpolation.
constexpr int kTolerance = 3;
void compare(const char *name, Renderer &renderer, scenes::RecordingStore &store,
             const scenes::GlyphRuns *runs, const Scene &scene, int width, int height) {
    ++checks;
    renderer.draw(scene);
    const std::string path = "native-renderer-gl-capture.ppm";
    renderer.capture(path);
    Capture gpu = readPpm(path);
    std::remove(path.c_str());

    scenes::SceneHost host(store, runs);
    Batcher batcher(host);
    float scale = float(width) / 1920;
    batcher.begin(1920, 1080, scale);
    for (const auto &node : scene.nodes)
        batcher.add(node, node.texture);
    soft::Framebuffer cpu(width, height, scene.background);
    soft::draw(cpu, batcher, store.table, scale);

    int worst = 0, outliers = 0;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            for (int c = 0; c < 3; ++c) {
                int diff = std::abs(int(gpu.rgb[(size_t(y) * width + x) * 3 + c]) - cpu.byte(x, y, c));
                worst = std::max(worst, diff);
                outliers += diff > kTolerance;
            }
    std::printf("%s: draws gpu=%u cpu=%zu worst=%d outliers=%d\n", name, renderer.counters().draws,
                batcher.batchCount(), worst, outliers);
    if (outliers)
        throw std::runtime_error(std::string("GPU and CPU rasters differ: ") + name);
    if (renderer.counters().draws != batcher.batchCount())
        throw std::runtime_error(std::string("Draw count differs from the batches: ") + name);
}
} // namespace

int main() {
    if (SDL_Init(SDL_INIT_VIDEO)) {
        std::printf("skipped: no video (%s)\n", SDL_GetError());
        return kSkipped;
    }
#ifdef __APPLE__
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#endif
    SDL_Window *window = SDL_CreateWindow("nativeTv renderer test", 0, 0, kWindow, kWindow,
                                          SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    SDL_GLContext context = window ? SDL_GL_CreateContext(window) : nullptr;
    if (!context) {
        std::printf("skipped: no GL context (%s)\n", SDL_GetError());
        if (window)
            SDL_DestroyWindow(window);
        SDL_Quit();
        return kSkipped;
    }
    int status = 0;
    try {
        if (TTF_Init())
            throw std::runtime_error(TTF_GetError());
        auto never = [](const std::string &) { return false; };
        std::string assets = NATIVETV_ASSETS;
        {
            Renderer renderer(128, 72, assets + "/Roboto-Regular.woff", assets, never);
            scenes::RecordingStore store(&renderer);
            Scene scene;
            using Builder = void (*)(Scene &);
            const struct {
                const char *name;
                Builder build;
            } small[] = {{"sdf-fill", scenes::fillScene}, {"ring", scenes::ringScene},
                         {"shadow", scenes::shadowScene}, {"gradient", scenes::gradientScene},
                         {"clip", scenes::clipScene}};
            for (const auto &entry : small) {
                entry.build(scene);
                magnify(scene);
                compare(entry.name, renderer, store, nullptr, scene, 128, 72);
            }
        }
        {
            Renderer renderer(240, 135, assets + "/Roboto-Regular.woff", assets, never);
            scenes::RecordingStore store(&renderer);
            scenes::Textures textures = scenes::makeTextures(store);
            scenes::GlyphRuns glyphs(textures.glyphs);
            renderer.setTextRuns(&glyphs);
            RunHandle runs[8] = {glyphs.make(1, 28), glyphs.make(4, 31), glyphs.make(4, 31),
                                 glyphs.make(5, 30), glyphs.make(28, 24), glyphs.make(12, 72),
                                 glyphs.make(40, 28), glyphs.make(14, 26)};
            Scene scene;
            scenes::homeScene(scene, textures, runs);
            compare("home", renderer, store, &glyphs, scene, 240, 135);
            const RenderCounters &counters = renderer.counters();
            std::printf("home: %u draws, %u vertices, %u full-screen layers, %zu B uploaded, batch %.3f ms, "
                        "submit %.3f ms\n", counters.draws, counters.vertices, counters.fullScreenLayers,
                        counters.uploadBytes, counters.batchMs, counters.submitMs);
            if (!counters.withinBudget() || counters.overCap())
                throw std::runtime_error("The Home frame is outside the render budget");
            ++checks;
        }
        std::printf("%d renderer GPU checks passed\n", checks);
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        status = 1;
    }
    TTF_Quit();
    SDL_GL_DeleteContext(context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return status;
}
