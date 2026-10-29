#include "graphics/renderer.hpp"
#include "graphics/debug.hpp"
#include "graphics/shaders.hpp"
#include "graphics/svg.hpp"
#include "graphics/text.hpp"
#include "qrcodegen.hpp"
#include <SDL_image.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace nativetv {
namespace {
constexpr float kCanvasWidth = 1920, kCanvasHeight = 1080;
constexpr int kMaxTextureSide = 4096;

GLuint compile(GLenum type, const std::string &source) {
    GLuint shader = glCreateShader(type);
    const char *text = source.c_str();
    glShaderSource(shader, 1, &text, nullptr);
    glCompileShader(shader);
    GLint ok;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        glDeleteShader(shader);
        throw std::runtime_error(std::string("OpenGL shader compile: ") + log);
    }
    return shader;
}
GLuint link(const std::string &vertex, const std::string &fragment) {
    GLuint vs = compile(GL_VERTEX_SHADER, vertex), fs = compile(GL_FRAGMENT_SHADER, fragment);
    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glBindAttribLocation(program, 0, "a_pos");
    glBindAttribLocation(program, 1, "a_shape");
    glBindAttribLocation(program, 2, "a_radii");
    glBindAttribLocation(program, 3, "a_band");
    glBindAttribLocation(program, 4, "a_color");
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        glDeleteProgram(program);
        throw std::runtime_error(std::string("OpenGL shader link: ") + log);
    }
    return program;
}
size_t bytesPerPixel(TextureFormat format) {
    return format == TextureFormat::Alpha8 ? 1 : 4;
}
double millisecondsSince(Uint64 start) {
    return 1000.0 * static_cast<double>(SDL_GetPerformanceCounter() - start) /
           static_cast<double>(SDL_GetPerformanceFrequency());
}
} // namespace

Renderer::Renderer(int width, int height, const std::string &font, const std::string &assets,
                   std::function<bool(const std::string &)> requestImage)
    : width_(width), height_(height), fontPath_(font), assetDirectory_(assets),
      requestImage_(requestImage), batcher_(*this) {
    fonts_[56] = TTF_OpenFont(fontPath_.c_str(), 28);
    if (!fonts_[56])
        throw std::runtime_error(std::string("Font could not be opened: ") + TTF_GetError());
    // Every variant is compiled here, behind the boot cover, never on the first frame that needs it.
    for (int i = 0; i < kProgramCount; ++i) {
        program(static_cast<Program>(i), false, 0);
        program(static_cast<Program>(i), true, 0);
    }
    glGenBuffers(1, &buffer_);
    white_ = create(1, 1, TextureFormat::Rgba8);
    const unsigned char pixel[] = {255, 255, 255, 255};
    update(white_, 0, 0, 1, 1, pixel);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    current_ = RenderCounters();
}
Renderer::~Renderer() {
    for (auto &texture : pool_)
        if (texture.gl)
            glDeleteTextures(1, &texture.gl);
    for (auto &pair : fonts_)
        TTF_CloseFont(pair.second);
    glDeleteBuffers(1, &buffer_);
    for (auto &built : programs_)
        glDeleteProgram(built.second);
}

GLuint Renderer::program(Program kind, bool singleTexture, unsigned debugVariant) {
    bool textured = kind == Program::Image || kind == Program::FlatImage || kind == Program::Mask;
    unsigned variant = debugVariant | (singleTexture && textured ? kSingleTexture : 0);
    GLuint &built = programs_[(variant << 4) | static_cast<unsigned>(kind)];
    if (built)
        return built;
    built = link(vertexSource(), fragmentSource(kind, variant));
    glUseProgram(built);
    for (int unit = 0; unit < kTextureSlots; ++unit) {
        GLint sampler = glGetUniformLocation(built, ("u_tex" + std::to_string(unit)).c_str());
        if (sampler >= 0)
            glUniform1i(sampler, unit);
    }
    glUniform4f(glGetUniformLocation(built, "u_proj"), 2 / kCanvasWidth, -2 / kCanvasHeight, -1, 1);
    return built;
}

