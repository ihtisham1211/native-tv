#include "graphics/image_loader.hpp"
#include <SDL.h>
#include <SDL_image.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <stdexcept>

using namespace nativetv;
namespace {
int checks = 0;
void check(bool condition, const char *message) {
    ++checks;
    if (!condition)
        throw std::runtime_error(message);
}
struct StubStore final : TextureStore {
    struct Update {
        TextureId texture;
        int firstRow, rows;
    };
    struct Texture {
        int width, height;
        bool alive;
    };
    std::vector<Texture> textures;
    std::vector<Update> updates;
    int destroyed = 0;
    TextureId create(int width, int height, TextureFormat, bool) override {
        textures.push_back({width, height, true});
        return static_cast<TextureId>(textures.size());
    }
    void update(TextureId id, int x, int y, int width, int height, const void *) override {
        check(x == 0 && width == textures.at(id - 1).width &&
                  y + height <= textures.at(id - 1).height,
              "Uploads are whole rows inside the texture");
        updates.push_back({id, y, height});
    }
    void destroy(TextureId id) override {
        textures.at(id - 1).alive = false;
        ++destroyed;
    }
    size_t uploadBudget() const override {
        return 1 << 20;
    }
    int alive() const {
        int count = 0;
        for (const auto &texture : textures)
            count += texture.alive ? 1 : 0;
        return count;
    }
};

std::string encode(int width, int height, bool png,
                   const std::function<uint32_t(int, int)> &pixel) {
    SDL_Surface *surface =
        SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_RGBA32);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            static_cast<uint32_t *>(surface->pixels)[y * (surface->pitch / 4) + x] = pixel(x, y);
    std::vector<char> buffer(1 << 20);
    SDL_RWops *stream = SDL_RWFromMem(buffer.data(), static_cast<int>(buffer.size()));
    if (png)
        IMG_SavePNG_RW(surface, stream, 0);
    else
        IMG_SaveJPG_RW(surface, stream, 0, 95);
    size_t size = static_cast<size_t>(SDL_RWtell(stream));
    SDL_RWclose(stream);
    SDL_FreeSurface(surface);
    return std::string(buffer.data(), size);
}
// RGBA bytes in memory order.
uint32_t rgba(unsigned r, unsigned g, unsigned b, unsigned a = 255) {
    return r | g << 8 | b << 16 | a << 24;
}
const uint8_t *at(const DecodedImage &image, int x, int y) {
    return &image.rgba[(static_cast<size_t>(y) * image.width + x) * 4];
}

void testDecode() {
    std::string halves =
        encode(64, 32, true, [](int x, int) { return x < 32 ? rgba(255, 0, 0) : rgba(0, 0, 255); });
    DecodedImage cover = decodeImage(halves, 16, 16, ImageFit::Cover);
    check(cover.width == 16 && cover.height == 16 && cover.rgba.size() == 16 * 16 * 4,
          "Cover decodes to exactly the requested size");
    // The 2:1 picture is cropped to its middle square, which is still half red, half blue.
    check(at(cover, 2, 8)[0] == 255 && at(cover, 2, 8)[2] == 0 && at(cover, 13, 8)[2] == 255 &&
              at(cover, 13, 8)[0] == 0 && at(cover, 8, 8)[3] == 255,
          "Cover crops the middle of the picture");
    DecodedImage contain = decodeImage(halves, 16, 16, ImageFit::Contain);
    check(contain.width == 16 && contain.height == 8, "Contain keeps the aspect ratio");
    DecodedImage larger =
        decodeImage(encode(4, 4, true, [](int x, int y) { return rgba(x * 80, y * 80, 0); }), 16,
                    16, ImageFit::Cover);
    check(larger.width == 16 && at(larger, 0, 0)[0] == 0 && at(larger, 15, 15)[0] == 240 &&
              at(larger, 8, 0)[0] > 100 && at(larger, 8, 0)[0] < 160,
          "Enlarging blends neighbouring pixels");
    DecodedImage fade = decodeImage(
        encode(4, 2, true, [](int x, int) { return x < 2 ? rgba(255, 0, 0) : rgba(0, 0, 0, 0); }),
        2, 1, ImageFit::Cover);
    check(at(fade, 0, 0)[0] == 255 && at(fade, 0, 0)[3] == 255 && at(fade, 1, 0)[3] == 0,
          "Averaging uses premultiplied colour, so transparency does not darken edges");
    DecodedImage edge = decodeImage(
        encode(3, 1, true, [](int x, int) { return x == 0 ? rgba(255, 0, 0) : rgba(0, 0, 0, 0); }),
        1, 1, ImageFit::Contain);
    check(edge.rgba[0] > 250 && edge.rgba[3] > 80 && edge.rgba[3] < 90,
          "A third-covered red pixel stays red at a third of the alpha");
    std::string jpeg = encode(40, 40, false, [](int, int) { return rgba(120, 130, 140); });
    DecodedImage flat = decodeImage(jpeg, 10, 10, ImageFit::Cover);
    check(flat.width == 10 && std::abs(at(flat, 5, 5)[0] - 120) < 6 &&
              std::abs(at(flat, 5, 5)[2] - 140) < 6,
          "JPEG decodes too");
    check(cover.opaque && flat.opaque && !fade.opaque && !edge.opaque,
          "A picture is opaque when none of its pixels is see-through");
    check(decodeImage("not an image", 10, 10, ImageFit::Cover).rgba.empty() &&
              decodeImage("", 10, 10, ImageFit::Cover).rgba.empty() &&
              decodeImage(halves, 0, 10, ImageFit::Cover).rgba.empty(),
          "Bad input fails cleanly");
}

