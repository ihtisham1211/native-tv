#include "platform/gl_window.hpp"

namespace nativetv {

GlWindow::GlWindow(bool windowed) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_EVENTS))
        throw WindowError(WindowStage::Display, SDL_GetError());
#ifdef __APPLE__
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
    window_ = SDL_CreateWindow("nativeTv Native", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               windowed ? 1280 : 1920, windowed ? 720 : 1080,
                               SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);
    if (window_)
        context_ = SDL_GL_CreateContext(window_);
    if (!context_) {
        // The destructor does not run for a constructor that throws.
        const WindowError error(WindowStage::Context, SDL_GetError());
        if (window_)
            SDL_DestroyWindow(window_);
        SDL_Quit();
        throw error;
    }
    SDL_GL_GetDrawableSize(window_, &width_, &height_);
}

GlWindow::~GlWindow() {
    SDL_GL_DeleteContext(context_);
    SDL_DestroyWindow(window_);
}

} // namespace nativetv
