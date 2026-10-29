#include "adapters/http_services.hpp"
#include "graphics/glyph_atlas.hpp"
#include "graphics/icon_atlas.hpp"
#include "graphics/image_loader.hpp"
#include "graphics/renderer.hpp"
#include "graphics/text.hpp"
#include "platform/client_config.hpp"
#include "platform/app_clock.hpp"
#include "platform/client_launch.hpp"
#include "platform/frame_stats.hpp"
#include "platform/gl_window.hpp"
#include "platform/paths.hpp"
#include "platform/probe.hpp"
#include "ui/application.hpp"
#include "ui/kit/run_glyphs.hpp"
#include <SDL_image.h>
#include <algorithm>
#include <clocale>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
using namespace nativetv;
bool translate(SDL_Keycode code, Key &key) {
    if (code == SDLK_LEFT)
        key = Key::Left;
    else if (code == SDLK_RIGHT)
        key = Key::Right;
    else if (code == SDLK_UP)
        key = Key::Up;
    else if (code == SDLK_DOWN)
        key = Key::Down;
    else if (code == SDLK_RETURN || code == SDLK_KP_ENTER)
        key = Key::Ok;
    else if (code == SDLK_ESCAPE || code == SDLK_AC_BACK || code == 461 || code == 0x100019c)
        key = Key::Back;
    else if (code == SDLK_F1 || code == 403)
        key = Key::Red;
    else if (code == SDLK_PAGEUP || code == 402)
        key = Key::PageUp;
    else if (code == SDLK_PAGEDOWN || code == 427)
        key = Key::PageDown;
    else if (code == SDLK_BACKSPACE || code == SDLK_DELETE)
        key = Key::Delete;
    else
        return false;
    return true;
}
struct Options {
    std::string server, font = "/usr/share/fonts/LG_Smart_UI-Regular.ttf",
                screen = "home", capture, script, report, perf;
    Uint32 duration = 0;
    long long time = -1; // --time: fixed clock, stop and capture at this many ms; -1 is off
    bool windowed = false, probe = false;
};
Options options(int argc, char **argv) {
    Options result;
    result.font = resourceDirectory(argv[0]) + "/assets/Roboto-Regular.woff";
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i], value;
        if (!arg.empty() && arg[0] == '{') {
            (void)applyClientLaunchParameters(arg, result.screen);
            continue;
        }
        // "--name value" and "--name=value" both work.
        const size_t equals = arg.compare(0, 2, "--") == 0 ? arg.find('=') : std::string::npos;
        const bool inlineValue = equals != std::string::npos;
        if (inlineValue) {
            value = arg.substr(equals + 1);
            arg.resize(equals);
        }
        if (arg == "--windowed")
            result.windowed = true;
        else if (arg == "--probe")
            result.probe = true;
        else if (arg == "--help") {
            std::puts("nativetv-tv [--server http://127.0.0.1:PORT] [--font font.ttf] [--screen "
                      "splash|home|dashboard|setup:welcome|kit:keyboard] [--windowed] [--script "
                      "steps.json] [--capture image.ppm] [--duration-ms N] [--time=MS] "
                      "[--perf=log.jsonl] [--report report.json] [--probe]");
            std::exit(0);
        } else if (inlineValue || i + 1 < argc) {
            if (!inlineValue)
                value = argv[++i];
            if (arg == "--server")
                result.server = value;
            else if (arg == "--font")
                result.font = value;
            else if (arg == "--screen")
                result.screen = value;
            else if (arg == "--capture")
                result.capture = value;
            else if (arg == "--script")
                result.script = value;
            else if (arg == "--report")
                result.report = value;
            else if (arg == "--perf")
                result.perf = value;
            else if (arg == "--duration-ms")
                result.duration = static_cast<Uint32>(std::stoul(value));
            else if (arg == "--time") {
                result.time = std::stoll(value);
                if (result.time < 0)
                    throw std::runtime_error("--time must not be negative");
            }
            else
                throw std::runtime_error("Unknown option: " + arg);
        } else
            throw std::runtime_error("Missing option value: " + arg);
    }
    if (result.server.empty() && !result.probe)
        result.server = clientServerOrigin(clientServerConfigPath());
    return result;
}
std::string read(const std::string &path) {
    std::ifstream stream(path);
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}
Key scriptedKey(const std::string &name) {
    if (name == "left")
        return Key::Left;
    if (name == "right")
        return Key::Right;
    if (name == "up")
        return Key::Up;
    if (name == "down")
        return Key::Down;
    if (name == "back")
        return Key::Back;
    if (name == "red")
        return Key::Red;
    if (name == "delete")
        return Key::Delete;
    return Key::Ok;
}
// Longest the loop sleeps in SDL_WaitEventTimeout. Idle still has to look at the
// application's timers and the HTTP worker's responses, neither of which wakes
// the loop. Paused (focus lost) only has to notice the window coming back.
constexpr Uint32 kIdleWaitMs = 50, kPausedWaitMs = 250;
// Fastest the loop draws when the display does not hold it to its refresh rate
// (a headless context). A 120 Hz panel takes 8.3 ms a frame, so this never slows it.
constexpr double kMinFrameMs = 8, kAmbientFrameMs = 33;
// Wall clock of a `--time` run, so the clock text does not change the capture.
constexpr int kFixedHour = 10, kFixedMinute = 9, kFixedWeekday = 4, kFixedDay = 15,
              kFixedMonth = 0;