// Calls `step` until `done` or two seconds pass; each call is one frame.
template <typename Step, typename Done> bool frames(Step step, Done done) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!done() && std::chrono::steady_clock::now() < deadline) {
        step();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return done();
}
ImageLoader::Decoder solid(int width, int height) {
    return [width, height](const std::string &, int, int, ImageFit) {
        DecodedImage image;
        image.width = width;
        image.height = height;
        image.rgba.assign(static_cast<size_t>(width) * height * 4, 200);
        return image;
    };
}

void testOffThreadAndFade() {
    StubStore store;
    std::vector<std::string> fetched;
    std::atomic<int> decodes{0};
    std::thread::id decodeThread;
    std::mutex lock;
    auto decoder = [&](const std::string &bytes, int w, int h, ImageFit fit) {
        {
            std::lock_guard<std::mutex> guard(lock);
            decodeThread = std::this_thread::get_id();
        }
        ++decodes;
        return decodeImage(bytes, w, h, fit);
    };
    ImageLoader loader(
        store,
        [&](const std::string &id) {
            fetched.push_back(id);
            return true;
        },
        1 << 20, 1, decoder);
    double now = 1000;
    ImageView view = loader.get("poster", 32, 20, ImageFit::Cover, now);
    check(view.texture == 0 && fetched.size() == 1 && fetched[0] == "poster",
          "The first request shows the placeholder and asks the app for the bytes");
    loader.get("poster", 32, 20, ImageFit::Cover, now);
    check(fetched.size() == 1, "A repeated request does not fetch again");
    loader.get("poster", 64, 40, ImageFit::Cover, now);
    check(fetched.size() == 2, "Another size is a separate entry");
    loader.provide("poster", encode(80, 50, true, [](int x, int y) { return rgba(x, y, 9); }), now);
    check(frames([] {}, [&] { return loader.pendingDecodes() == 0; }),
          "The loader counts decodes until they finish");
    loader.pump(1 << 20, now);
    check(loader.residentBytes() == 32 * 20 * 4 + 64 * 40 * 4,
          "Both sizes of the provided picture are decoded and uploaded");
    check(decodes == 2 && decodeThread != std::this_thread::get_id(),
          "Decoding ran on a worker thread, not the render thread");
    view = loader.get("poster", 32, 20, ImageFit::Cover, now);
    check(view.texture != 0 && view.width == 32 && view.height == 20 && view.fade == 0,
          "The picture arrives at the exact size with the fade at zero");
    check(view.opaque && !view.failed, "The view says the picture is opaque");
    check(loader.fading(now), "The loader reports the fade so the app keeps drawing");
    check(std::fabs(loader.get("poster", 32, 20, ImageFit::Cover, now + 110).fade - 0.5f) < 0.01f,
          "The fade reaches one half after 110 ms");
    check(loader.get("poster", 32, 20, ImageFit::Cover, now + 300).fade == 1 &&
              !loader.fading(now + 300),
          "The fade ends after 220 ms");
}

