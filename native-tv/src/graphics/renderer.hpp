#pragma once
#include "graphics/batch.hpp"
#include "ui/scene.hpp"
#include <SDL.h>
#include <SDL_ttf.h>
#ifdef __APPLE__
#include <SDL_opengl.h>
#else
#include <GLES2/gl2.h>
#endif
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace nativetv {
// What the last completed draw() cost. Texture uploads made between two draws are counted
// with the frame that follows them. The perf overlay and --perf log read this.
struct RenderCounters {
    uint32_t draws = 0;
    uint32_t vertices = 0;
    uint32_t nodes = 0;            // nodes that produced geometry
    uint32_t culled = 0;           // nodes skipped
    uint32_t fullScreenLayers = 0;
    uint32_t textureBinds = 0;
    uint32_t scissorChanges = 0;
    uint32_t uploads = 0;          // buffer and texture upload calls
    size_t uploadBytes = 0;        // vertex plus texture bytes sent to the GPU
    size_t textureUploadBytes = 0; // texture bytes only, the part kUploadBudgetBytes limits
    size_t textureBytes = 0;       // resident
    double batchMs = 0;            // scene to batches
    double submitMs = 0;           // batches to GL calls
    bool withinBudget() const {
        return draws <= kDrawBudget && fullScreenLayers <= kFullScreenLayerBudget &&
               vertices <= kVertexBudget && textureUploadBytes <= kUploadBudgetBytes &&
               textureBytes <= kTextureBudgetBytes;
    }
    bool overCap() const {
        return draws > kDrawCap;
    }
};

class Renderer : public TextureStore, private BatchHost {
    struct Texture {
        GLuint gl = 0;
        int width = 0, height = 0;
        TextureFormat format = TextureFormat::Rgba8;
        size_t bytes = 0;
    };
    struct Cached { // legacy path/string keyed textures, evicted least recently used
        TextureId id = 0;
        unsigned used = 0;
    };
    GLuint programs_[kProgramCount] = {}, buffer_ = 0, white_ = 0;
    int width_ = 0, height_ = 0;
    unsigned frame_ = 0;
    size_t bytes_ = 0;
    std::string fontPath_;
    std::string assetDirectory_;
    std::map<int, TTF_Font *> fonts_;
    std::vector<Texture> pool_; // index = id - 1
    std::vector<TextureId> freeIds_;
    std::map<std::string, Cached> textures_;
    std::map<std::string, unsigned> requested_;
    std::function<bool(const std::string &)> requestImage_;
    const TextRunSource *runs_ = nullptr;
    Batcher batcher_;
    std::vector<Vertex> staging_;
    RenderCounters current_, last_;
    TextureId upload(SDL_Surface *surface, bool nearest = false);
    void trim(size_t incoming = 0);
    TextureId legacyText(const Node &node);
    TextureId legacyImage(const Node &node);
    bool textureSize(TextureId id, int &width, int &height) const override;
    bool textRun(RunHandle handle, TextRun &out) const override;

  public:
    // width and height are the drawable's pixels; the scene's canvas is always 1920x1080.
    Renderer(int width, int height, const std::string &font, const std::string &assets,
             std::function<bool(const std::string &)> requestImage);
    ~Renderer();
    void draw(const Scene &scene);

    // TextureStore. Ids are renderer-wide; 0 is never issued.
    TextureId create(int width, int height, TextureFormat format, bool nearest = false) override;
    void update(TextureId id, int x, int y, int width, int height, const void *pixels) override;
    void destroy(TextureId id) override;
    size_t uploadBudget() const override {
        return current_.textureUploadBytes >= kUploadBudgetBytes
                   ? 0
                   : kUploadBudgetBytes - current_.textureUploadBytes;
    }
    void setTextRuns(const TextRunSource *source) {
        runs_ = source;
    }
    const RenderCounters &counters() const {
        return last_;
    }

    // Legacy: encoded bytes keyed by path, decoded on the calling thread.
    bool image(const std::string &path, const std::string &bytes);
    void imageFailed(const std::string &path);
    bool hasImage(const std::string &path) const {
        return textures_.find(path) != textures_.end();
    }
    void capture(const std::string &path) const;
    size_t textureBytes() const {
        return bytes_;
    }
};
} // namespace nativetv
