#include "adapters/http_services.hpp"
#include "graphics/debug.hpp"
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
#include <sys/resource.h>
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

// What the loop acts on. Anything else (mouse motion, a window move or exposure) is let go
// during the pause between frames: the window manager or a Magic Remote pointer can send them at
// the display rate, and each one used to cut the 30 fps ambient pause short.
bool endsPause(const SDL_Event &event) {
    switch (event.type) {
    case SDL_QUIT:
    case SDL_KEYDOWN:
    case SDL_KEYUP:
    case SDL_TEXTINPUT:
    case SDL_MOUSEBUTTONUP:
    case SDL_MOUSEWHEEL:
        return true;
    case SDL_WINDOWEVENT:
        return event.window.event == SDL_WINDOWEVENT_FOCUS_LOST ||
               event.window.event == SDL_WINDOWEVENT_HIDDEN ||
               event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED ||
               event.window.event == SDL_WINDOWEVENT_SHOWN;
    default:
        return false;
    }
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

// NATIVETV_DEBUG_BENCH=MS: once, at MS into the run, draws the current frame repeatedly under
// the debug switches (graphics/debug.hpp), then synthetic layers, and prints the GPU milliseconds
// per frame (glFinish after every draw), so each cost can be read off one run on the TV.
void benchmark(Renderer &renderer, const Scene &scene) {
    // "x" names nothing: the frame as it is. The first one warms the clocks up.
    static const char *variants[] = {"x",       "x",      "wall",      "wallimg", "wallgrad",
                                     "images",  "text",   "shadows",   "rings",   "fills",
                                     "gradients", "icons", "wall,images,text,shadows,rings,fills,gradients,icons",
                                     "flat",    "noblend", "half",     "nocore"};
    const DebugSkip original = debugSkip();
    for (const char *variant : variants) {
        debugSkip() = parseDebugSkip(variant);
        std::fprintf(stderr, "bench %-60s %6.2f ms\n", variant, renderer.timeFrames(scene, 12));
    }
    // What one full-screen layer of each kind costs: eight of them, less the empty frame.
    struct Layer {
        const char *name;
        bool rounded, gradient, opaque;
        const char *skip;
    };
    static const Layer layers[] = {{"flat blended", false, false, false, "x"},
                                   {"flat opaque, blend on", false, false, true, "x"},
                                   {"flat opaque, blend off", false, false, true, "noblend"},
                                   {"gradient blended", false, true, false, "x"},
                                   {"distance field blended", true, false, false, "nocore"},
                                   {"distance field, blend off", true, false, true, "nocore,noblend"}};
    debugSkip() = parseDebugSkip("x");
    Scene empty;
    empty.background = {0, 0, 0, 1};
    double base = renderer.timeFrames(empty, 12);
    std::fprintf(stderr, "bench empty frame %.2f ms\n", base);
    for (const Layer &layer : layers) {
        Scene synthetic;
        synthetic.background = {0, 0, 0, 1};
        const float alpha = layer.opaque ? 1.f : .5f;
        for (int i = 0; i < 8; ++i) {
            Node &node = synthetic.shape({0, 0, 1920, 1080}, {.2f, .3f, .4f, alpha}, layer.rounded ? 12.f : 0);
            if (layer.gradient)
                node.gradient = Gradient::between(90, {1, 0, 0, alpha}, {0, 0, 1, alpha});
        }
        debugSkip() = parseDebugSkip(layer.skip);
        double ms = renderer.timeFrames(synthetic, 12);
        std::fprintf(stderr, "bench layer %-28s %6.2f ms per full-screen layer\n", layer.name,
                     (ms - base) / 8);
    }
    // What a draw costs: forty small shapes in a chain, each overlapping the last, so none can
    // join another's draw (square corners and round alternate, so neighbours differ in program).
    debugSkip() = parseDebugSkip("x");
    Scene chain;
    chain.background = {0, 0, 0, 1};
    for (int i = 0; i < 40; ++i)
        chain.shape({100.f + i * 10, 100, 40, 40}, {.2f, .3f, .4f, .5f}, i % 2 ? 0 : 8);
    double chained = renderer.timeFrames(chain, 12);
    std::fprintf(stderr, "bench %zu chained draws %.2f ms, %.3f ms a draw\n",
                 static_cast<size_t>(renderer.counters().draws), chained,
                 (chained - base) / renderer.counters().draws);
    debugSkip() = parseDebugSkip("x");
    renderer.timeFrames(scene, 4, true);
    debugSkip() = original;
}

double millisecondsSince(Uint64 counter) {
    return (SDL_GetPerformanceCounter() - counter) * 1000.0 / SDL_GetPerformanceFrequency();
}
} // namespace
int main(int argc, char **argv) {
    int result = 0;
    try {
        Options opts = options(argc, argv);
        if (opts.probe)
            return runProbe(opts.windowed);
        std::setlocale(LC_TIME, "");
        // The loop wakes on the display's schedule while the TV runs a load average near 30, and
        // at normal priority the wake-ups come up to several ms late: 16.7 ms frames spread over
        // 14 to 19 ms (p95 18.4 ms against 17.5 ms at -10). Refused without the privilege, which
        // costs nothing but that.
        const bool priorityRaised = setpriority(PRIO_PROCESS, 0, -10) == 0;
        SDL_SetHint("SDL_WEBOS_ACCESS_POLICY_KEYS_BACK", "true");
        SDL_SetHint("SDL_WEBOS_ACCESS_POLICY_KEYS_EXIT", "true");
        // Registers the app with sam (registerNativeApp), the only way sam can send a running
        // native app its relaunch: without it a launch of the app behind the Home stub leaves
        // the stub in front (a black screen) until the client is restarted.
        SDL_SetHint("SDL_HINT_WEBOS_REGISTER_APP", "true");
        // Milliseconds from here, written to the --report file, to see where start-up goes.
        const Uint64 startedAt = SDL_GetPerformanceCounter();
        Json startup = Json::object();
        auto markStartup = [&](const char *phase) {
            startup.set(phase, Json::numeric(millisecondsSince(startedAt)));
        };
        GlWindow gl(opts.windowed);
        if (TTF_Init())
            throw std::runtime_error(TTF_GetError());
        IMG_Init(IMG_INIT_JPG | IMG_INIT_PNG);
        SDL_StartTextInput();
        installTerminationHandlers();
        const int width = gl.width(), height = gl.height();
        std::fprintf(stderr, "Native Tv native: %dx%d, %s, %s%s\n", width, height,
                     glGetString(GL_RENDERER), glGetString(GL_VERSION),
                     priorityRaised ? ", priority raised" : "");
        {
            markStartup("windowMs");
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
            markStartup("rendererMs");
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
            app.holdForSetupCheck();
            markStartup("atlasesMs");
            prewarm(renderer, glyphs, glyphAtlas, iconAtlas, runs);
            markStartup("prewarmMs");
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
            // OK and Back act when released, unless held for 650 ms: then OK starts editing and
            // Back opens the recent apps, once, and the release is swallowed.
            Uint32 okAt = 0, backAt = 0;
            bool okDown = false, held = false, backDown = false, backHeld = false, draw = true,
                 quit = false, active = true, animating = true;
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
            const char *benchEnv = std::getenv("NATIVETV_DEBUG_BENCH");
            const long benchAt = benchEnv ? std::atol(benchEnv) : -1;
            bool benched = false;
            // The swap interval is set once. Changing it between ambient and interactive made
            // the first frame after the change take two display frames on the B3, a hitch on the
            // first key press after every idle spell, so ambient pacing is the loop's sleep.
            SDL_GL_SetSwapInterval(fixed ? 0 : 1);
            int pace = -1; // 2 while ambient, 1 otherwise (0 in a fixed-clock run)
            Uint64 lastSwapAt = 0;
            bool lastSwapMeasurable = false;
            // The oldest input event not yet shown, for the perf log's input-to-swap time. An
            // event the pacing sleep pushed back keeps the stamp it woke with, because
            // SDL_PushEvent restamps it.
            Uint32 inputAt = 0, repushedAt = 0;
            bool inputPending = false;
            auto noteInput = [&](Uint32 stamp) {
                const Uint32 polled = SDL_GetTicks();
                if (repushedAt) {
                    stamp = repushedAt;
                    repushedAt = 0;
                }
                if (!stamp || static_cast<Sint32>(stamp - polled) > 0)
                    stamp = polled;
                if (!inputPending) {
                    inputPending = true;
                    inputAt = stamp;
                }
            };
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
                    if (active && backDown && !backHeld) {
                        const Sint32 due = static_cast<Sint32>(backAt + 650 - waited);
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
                    if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP ||
                        event.type == SDL_TEXTINPUT || event.type == SDL_MOUSEBUTTONUP ||
                        event.type == SDL_MOUSEWHEEL)
                        noteInput(event.common.timestamp);
                    if (event.type == SDL_QUIT)
                        quit = true;
                    else if (event.type == SDL_WINDOWEVENT && !stops) {
                        // A bounded run (`--time`, `--duration-ms`) has to reach its end
                        // and draw whatever the window manager does with focus.
                        if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST ||
                            event.window.event == SDL_WINDOWEVENT_HIDDEN) {
                            active = false;
                            okDown = backDown = false;
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
                            } else if (key == Key::Back) {
                                if (!backDown && !event.key.repeat) {
                                    backDown = true;
                                    backHeld = false;
                                    backAt = now;
                                }
                            } else if (!event.key.repeat || key != Key::Red)
                                app.key(key);
                        }
                    } else if (event.type == SDL_KEYUP) {
                        Key key;
                        const bool known = translate(event.key.keysym.sym, key);
                        if (known && key == Key::Ok && okDown) {
                            okDown = false;
                            if (!held)
                                app.key(Key::Ok);
                        } else if (backDown &&
                                   ((known && key == Key::Back) ||
                                    event.key.keysym.scancode == 482 ||
                                    event.key.keysym.scancode == 505)) {
                            backDown = false;
                            if (!backHeld)
                                app.key(Key::Back);
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
                repushedAt = 0;
                if (okDown && !held && now - okAt >= 650 && app.screenName() == "home") {
                    held = true;
                    app.longPress();
                }
                if (backDown && !backHeld && now - backAt >= 650 && app.screenName() == "home") {
                    backHeld = true;
                    app.longBack();
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
                    if (instruction.has("text") || instruction.has("key") ||
                        instruction.has("click") || instruction.has("longPress") ||
                        instruction.has("longBack"))
                        noteInput(0); // a scripted input is stamped when the loop takes it
                    if (instruction.has("text"))
                        app.input(instruction["text"].string());
                    if (instruction.has("key"))
                        app.key(scriptedKey(instruction["key"].string()));
                    if (instruction["longPress"].boolean()) // OK held past the long-press time
                        app.longPress();
                    if (instruction["longBack"].boolean()) // Back held past it
                        app.longBack();
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
                    if (wanted != pace) {
                        pace = wanted;
                        lastSwapMeasurable = false; // the pause between paces is not a frame
                    }
                    runs.beginFrame();
                    const Scene &scene = app.frame();
                    if (benchAt >= 0 && !benched && now >= static_cast<Uint32>(benchAt)) {
                        benched = true;
                        benchmark(renderer, scene);
                    }
                    const double sceneMs = millisecondsSince(frameStart);
                    renderer.draw(scene);
                    const RenderCounters &drawn = renderer.counters();
                    if (lastFrame && !opts.capture.empty())
                        renderer.capture(opts.capture);
                    gl.swap();
                    if (!startup.has("firstSwapMs"))
                        markStartup("firstSwapMs");
                    const Uint64 swappedAt = SDL_GetPerformanceCounter();
                    if (perf) {
                        FrameStats stats;
                        stats.buildMs = sceneMs + drawn.batchMs;
                        stats.submitMs = drawn.submitMs;
                        stats.draws = drawn.draws;
                        stats.vertices = drawn.vertices;
                        stats.textureBytes = drawn.textureBytes;
                        if (inputPending)
                            stats.inputMs = static_cast<Sint32>(SDL_GetTicks() - inputAt);
                        perf->record(now,
                                     lastSwapMeasurable
                                         ? (swappedAt - lastSwapAt) * 1000.0 /
                                               SDL_GetPerformanceFrequency()
                                         : -1,
                                     wanted, stats);
                    }
                    lastSwapAt = swappedAt;
                    lastSwapMeasurable = true;
                    // eglSwapBuffers returns before the next buffer is free, and the first GL call
                    // of the next frame then waits for it (21 ms about every seven seconds on the
                    // B3), after the clock was read and the keys taken. Waiting here puts that wait
                    // before they are.
                    if (!fixed && !lastFrame)
                        renderer.acquireBackBuffer();
                    ++frames;
                    draw = false;
                    // Ambient frames are paced to 30 fps here; the rest of the time the swap waits
                    // for the display, and where it does not (a headless context) the floor
                    // keeps the loop from spinning.
                    const double frameMs = wanted == 2 ? kAmbientFrameMs : kMinFrameMs;
                    const double spent = millisecondsSince(frameStart);
                    if (!fixed && spent < frameMs) {
                        // Waiting on events instead of sleeping, so a key press ends the pause.
                        for (double left = frameMs - spent; left > 0;
                             left = frameMs - millisecondsSince(frameStart)) {
                            SDL_Event woken;
                            if (SDL_WaitEventTimeout(&woken, static_cast<int>(left + 0.999)) != 1)
                                break;
                            if (endsPause(woken)) {
                                repushedAt = woken.common.timestamp;
                                SDL_PushEvent(&woken);
                                break;
                            }
                        }
                    }
                } else
                    lastSwapMeasurable = false;
                // An input that drew nothing is not carried into a later, unrelated frame.
                inputPending = false;
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
                            {"startup", startup},
                            {"screens", screens},
                            {"checkpoints", checkpoints}});
                std::ofstream file(opts.report);
                file << report.dump() << '\n';
            }
        }
    } catch (const std::exception &error) {
        std::fprintf(stderr, "Native Tv native: %s\n", error.what());
        result = 1;
    }
    IMG_Quit();
    TTF_Quit();
    SDL_Quit();
    return result;
}