void testUploadOrderAndBudget() {
    StubStore store;
    ImageLoader loader(
        store, [](const std::string &) { return true; }, 8 << 20, 1,
        [](const std::string &bytes, int, int, ImageFit) {
            int side = bytes == "a" ? 400 : bytes == "b" ? 500 : 100;
            DecodedImage image;
            image.width = side;
            image.height = bytes == "b" ? 300 : side;
            image.rgba.assign(static_cast<size_t>(image.width) * image.height * 4, 1);
            return image;
        });
    double now = 0;
    for (const char *id : {"a", "b", "c"})
        loader.get(id, 10, 10, ImageFit::Cover, now);
    for (const char *id : {"a", "b", "c"})
        loader.provide(id, id, now); // one worker: decoded in this order
    const size_t budget = 1000000;
    check(frames([] {}, [&] { return loader.pendingDecodes() == 0; }),
          "All three pictures finish decoding");
    size_t first = loader.pump(budget, now);
    // a: 400 x 400 x 4 = 640000 bytes; b: 500 x 300 x 4 = 600000, of which 360000 fit now.
    check(first == 640000 + 360000, "The first frame spends the whole budget");
    check(loader.get("a", 10, 10, ImageFit::Cover, now).texture != 0 &&
              loader.get("b", 10, 10, ImageFit::Cover, now).texture == 0,
          "The oldest request finishes first and the next one waits half-uploaded");
    check(store.updates.size() == 2 && store.updates[1].rows == 180,
          "A big picture is cut into strips of whole rows");
    size_t second = loader.pump(budget, now + 16);
    check(second == 240000 + 40000 && store.updates.size() == 4,
          "The next frame finishes b and then uploads c");
    check(loader.get("b", 10, 10, ImageFit::Cover, now).texture != 0 &&
              loader.get("c", 10, 10, ImageFit::Cover, now).texture != 0,
          "Both are ready after two frames");
    for (size_t i = 1; i < store.updates.size(); ++i)
        check(store.updates[i].texture >= store.updates[i - 1].texture,
              "Uploads happen in request order");
    check(loader.pump(budget, now + 32) == 0, "Nothing is left to upload");
    ImageLoader tiny(store, [](const std::string &) { return true; }, 1 << 20, 1, solid(50, 50));
    tiny.get("x", 50, 50, ImageFit::Cover, 0);
    tiny.provide("x", "x", 0);
    size_t spent = 0;
    check(frames([&] { spent += tiny.pump(10, 0); },
                 [&] { return tiny.residentBytes() > 0 && spent == 50 * 50 * 4; }),
          "Even a budget smaller than a row still makes progress");
}