TextureId Renderer::create(int width, int height, TextureFormat format, bool nearest) {
    if (width <= 0 || height <= 0 || width > kMaxTextureSide || height > kMaxTextureSide)
        return 0;
    Texture texture;
    texture.width = width;
    texture.height = height;
    texture.format = format;
    texture.bytes = static_cast<size_t>(width) * height * bytesPerPixel(format);
    glGenTextures(1, &texture.gl);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture.gl);
    GLenum layout = format == TextureFormat::Alpha8 ? GL_ALPHA : GL_RGBA;
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, layout, width, height, 0, layout, GL_UNSIGNED_BYTE, nullptr);
    GLint filter = nearest ? GL_NEAREST : GL_LINEAR;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    bytes_ += texture.bytes;
    TextureId id;
    if (!freeIds_.empty()) {
        id = freeIds_.back();
        freeIds_.pop_back();
        pool_[id - 1] = texture;
    } else {
        pool_.push_back(texture);
        id = static_cast<TextureId>(pool_.size());
    }
    return id;
}
void Renderer::update(TextureId id, int x, int y, int width, int height, const void *pixels) {
    if (!id || id > pool_.size() || !pool_[id - 1].gl || !pixels)
        return;
    const Texture &texture = pool_[id - 1];
    if (x < 0 || y < 0 || width <= 0 || height <= 0 || x + width > texture.width ||
        y + height > texture.height)
        return;
    GLenum layout = texture.format == TextureFormat::Alpha8 ? GL_ALPHA : GL_RGBA;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture.gl);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, width, height, layout, GL_UNSIGNED_BYTE, pixels);
    size_t sent = static_cast<size_t>(width) * height * bytesPerPixel(texture.format);
    ++current_.uploads;
    current_.uploadBytes += sent;
    current_.textureUploadBytes += sent;
}
void Renderer::destroy(TextureId id) {
    if (!id || id > pool_.size() || !pool_[id - 1].gl)
        return;
    Texture &texture = pool_[id - 1];
    glDeleteTextures(1, &texture.gl);
    bytes_ -= texture.bytes;
    texture = Texture();
    freeIds_.push_back(id);
}
bool Renderer::textureSize(TextureId id, int &width, int &height) const {
    if (!id || id > pool_.size() || !pool_[id - 1].gl)
        return false;
    width = pool_[id - 1].width;
    height = pool_[id - 1].height;
    return true;
}
bool Renderer::textRun(RunHandle handle, TextRun &out) const {
    return runs_ && runs_->run(handle, out);
}

void Renderer::trim(size_t incoming) {
    while (!textures_.empty() &&
           (bytes_ + incoming > kTextureBudgetBytes || textures_.size() >= 384)) {
        auto oldest = textures_.end();
        for (auto it = textures_.begin(); it != textures_.end(); ++it)
            if (it->second.used != frame_ &&
                (oldest == textures_.end() || it->second.used < oldest->second.used))
                oldest = it;
        if (oldest == textures_.end())
            break;
        destroy(oldest->second.id);
        requested_.erase(oldest->first);
        textures_.erase(oldest);
    }
}
TextureId Renderer::upload(SDL_Surface *source, bool nearest) {
    if (!source)
        return 0;
    if (source->w > kMaxTextureSide || source->h > kMaxTextureSide || source->w <= 0 ||
        source->h <= 0) {
        SDL_FreeSurface(source);
        return 0;
    }
    SDL_Surface *rgba =
        SDL_CreateRGBSurface(0, source->w, source->h, 32, 0xff, 0xff00, 0xff0000, 0xff000000);
    if (!rgba) {
        SDL_FreeSurface(source);
        return 0;
    }
    SDL_SetSurfaceBlendMode(source, SDL_BLENDMODE_NONE);
    SDL_BlitSurface(source, nullptr, rgba, nullptr);
    SDL_FreeSurface(source);
    size_t size = static_cast<size_t>(rgba->w) * rgba->h * 4;
    trim(size);
    TextureId id = bytes_ + size > kTextureBudgetBytes
                       ? 0
                       : create(rgba->w, rgba->h, TextureFormat::Rgba8, nearest);
    if (id)
        update(id, 0, 0, rgba->w, rgba->h, rgba->pixels);
    SDL_FreeSurface(rgba);
    return id;
}
bool Renderer::image(const std::string &path, const std::string &bytes) {
    if (bytes.empty() || bytes.size() > 8 * 1024 * 1024)
        return false;
    SDL_RWops *stream = SDL_RWFromConstMem(bytes.data(), static_cast<int>(bytes.size()));
    if (!stream)
        return false;
    TextureId id = upload(IMG_Load_RW(stream, 1));
    if (!id)
        return false;
    auto found = textures_.find(path);
    if (found != textures_.end())
        destroy(found->second.id);
    textures_[path] = {id, frame_};
    requested_.erase(path);
    return true;
}
void Renderer::imageFailed(const std::string &path) {
    requested_[path] = SDL_GetTicks() + 30000;
}