volatile std::sig_atomic_t terminationRequested = 0;
void requestTermination(int) {
    terminationRequested = 1;
}
// SDL turns SIGTERM into SDL_QUIT on some platforms only, so the loop watches a
// flag it controls and leaves through its normal shutdown.
void installTerminationHandlers() {
    struct sigaction action = {};
    action.sa_handler = requestTermination;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, nullptr);
    sigaction(SIGINT, &action, nullptr);
}

// True while the only animation running is ambient (Ken Burns, hero progress), which looks the
// same at 30 fps. NATIVETV_AMBIENT=1 treats every animation as ambient, to exercise the 30 fps
// path.
bool ambientOnly(const Application &app) {
    static const bool forced = std::getenv("NATIVETV_AMBIENT") != nullptr;
    return forced || app.ambientOnly();
}

// The application asks for pictures at the size it draws them; the loader decodes them off the
// render thread and uploads a megabyte a frame.
class LoaderPictures final : public Pictures {
  public:
    explicit LoaderPictures(ImageLoader &loader) : loader_(loader) {}
    ImageView view(const std::string &id, int width, int height, ImageFit fit,
                   double nowMs) override {
        return loader_.get(id, width, height, fit, nowMs);
    }

  private:
    ImageLoader &loader_;
};

// Compiles nothing new but makes the first visible frame cheap: the glyphs of every style the
// pages use and the icons of the bar and rows are placed in their atlases, and the designed
// pages are drawn once into the back buffer so every shader program and texture binding has run.
// It happens before the first swap, which is behind the splash that covers the screen at boot.
void prewarm(Renderer &renderer, kit::RunGlyphs &glyphs, GlyphAtlas &atlas, IconAtlas &icons,
             TextRuns &runs) {
    using namespace kit;
    static const TypeStyle styles[] = {type::display,  type::headline, type::title,   type::body,
                                       type::bodyL,    type::nav,      type::clock,    type::labelL,
                                       type::labelM,   type::labelS,   type::cardTitle, type::inputName,
                                       type::heroSource};
    for (const TypeStyle &style : styles)
        for (uint32_t c = 32; c < 127; ++c)
            atlas.place(atlas.glyph(static_cast<int>(style.px + .5f), style.weight, c));
    icons.prewarm({"settings", "search", "play", "apps", "home", "hdmi", "tv"}, {18, 28, 34, 40, 52});
    for (const char *name : {"home", "apps"}) {
        runs.beginFrame();
        Scene scene;
        if (designGallery(Paint{scene, glyphs}, name))
            renderer.draw(scene);
    }
}