void testLeastRecentlyUsedEviction() {
    StubStore store;
    int fetches = 0;
    // Room for two 100 x 100 pictures (40000 bytes each).
    ImageLoader loader(
        store,
        [&](const std::string &) {
            ++fetches;
            return true;
        },
        100000, 1, solid(100, 100));
    double now = 0;
    auto show = [&](const char *id) {
        return loader.get(id, 100, 100, ImageFit::Cover, now).texture;
    };
    auto load = [&](const char *id) {
        show(id);
        loader.provide(id, id, now);
        for (int i = 0; i < 400 && !show(id); ++i) {
            loader.pump(1 << 20, now += 16);
            show(id);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    load("a");
    load("b");
    check(show("a") && show("b") && store.alive() == 2 && fetches == 2,
          "Two pictures fit the budget");
    // c arrives while a and b are both still on screen: nothing may be evicted for it.
    show("c");
    loader.provide("c", "c", now);
    for (int i = 0; i < 100; ++i) {
        show("a");
        show("b");
        show("c");
        loader.pump(1 << 20, now += 16);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(show("a") && show("b") && !show("c") && store.destroyed == 0,
          "Pictures drawn this frame are never evicted; the newcomer waits");
    // Only b and c stay on screen: a is the least recently used and goes.
    for (int i = 0; i < 20; ++i) {
        show("b");
        show("c");
        loader.pump(1 << 20, now += 16);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(show("c") && show("b") && store.destroyed == 1 && loader.residentBytes() == 80000,
          "The least recently used picture makes room and the budget holds");
    int before = fetches;
    show("a");
    check(fetches == before + 1, "An evicted picture is fetched again when it is wanted");
}

// The loop pumps on every wake-up, also the idle ones that draw nothing. Pictures on screen
// must not turn evictable just because time passed.
void testIdlePumpsKeepVisiblePictures() {
    StubStore store;
    ImageLoader loader(
        store, [](const std::string &) { return true; }, 100000, 1, solid(100, 100));
    double now = 0;
    auto show = [&](const char *id) {
        return loader.get(id, 100, 100, ImageFit::Cover, now).texture;
    };
    for (const char *id : {"a", "b"}) {
        show(id);
        loader.provide(id, id, now);
        for (int i = 0; i < 400 && !show(id); ++i) {
            loader.pump(1 << 20, now += 16);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    check(show("a") && show("b"), "Two pictures fit the budget");
    show("c");
    loader.provide("c", "c", now);
    for (int i = 0; i < 100; ++i) {
        loader.pump(1 << 20, now += 50);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(show("a") && show("b") && !show("c") && store.destroyed == 0,
          "Idle pumps do not make the pictures on screen evictable");
}

void testFailureAndRetry() {
    StubStore store;
    int fetches = 0;
    bool answer = false;
    ImageLoader loader(
        store,
        [&](const std::string &) {
            ++fetches;
            return answer;
        },
        1 << 20, 1, solid(8, 8));
    check(loader.get("x", 8, 8, ImageFit::Cover, 0).texture == 0 && fetches == 1,
          "A fetch the app refuses leaves the placeholder");
    check(loader.get("x", 8, 8, ImageFit::Cover, 1).failed,
          "It is reported as failed, so a caller can stop waiting for it");
    loader.get("x", 8, 8, ImageFit::Cover, 29999);
    check(fetches == 1, "It is not retried at once");
    answer = true;
    loader.get("x", 8, 8, ImageFit::Cover, 30000);
    check(fetches == 2, "It is retried after 30 s");
    loader.fail("x", 31000);
    loader.get("x", 8, 8, ImageFit::Cover, 40000);
    check(fetches == 2, "An error report holds the retry back too");
    loader.get("x", 8, 8, ImageFit::Cover, 61000);
    check(fetches == 3, "and the retry comes 30 s after the report");
    ImageLoader broken(store, [](const std::string &) { return true; }, 1 << 20, 1);
    broken.get("bad", 8, 8, ImageFit::Cover, 0);
    broken.provide("bad", "definitely not a picture", 0);
    for (int i = 0; i < 200; ++i) {
        broken.pump(1 << 20, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(broken.get("bad", 8, 8, ImageFit::Cover, 1).texture == 0 && broken.residentBytes() == 0,
          "Undecodable bytes leave the placeholder");
}

void testShutdownWithBusyWorkers() {
    StubStore store;
    {
        ImageLoader loader(
            store, [](const std::string &) { return true; }, 1 << 20, 2,
            [](const std::string &, int, int, ImageFit) {
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
                DecodedImage image;
                image.width = image.height = 4;
                image.rgba.assign(64, 7);
                return image;
            });
        for (int i = 0; i < 6; ++i) {
            std::string id = "p" + std::to_string(i);
            loader.get(id, 4, 4, ImageFit::Cover, 0);
            loader.provide(id, "x", 0);
        }
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (loader.residentBytes() == 0 && std::chrono::steady_clock::now() < deadline) {
            loader.pump(1 << 20, 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    check(store.alive() == 0 && !store.textures.empty(),
          "Destroying the loader stops busy workers and frees every texture");
}
} // namespace

int main() {
    try {
        testDecode();
        testOffThreadAndFade();
        testUploadOrderAndBudget();
        testLeastRecentlyUsedEviction();
        testIdlePumpsKeepVisiblePictures();
        testFailureAndRetry();
        testShutdownWithBusyWorkers();
        std::printf("%d image loader checks passed\n", checks);
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
    return 0;
}