TextureId Renderer::legacyText(const Node &node) {
    std::string value = node.value.substr(0, 12000);
    int width = static_cast<int>(node.rect.w);
    int fontKey = node.font * 2 + (node.weight >= 500 ? 1 : 0);
    std::string key = "text:" + std::to_string(fontKey) + ":" + std::to_string(width) + ":" +
                      std::to_string(static_cast<int>(node.rect.h)) + ":" +
                      std::to_string(node.lineHeight) + ":" +
                      std::to_string(static_cast<int>(node.align)) + ":" + value;
    auto found = textures_.find(key);
    if (found != textures_.end()) {
        found->second.used = frame_;
        return found->second.id;
    }
    TTF_Font *&font = fonts_[fontKey];
    if (!font) {
        std::string path = fontPath_;
        size_t regular = path.find("Roboto-Regular.woff");
        if (node.weight >= 500 && regular != std::string::npos)
            path.replace(regular, 19, "Roboto-Medium.woff");
        font = TTF_OpenFont(path.c_str(), node.font);
    }
    if (!font)
        return 0;
    TextureId id = upload(rasterText(font, node));
    if (id)
        textures_[key] = {id, frame_};
    return id;
}
TextureId Renderer::legacyImage(const Node &node) {
    auto found = textures_.find(node.value);
    if (found == textures_.end() && node.value.compare(0, 6, "local:") == 0) {
        std::string relative = node.value.substr(6);
        if (relative.find("..") != std::string::npos || relative.empty() || relative[0] == '/')
            return 0;
        SvgPixels pixels = rasterSvg(assetDirectory_ + "/" + relative, 2,
                                     relative.compare(0, 6, "icons/") == 0);
        if (pixels.rgba.empty())
            return 0;
        SDL_Surface *surface =
            SDL_CreateRGBSurfaceFrom(pixels.rgba.data(), pixels.width, pixels.height, 32,
                                     pixels.width * 4, 0xff, 0xff00, 0xff0000, 0xff000000);
        TextureId id = upload(surface);
        if (id)
            textures_[node.value] = {id, frame_};
        found = textures_.find(node.value);
    }
    if (found == textures_.end() && node.value.compare(0, 3, "qr:") == 0) {
        try {
            auto qr = qrcodegen::QrCode::encodeText(node.value.substr(3).c_str(),
                                                    qrcodegen::QrCode::Ecc::MEDIUM);
            int size = qr.getSize() + 8;
            SDL_Surface *surface =
                SDL_CreateRGBSurface(0, size, size, 32, 0xff, 0xff00, 0xff0000, 0xff000000);
            if (surface) {
                SDL_FillRect(surface, nullptr, 0xffffffff);
                for (int y = 0; y < qr.getSize(); ++y)
                    for (int x = 0; x < qr.getSize(); ++x)
                        if (qr.getModule(x, y)) {
                            SDL_Rect pixel = {x + 4, y + 4, 1, 1};
                            SDL_FillRect(surface, &pixel, 0xff000000);
                        }
                TextureId id = upload(surface, true);
                if (id)
                    textures_[node.value] = {id, frame_};
            }
            found = textures_.find(node.value);
        } catch (const std::exception &) {
            return 0;
        }
    }
    if (found == textures_.end()) {
        auto pending = requested_.find(node.value);
        if (pending == requested_.end() ||
            (pending->second && static_cast<Sint32>(SDL_GetTicks() - pending->second) >= 0)) {
            requested_[node.value] = 0;
            if (!requestImage_(node.value))
                imageFailed(node.value);
        }
        return 0;
    }
    found->second.used = frame_;
    return found->second.id;
}