double millisecondsSince(Uint64 counter) {
    return (SDL_GetPerformanceCounter() - counter) * 1000.0 / SDL_GetPerformanceFrequency();
}
// Interval 2 is refused by some drivers and silently clamped to 1 by others
// (EGL does that past EGL_MAX_SWAP_INTERVAL, Cocoa always), so the loop also
// sleeps to 30 fps in ambient mode; where the swap already waits, that adds nothing.
void setSwapInterval(int wanted) {
    if (SDL_GL_SetSwapInterval(wanted) != 0)
        SDL_GL_SetSwapInterval(1);
}
} // namespace
int main(int argc, char **argv) {
    int result = 0;
    try {
        Options opts = options(argc, argv);
        if (opts.probe)
            return runProbe(opts.windowed);
        std::setlocale(LC_TIME, "");
        SDL_SetHint("SDL_WEBOS_ACCESS_POLICY_KEYS_BACK", "true");
        SDL_SetHint("SDL_WEBOS_ACCESS_POLICY_KEYS_EXIT", "true");
        GlWindow gl(opts.windowed);
        if (TTF_Init())
            throw std::runtime_error(TTF_GetError());
        IMG_Init(IMG_INIT_JPG | IMG_INIT_PNG);
        SDL_StartTextInput();
        installTerminationHandlers();
        const int width = gl.width(), height = gl.height();
        std::fprintf(stderr, "nativeTv native: %dx%d, %s, %s\n", width, height,
                     glGetString(GL_RENDERER), glGetString(GL_VERSION));
        {
            HttpServices http(opts.server);
            Application app(http, opts.screen);
            const std::string assets = resourceDirectory(argv[0]) + "/assets";
            Renderer renderer(width, height, opts.font, assets,
                              [&http](const std::string &path) { return http.image(path) != 0; });
            // The atlases and the loader hold textures in the renderer, so they are declared
            // after it and destroyed before it.
            GLint maxTexture = 2048;
            glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTexture);
            const int atlasSide = std::min<int>(2048, maxTexture);
            GlyphAtlas glyphAtlas(&renderer, assets + "/fonts", atlasSide, atlasSide / 2);
            IconAtlas iconAtlas(&renderer, assets + "/icons", std::min<int>(1024, maxTexture),
                                std::min<int>(512, maxTexture));
            TextRuns runs(glyphAtlas);
            renderer.setTextRuns(&runs);
            kit::RunGlyphs glyphs(runs, glyphAtlas, iconAtlas);
            ImageLoader loader(renderer,
                               [&http](const std::string &id) { return http.image(id) != 0; });
            LoaderPictures pictures(loader);
            app.attach(glyphs, pictures);
            prewarm(renderer, glyphs, glyphAtlas, iconAtlas, runs);
            Json script = opts.script.empty() ? Json::array() : Json::parse(read(opts.script));
            size_t step = 0;
            if (!script.isArray())
                throw std::runtime_error("The script must be a JSON array");
            std::unique_ptr<FrameLog> perf;
            if (!opts.perf.empty())
                perf.reset(new FrameLog(opts.perf));
            const bool fixed = opts.time >= 0;
            const bool stops = fixed || opts.duration;
            const Uint32 stopAt = fixed ? static_cast<Uint32>(opts.time) : opts.duration;
            AppClock clock(fixed, SDL_GetTicks());
            Uint32 okAt = 0;
            bool okDown = false, held = false, draw = true, quit = false, active = true,
                 animating = true;
            unsigned frames = 0;
            Json screens = Json::array();
            Json checkpoints = Json::array();
            std::string lastScreen;
            Uint32 wallClockAt = 0;
            bool scriptedWallClock = false;
            if (fixed) {
                app.wallClock(kFixedHour, kFixedMinute, kFixedWeekday, kFixedDay, kFixedMonth);
                scriptedWallClock = true;
            }
            // Replies to the application, pictures to the loader. True when anything arrived.
            auto pumpNetwork = [&](Uint32 stamp) {
                std::vector<Response> replies = http.drain();
                for (Response &response : replies) {
                    if (response.channel.compare(0, 6, "image:") != 0)
                        app.response(response);
                    else if (!response.ok()) {
                        loader.fail(response.path, stamp);
                        app.imageFailed(response.path);
                    } else
                        loader.provide(response.path, std::move(response.bytes), stamp);
                }
                return !replies.empty();
            };
            Uint32 lastNow = 0;
            int swapInterval = -1; // -1: not set yet
            Uint64 lastSwapAt = 0;
            bool lastSwapMeasurable = false;
            while (!quit && !terminationRequested && app.running()) {
                // Nothing to draw: sleep until an event or the nearest timer.
                const bool idle = !fixed && (!active || (!draw && !animating));
                SDL_Event event;
                bool pending = false;
                if (idle) {
                    Uint32 wait = active ? kIdleWaitMs : kPausedWaitMs;
                    const Uint32 waited = clock.tick(SDL_GetTicks());
                    if (active && step < script.size()) {
                        const double due = script[step]["at"].number() - waited;
                        wait = std::min(wait, due < 1 ? 1u : static_cast<Uint32>(due));
                    }
                    if (active && okDown && !held) {
                        const Sint32 due = static_cast<Sint32>(okAt + 650 - waited);
                        wait = std::min(wait, due < 1 ? 1u : static_cast<Uint32>(due));
                    }
                    pending = SDL_WaitEventTimeout(&event, static_cast<int>(wait)) == 1;
                }
                // A fixed-clock run waits for what the last frame asked of the server, so the
                // frame at a given time shows the same pictures however fast the machine is.
                if (fixed)
                    for (int wait = 0; wait < 4000; ++wait) {
                        bool arrived = pumpNetwork(lastNow);
                        if (!arrived && http.idle() && loader.pendingDecodes() == 0)
                            break;
                        SDL_Delay(1);
                    }
                const Uint64 frameStart = SDL_GetPerformanceCounter();
                const Uint32 now = clock.tick(SDL_GetTicks());
                lastNow = now;
                if (!scriptedWallClock &&
                    (!wallClockAt || static_cast<Sint32>(now - wallClockAt) >= 0)) {
                    std::time_t time = std::time(nullptr);
                    std::tm local;
                    localtime_r(&time, &local);
                    app.wallClock(local.tm_hour, local.tm_min, local.tm_wday, local.tm_mday,
                                  local.tm_mon);
                    wallClockAt = now + 1000;
                }
                for (bool more = pending || SDL_PollEvent(&event); more;
                     more = SDL_PollEvent(&event)) {
                    if (event.type == SDL_QUIT)
                        quit = true;
                    else if (event.type == SDL_WINDOWEVENT && !stops) {
                        // A bounded run (`--time`, `--duration-ms`) has to reach its end
                        // and draw whatever the window manager does with focus.
                        if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST ||
                            event.window.event == SDL_WINDOWEVENT_HIDDEN) {
                            active = false;
                            okDown = false;
                            clock.pause(SDL_GetTicks());
                            app.active(false);
                        }
                        if (event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED ||
                            event.window.event == SDL_WINDOWEVENT_SHOWN) {
                            active = true;
                            clock.resume(SDL_GetTicks());
                            app.active(true);
                            draw = true;
                        }
                    } else if (event.type == SDL_KEYDOWN) {
                        Key key;
                        bool known = translate(event.key.keysym.sym, key);
                        if (event.key.keysym.scancode == 482 || event.key.keysym.scancode == 505) {
                            key = Key::Back;
                            known = true;
                        }
                        if (event.key.keysym.scancode == 486) {
                            key = Key::Red;
                            known = true;
                        }
                        if (event.key.keysym.scancode == 480 || event.key.keysym.scancode == 481) {
                            key = event.key.keysym.scancode == 480 ? Key::PageUp : Key::PageDown;
                            known = true;
                        }
                        if (known) {
                            if (key == Key::Ok) {
                                if (!okDown && !event.key.repeat) {
                                    okDown = true;
                                    held = false;
                                    okAt = now;
                                }
                            } else if (!event.key.repeat || (key != Key::Back && key != Key::Red))
                                app.key(key);
                        }
                    } else if (event.type == SDL_KEYUP) {
                        Key key;
                        if (translate(event.key.keysym.sym, key) && key == Key::Ok && okDown) {
                            okDown = false;
                            if (!held)
                                app.key(Key::Ok);
                        }
                    } else if (event.type == SDL_TEXTINPUT)
                        app.input(event.text.text);
                    else if (event.type == SDL_MOUSEBUTTONUP &&
                             event.button.button == SDL_BUTTON_LEFT) {
                        int w, h;
                        SDL_GetWindowSize(gl.window(), &w, &h);
                        app.click(event.button.x * 1920.0f / w, event.button.y * 1080.0f / h);
                    } else if (event.type == SDL_MOUSEWHEEL)
                        app.key(event.wheel.y > 0 ? Key::Up : Key::Down);
                }
                if (okDown && !held && now - okAt >= 650 && app.screenName() == "home") {
                    held = true;
                    app.longPress();
                }
                if (pumpNetwork(now))
                    draw = true;
                while (step < script.size() && now >= script[step]["at"].number()) {
                    Json instruction = script[step++];
                    if (instruction.has("wallClock")) {
                        double hour = instruction["wallClock"]["hour"].number(-1);
                        double minute = instruction["wallClock"]["minute"].number(-1);
                        double weekday = instruction["wallClock"]["weekday"].number(-1);
                        double day = instruction["wallClock"]["day"].number(-1);
                        double month = instruction["wallClock"]["month"].number(-1);
                        if (hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
                            hour != static_cast<int>(hour) || minute != static_cast<int>(minute) ||
                            weekday < -1 || weekday > 6 ||
                            weekday != static_cast<int>(weekday) || day < -1 || day > 31 ||
                            day != static_cast<int>(day) || month < -1 || month > 11 ||
                            month != static_cast<int>(month))
                            throw std::runtime_error("Invalid scripted wall clock");
                        app.wallClock(static_cast<int>(hour), static_cast<int>(minute),
                                      static_cast<int>(weekday), static_cast<int>(day),
                                      static_cast<int>(month));
                        scriptedWallClock = true;
                    }
                    if (instruction.has("text"))
                        app.input(instruction["text"].string());
                    if (instruction.has("key"))
                        app.key(scriptedKey(instruction["key"].string()));
                    if (instruction.has("click"))
                        app.click(static_cast<float>(
                                      instruction["click"][static_cast<size_t>(0)].number()),
                                  static_cast<float>(
                                      instruction["click"][static_cast<size_t>(1)].number()));
                    if (instruction.has("activate") &&
                        !app.activate(instruction["activate"].string()))
                        throw std::runtime_error("Visible target not found: " +
                                                 instruction["activate"].string());
                    if (instruction.has("capture")) {
                        runs.beginFrame();
                        renderer.draw(app.frame());
                        renderer.capture(instruction["capture"].string());
                    }
                    if (instruction.has("expectScreen") &&
                        instruction["expectScreen"].string() != app.screenName())
                        throw std::runtime_error("Expected " +
                                                 instruction["expectScreen"].string() + ", got " +
                                                 app.screenName());
                    if (instruction.has("expectText") &&
                        !app.shows(instruction["expectText"].string()))
                        throw std::runtime_error("Expected text missing on " + app.screenName());
                    if (instruction.has("expectWallpaper")) {
                        std::string expected = instruction["expectWallpaper"].string();
                        std::string actual = app.snapshot()["wallpaper"].string();
                        if (actual != expected)
                            throw std::runtime_error("Expected wallpaper " + expected + ", got " +
                                                     actual);
                    }
                    if (instruction.has("expectWallpaperSlot")) {
                        std::string expected = instruction["expectWallpaperSlot"].string();
                        std::string actual = app.snapshot()["wallpaperSlot"].string();
                        if (actual != expected)
                            throw std::runtime_error("Expected wallpaper slot " + expected +
                                                     ", got " + actual);
                    }
                    if (instruction.has("checkpoint"))
                        checkpoints.add(fields({{"name", instruction["checkpoint"]},
                                                {"at", Json::numeric(now)},
                                                {"state", app.snapshot()}}));
                    draw = true;
                }
                // Pictures arrive on the render thread a megabyte at a time and fade in.
                const size_t uploaded = loader.pump(renderer.uploadBudget(), now);
                const bool loading =
                    uploaded > 0 || loader.pendingDecodes() > 0 || loader.fading(now);
                animating = app.update(now) || loading;
                const bool ambient = animating && !loading && ambientOnly(app);
                draw = animating || draw || fixed;
                // The last frame of a bounded run is always drawn, so it can be captured.
                const bool lastFrame = stops && now >= stopAt;
                if ((active || lastFrame) && (draw || lastFrame)) {
                    const int wanted = fixed ? 0 : (ambient ? 2 : 1);
                    if (wanted != swapInterval) {
                        setSwapInterval(wanted);
                        swapInterval = wanted;
                        lastSwapMeasurable = false;
                    }
                    runs.beginFrame();
                    const Scene &scene = app.frame();
                    const double sceneMs = millisecondsSince(frameStart);
                    renderer.draw(scene);
                    const RenderCounters &drawn = renderer.counters();
                    if (lastFrame && !opts.capture.empty())
                        renderer.capture(opts.capture);
                    gl.swap();
                    const Uint64 swappedAt = SDL_GetPerformanceCounter();
                    if (perf) {
                        FrameStats stats;
                        stats.buildMs = sceneMs + drawn.batchMs;
                        stats.submitMs = drawn.submitMs;
                        stats.draws = drawn.draws;
                        stats.vertices = drawn.vertices;
                        stats.textureBytes = drawn.textureBytes;
                        perf->record(now,
                                     lastSwapMeasurable
                                         ? (swappedAt - lastSwapAt) * 1000.0 /
                                               SDL_GetPerformanceFrequency()
                                         : -1,
                                     wanted, stats);
                    }
                    lastSwapAt = swappedAt;
                    lastSwapMeasurable = true;
                    ++frames;
                    draw = false;
                    // Some contexts do not wait for the display; keep the loop from spinning.
                    const double frameMs = wanted == 2 ? kAmbientFrameMs : kMinFrameMs;
                    const double spent = millisecondsSince(frameStart);
                    if (!fixed && spent < frameMs)
                        SDL_Delay(static_cast<Uint32>(frameMs - spent + 0.999));
                } else
                    lastSwapMeasurable = false;
                std::string screen = app.screenName();
                if (screen != lastScreen) {
                    screens.add(
                        fields({{"screen", Json::text(screen)}, {"at", Json::numeric(now)}}));
                    lastScreen = screen;
                }
                if (lastFrame)
                    break;
                if (opts.screen == "splash" && now >= 100000)
                    break;
            }
            if (perf) {
                perf->finish();
                std::puts(summaryLine(perf->summary()).c_str());
            }
            if (!opts.report.empty()) {
                Json report =
                    fields({{"renderer",
                             Json::text(reinterpret_cast<const char *>(glGetString(GL_RENDERER)))},
                            {"glVersion",
                             Json::text(reinterpret_cast<const char *>(glGetString(GL_VERSION)))},
                            {"width", Json::numeric(width)},
                            {"height", Json::numeric(height)},
                            {"frames", Json::numeric(frames)},
                            {"textureBytes", Json::numeric(renderer.textureBytes())},
                            {"screens", screens},
                            {"checkpoints", checkpoints}});
                std::ofstream file(opts.report);
                file << report.dump() << '\n';
            }
        }
    } catch (const std::exception &error) {
        std::fprintf(stderr, "nativeTv native: %s\n", error.what());
        result = 1;
    }
    IMG_Quit();
    TTF_Quit();
    SDL_Quit();
    return result;
}
