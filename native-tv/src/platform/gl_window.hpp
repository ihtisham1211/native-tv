#pragma once

#include <SDL.h>
#include <stdexcept>

namespace nativetv {

// Which step of bringing up the screen failed; `--probe` turns it into an exit code.
enum class WindowStage { Display, Context };

struct WindowError : std::runtime_error {
    WindowStage stage;
    WindowError(WindowStage failedStage, const char *message)
        : std::runtime_error(message), stage(failedStage) {}
};

// SDL's video system, a window and a GLES 2 context. The client and `--probe`
// both create it here, so the probe fails exactly when the client would.
class GlWindow {
    SDL_Window *window_ = nullptr;
    SDL_GLContext context_ = nullptr;
    int width_ = 0, height_ = 0;

  public:
    // Fullscreen 1920x1080 on the TV; windowed is 1280x720 for a desktop.
    explicit GlWindow(bool windowed);
    ~GlWindow();
    GlWindow(const GlWindow &) = delete;
    GlWindow &operator=(const GlWindow &) = delete;

    SDL_Window *window() const {
        return window_;
    }
    int width() const {
        return width_;
    }
    int height() const {
        return height_;
    }
    void swap() const {
        SDL_GL_SwapWindow(window_);
    }
};

} // namespace nativetv