void Renderer::draw(const Scene &scene) {
    ++frame_;
    Uint64 start = SDL_GetPerformanceCounter();
    batcher_.begin(kCanvasWidth, kCanvasHeight, static_cast<float>(width_) / kCanvasWidth);
    for (const auto &node : scene.nodes) {
        TextureId texture = node.texture;
        if (node.kind == Node::Kind::Text)
            texture = legacyText(node);
        else if (node.kind == Node::Kind::Image && !texture && !node.value.empty())
            texture = legacyImage(node);
        batcher_.add(node, texture);
    }
    current_.batchMs = millisecondsSince(start);
    Uint64 submitStart = SDL_GetPerformanceCounter();

    const DebugSkip &debug = debugSkip();
    const float viewport = debug.viewport;
    const unsigned variant = debug.shaderVariant() & ~kSingleTexture;
    if (debug.noBlend)
        glDisable(GL_BLEND);
    else
        glEnable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, static_cast<GLsizei>(width_ * viewport), static_cast<GLsizei>(height_ * viewport));
    glClearColor(scene.background.r, scene.background.g, scene.background.b, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    Uint64 traceClear = debug.trace ? SDL_GetPerformanceCounter() : 0, traceBuffer = 0,
           traceWorst = 0;
    size_t traceWorstBatch = 0;
    size_t batches = batcher_.batchCount();
    if (batches) {
        staging_.clear();
        for (size_t i = 0; i < batches; ++i) {
            const auto &vertices = batcher_.batch(i).vertices;
            staging_.insert(staging_.end(), vertices.begin(), vertices.end());
        }
        // Orphaning the buffer lets the driver keep drawing from last frame's copy.
        glBindBuffer(GL_ARRAY_BUFFER, buffer_);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(staging_.size() * sizeof(Vertex)),
                     staging_.data(), GL_STREAM_DRAW);
        ++current_.uploads;
        current_.uploadBytes += staging_.size() * sizeof(Vertex);
        if (debug.trace)
            traceBuffer = SDL_GetPerformanceCounter();
        const GLsizei stride = sizeof(Vertex);
        const size_t offsets[5] = {0, 16, 32, 48, 64};
        for (GLuint attribute = 0; attribute < 5; ++attribute) {
            glEnableVertexAttribArray(attribute);
            glVertexAttribPointer(attribute, 4, attribute == 4 ? GL_UNSIGNED_BYTE : GL_FLOAT,
                                  attribute == 4 ? GL_TRUE : GL_FALSE, stride,
                                  reinterpret_cast<const void *>(offsets[attribute]));
        }
        GLuint boundTextures[kTextureSlots];
        glActiveTexture(GL_TEXTURE0);
        GLuint white = pool_[white_ - 1].gl;
        for (int unit = kTextureSlots - 1; unit >= 0; --unit) {
            glActiveTexture(GL_TEXTURE0 + unit);
            glBindTexture(GL_TEXTURE_2D, white);
            boundTextures[unit] = white;
        }
        GLuint activeProgram = 0;
        bool scissorOn = false;
        DeviceRect scissor = {0, 0, 0, 0};
        GLint first = 0;
        for (size_t i = 0; i < batches; ++i) {
            const Batch &batch = batcher_.batch(i);
            GLuint program = this->program(batch.program, batch.textureCount <= 1, variant);
            if (program != activeProgram) {
                glUseProgram(program);
                activeProgram = program;
            }
            for (int slot = 0; slot < batch.textureCount; ++slot) {
                TextureId id = batch.textures[slot];
                GLuint gl = id && id <= pool_.size() && pool_[id - 1].gl ? pool_[id - 1].gl : white;
                if (boundTextures[slot] != gl) {
                    glActiveTexture(GL_TEXTURE0 + slot);
                    glBindTexture(GL_TEXTURE_2D, gl);
                    boundTextures[slot] = gl;
                    ++current_.textureBinds;
                }
            }
            if (batch.clipped) {
                DeviceRect rect = scissorFor(
                    batch.clip, viewport * static_cast<float>(width_) / kCanvasWidth,
                    static_cast<int>(height_ * viewport));
                if (!scissorOn || rect.x != scissor.x || rect.y != scissor.y ||
                    rect.w != scissor.w || rect.h != scissor.h) {
                    if (!scissorOn)
                        glEnable(GL_SCISSOR_TEST);
                    glScissor(rect.x, rect.y, rect.w, rect.h);
                    scissor = rect;
                    scissorOn = true;
                    ++current_.scissorChanges;
                }
            } else if (scissorOn) {
                glDisable(GL_SCISSOR_TEST);
                scissorOn = false;
                ++current_.scissorChanges;
            }
            GLsizei count = static_cast<GLsizei>(batch.vertices.size());
            Uint64 drawStart = 0;
            if (batchTimes_) {
                glFinish();
                drawStart = SDL_GetPerformanceCounter();
            }
            Uint64 traceDraw = debug.trace ? SDL_GetPerformanceCounter() : 0;
            glDrawArrays(GL_TRIANGLES, first, count);
            if (debug.trace) {
                Uint64 took = SDL_GetPerformanceCounter() - traceDraw;
                if (took > traceWorst) {
                    traceWorst = took;
                    traceWorstBatch = i;
                }
            }
            if (batchTimes_) {
                glFinish();
                (*batchTimes_)[i] += millisecondsSince(drawStart);
            }
            first += count;
            ++current_.draws;
            current_.vertices += static_cast<uint32_t>(count);
        }
        if (scissorOn)
            glDisable(GL_SCISSOR_TEST);
    }
    if (debug.finish)
        glFinish();
    trim();
