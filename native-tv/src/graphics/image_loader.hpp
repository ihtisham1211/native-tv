#pragma once
#include "ui/scene.hpp"
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace nativetv {
enum class ImageFit {
    Cover,  // fills the box exactly, cropping the middle of the picture
    Contain // fits inside the box; the result is smaller in one direction
};
struct DecodedImage {
    int width = 0, height = 0;
    std::vector<uint8_t> rgba; // empty when decoding failed
};
// JPEG or PNG (SDL_image) resampled to the size it will be drawn at, so the GPU never
// minifies a large texture. Safe to call from any thread.
DecodedImage decodeImage(const std::string &bytes, int width, int height, ImageFit fit);

struct ImageView {
    TextureId texture = 0;     // 0: not ready, draw the placeholder
    int width = 0, height = 0; // texels of the texture, which Contain makes smaller than asked
    float fade = 0;            // 0 to 1 over kFadeMs from the frame the picture arrived
};
// Pictures keyed by (id, size) under a texture byte budget. Decoding runs on worker threads;
// the render thread only copies finished rows into textures, as many bytes per frame as
// pump() is allowed. Time is the caller's frame clock, so captures stay deterministic. Every
// call except the workers' own runs on the render thread.
class ImageLoader {
  public:
    using Decoder = std::function<DecodedImage(const std::string &, int, int, ImageFit)>;
    // Asks the app for the encoded bytes of `id`, to arrive through provide() or fail();
    // returns false if it cannot.
    using Fetch = std::function<bool(const std::string &id)>;

  private:
    struct Slot {
        enum class State { Fetching, Decoding, Uploading, Ready, Failed };
        Slot(const std::string &id, int width, int height, ImageFit fit)
            : id(id), width(width), height(height), fit(fit) {}
        const std::string id; // immutable: the workers read these three
        const int width, height;
        const ImageFit fit;
        State state = State::Fetching;
        TextureId texture = 0;
        int textureWidth = 0, textureHeight = 0, rowsUploaded = 0;
        size_t bytes = 0;
        unsigned usedTick = 0;
        double arrivedMs = 0, retryAtMs = 0;
        DecodedImage pixels;
    };
    struct Job {
        std::shared_ptr<Slot> slot;
        std::shared_ptr<const std::string> bytes;
    };
    struct Result {
        std::shared_ptr<Slot> slot;
        DecodedImage image;
    };
    TextureStore &store_;
    Fetch fetch_;
    Decoder decoder_;
    size_t budget_, resident_ = 0;
    unsigned tick_ = 0;
    std::map<std::string, std::vector<std::shared_ptr<Slot>>> slots_;
    std::deque<std::shared_ptr<Slot>> uploads_;
    mutable std::mutex mutex_;
    size_t inFlight_ = 0; // queued or running decodes
    std::condition_variable wake_;
    std::deque<Job> jobs_;
    std::vector<Result> finished_;
    bool stopping_ = false;
    std::vector<std::thread> workers_;
    void work();
    void request(Slot &slot, double nowMs);
    bool makeRoom(size_t bytes);

  public:
    static constexpr float kFadeMs = 220;
    static constexpr double kRetryMs = 30000;
    ImageLoader(TextureStore &store, Fetch fetch, size_t byteBudget = 40u << 20,
                unsigned workers = 2, Decoder decoder = decodeImage);
    ~ImageLoader();
    ImageLoader(const ImageLoader &) = delete;
    ImageLoader &operator=(const ImageLoader &) = delete;
    // The first call for a (id, width, height, fit) asks the app for the bytes. Pictures
    // drawn this frame or last are never evicted.
    ImageView get(const std::string &id, int width, int height, ImageFit fit, double nowMs);
    void provide(const std::string &id, std::string bytes, double nowMs);
    void fail(const std::string &id, double nowMs);
    // Call once per frame: takes finished decodes and copies up to `uploadBudgetBytes` of them
    // into textures, oldest request first, a big picture across several frames. Returns the
    // bytes copied.
    size_t pump(size_t uploadBudgetBytes, double nowMs);
    // True while a picture that arrived recently is still fading in, so the app keeps drawing.
    bool fading(double nowMs) const;
    size_t residentBytes() const {
        return resident_;
    }
    // Decodes still queued or running; the app keeps drawing frames while this is not zero.
    size_t pendingDecodes() const;
};
} // namespace nativetv