#ifndef NDEBUG
    GLenum error = glGetError();
    if (error != GL_NO_ERROR)
        throw std::runtime_error("OpenGL draw failed: " + std::to_string(error));
#endif
    current_.nodes = batcher_.stats().nodes;
    current_.culled = batcher_.stats().culled;
    current_.fullScreenLayers = batcher_.stats().fullScreenLayers;
    current_.textureBytes = bytes_;
    current_.submitMs = millisecondsSince(submitStart);
    if (debug.trace && current_.submitMs > 8) {
        const double tick = 1000.0 / static_cast<double>(SDL_GetPerformanceFrequency());
        std::fprintf(stderr,
                     "slow submit %.1f ms: clear %.1f, buffer %.1f, worst draw %.1f (batch %zu of %zu), binds etc. %.1f\n",
                     current_.submitMs, static_cast<double>(traceClear - submitStart) * tick,
                     traceBuffer ? static_cast<double>(traceBuffer - traceClear) * tick : 0.0,
                     static_cast<double>(traceWorst) * tick, traceWorstBatch, batcher_.batchCount(),
                     current_.submitMs - static_cast<double>(traceClear - submitStart) * tick -
                         (traceBuffer ? static_cast<double>(traceBuffer - traceClear) * tick : 0.0) -
                         static_cast<double>(traceWorst) * tick);
    }
    last_ = current_;
    current_ = RenderCounters();
}

double Renderer::timeFrames(const Scene &scene, int frames, bool perBatch) {
    const bool finishBefore = debugSkip().finish;
    debugSkip().finish = true;
    draw(scene);
    draw(scene);
    std::vector<double> times;
    if (perBatch)
        times.assign(batcher_.batchCount(), 0.0);
    batchTimes_ = perBatch ? &times : nullptr;
    Uint64 start = SDL_GetPerformanceCounter();
    for (int i = 0; i < frames; ++i)
        draw(scene);
    double total = millisecondsSince(start) / frames;
    batchTimes_ = nullptr;
    debugSkip().finish = finishBefore;
    for (size_t i = 0; i < times.size(); ++i) {
        const Batch &batch = batcher_.batch(i);
        double area = 0; // canvas px covered by the triangles, before clipping
        for (size_t v = 0; v + 2 < batch.vertices.size(); v += 3) {
            const Vertex &a = batch.vertices[v], &b = batch.vertices[v + 1], &c = batch.vertices[v + 2];
            area += std::fabs((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x)) / 2;
        }
        std::fprintf(stderr,
                     "  batch %2zu program=%d verts=%4zu tex=%d clip=%d area=%7.0f bounds=%.0f,%.0f %.0fx%.0f ms=%.2f\n",
                     i, static_cast<int>(batch.program), batch.vertices.size(), batch.textureCount,
                     batch.clipped ? 1 : 0, area, batch.bounds.x, batch.bounds.y, batch.bounds.w,
                     batch.bounds.h, times[i] / frames);
    }
    return total;
}

void Renderer::acquireBackBuffer() {
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}

void Renderer::capture(const std::string &path) const {
    std::vector<unsigned char> pixels(static_cast<size_t>(width_) * height_ * 4);
    glFinish();
    glReadPixels(0, 0, width_, height_, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    FILE *file = std::fopen(path.c_str(), "wb");
    if (!file)
        throw std::runtime_error("Could not save capture");
    std::fprintf(file, "P6\n%d %d\n255\n", width_, height_);
    std::vector<unsigned char> row(static_cast<size_t>(width_) * 3);
    for (int y = height_ - 1; y >= 0; --y) {
        for (int x = 0; x < width_; ++x) {
            size_t source = (static_cast<size_t>(y) * width_ + x) * 4;
            std::copy_n(pixels.data() + source, 3, row.data() + x * 3);
        }
        if (std::fwrite(row.data(), 1, row.size(), file) != row.size()) {
            std::fclose(file);
            throw std::runtime_error("Could not write capture");
        }
    }
    std::fclose(file);
}
} // namespace nativetv
