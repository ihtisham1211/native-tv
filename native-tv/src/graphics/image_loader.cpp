#include "graphics/image_loader.hpp"
#include <SDL.h>
#include <SDL_image.h>
#include <algorithm>
#include <cmath>

namespace nativetv {
namespace {
// For every output pixel, the source pixels that feed it and their weights. Shrinking
// averages the covered area; enlarging blends the two nearest pixels.
struct Axis {
    std::vector<int> first, count;
    std::vector<size_t> offset;
    std::vector<float> weights;
    Axis(double start, double length, int outCount, int inCount) {
        double scale = length / outCount;
        int last = inCount - 1;
        for (int i = 0; i < outCount; ++i) {
            int low = 0;
            std::vector<float> taps;
            if (scale >= 1) {
                double from = start + i * scale, to = from + scale;
                low = std::max(0, static_cast<int>(std::floor(from)));
                int high = std::min(last, static_cast<int>(std::ceil(to)) - 1);
                for (int j = low; j <= high; ++j)
                    taps.push_back(static_cast<float>(
                        (std::min<double>(j + 1, to) - std::max<double>(j, from)) / scale));
            } else {
                double centre = start + (i + 0.5) * scale - 0.5;
                int base = static_cast<int>(std::floor(centre));
                float fraction = static_cast<float>(centre - base);
                low = std::min(last, std::max(0, base));
                if (std::min(last, std::max(0, base + 1)) == low)
                    taps.push_back(1);
                else
                    taps = {1 - fraction, fraction};
            }
            if (taps.empty()) {
                low = std::min(last, std::max(0, low));
                taps.push_back(1);
            }
            float total = 0;
            for (float tap : taps)
                total += tap;
            for (float &tap : taps)
                tap /= total;
            first.push_back(low);
            count.push_back(static_cast<int>(taps.size()));
            offset.push_back(weights.size());
            weights.insert(weights.end(), taps.begin(), taps.end());
        }
    }
};
} // namespace

DecodedImage decodeImage(const std::string &bytes, int width, int height, ImageFit fit) {
    DecodedImage result;
    if (bytes.empty() || bytes.size() > (16u << 20) || width <= 0 || height <= 0 || width > 4096 ||
        height > 4096)
        return result;
    SDL_RWops *stream = SDL_RWFromConstMem(bytes.data(), static_cast<int>(bytes.size()));
    if (!stream)
        return result;
    SDL_Surface *loaded = IMG_Load_RW(stream, 1);
    if (!loaded)
        return result;
    SDL_Surface *surface = SDL_ConvertSurfaceFormat(loaded, SDL_PIXELFORMAT_RGBA32, 0);
    SDL_FreeSurface(loaded);
    if (!surface)
        return result;
    int sourceWidth = surface->w, sourceHeight = surface->h;
    if (sourceWidth <= 0 || sourceHeight <= 0 ||
        static_cast<int64_t>(sourceWidth) * sourceHeight > (64 << 20)) {
        SDL_FreeSurface(surface);
        return result;
    }
    double cropX = 0, cropY = 0, cropWidth = sourceWidth, cropHeight = sourceHeight;
    int outWidth = width, outHeight = height;
    if (fit == ImageFit::Cover) {
        double wanted = static_cast<double>(width) / height,
               actual = static_cast<double>(sourceWidth) / sourceHeight;
        if (actual > wanted) {
            cropWidth = sourceHeight * wanted;
            cropX = (sourceWidth - cropWidth) / 2;
        } else {
            cropHeight = sourceWidth / wanted;
            cropY = (sourceHeight - cropHeight) / 2;
        }
    } else {
        double scale = std::min(static_cast<double>(width) / sourceWidth,
                                static_cast<double>(height) / sourceHeight);
        outWidth = std::max(1, static_cast<int>(std::lround(sourceWidth * scale)));
        outHeight = std::max(1, static_cast<int>(std::lround(sourceHeight * scale)));
    }
    Axis across(cropX, cropWidth, outWidth, sourceWidth),
        down(cropY, cropHeight, outHeight, sourceHeight);
    // Horizontal pass over just the rows the vertical pass reads, in premultiplied alpha so
    // transparent pixels do not bleed their colour into the edge.
    int firstRow = down.first.front(), lastRow = firstRow;
    for (int i = 0; i < outHeight; ++i) {
        firstRow = std::min(firstRow, down.first[i]);
        lastRow = std::max(lastRow, down.first[i] + down.count[i] - 1);
    }
    int rows = lastRow - firstRow + 1;
    std::vector<uint8_t> middle(static_cast<size_t>(outWidth) * rows * 4);
    const uint8_t *pixels = static_cast<const uint8_t *>(surface->pixels);
    for (int row = 0; row < rows; ++row) {
        const uint8_t *line = pixels + static_cast<size_t>(firstRow + row) * surface->pitch;
        for (int x = 0; x < outWidth; ++x) {
            float sum[4] = {0, 0, 0, 0};
            const float *weight = &across.weights[across.offset[x]];
            for (int k = 0; k < across.count[x]; ++k) {
                const uint8_t *pixel = line + (across.first[x] + k) * 4;
                float alpha = pixel[3] / 255.0f * weight[k];
                sum[0] += pixel[0] * alpha;
                sum[1] += pixel[1] * alpha;
                sum[2] += pixel[2] * alpha;
                sum[3] += 255.0f * alpha;
            }
            uint8_t *out = &middle[(static_cast<size_t>(row) * outWidth + x) * 4];
            for (int c = 0; c < 4; ++c)
                out[c] = static_cast<uint8_t>(std::min(255.0f, sum[c] + 0.5f));
        }
    }
    SDL_FreeSurface(surface);
    result.width = outWidth;
    result.height = outHeight;
    result.rgba.resize(static_cast<size_t>(outWidth) * outHeight * 4);
    for (int y = 0; y < outHeight; ++y) {
        const float *weight = &down.weights[down.offset[y]];
        for (int x = 0; x < outWidth; ++x) {
            float sum[4] = {0, 0, 0, 0};
            for (int k = 0; k < down.count[y]; ++k) {
                const uint8_t *pixel =
                    &middle[(static_cast<size_t>(down.first[y] - firstRow + k) * outWidth + x) * 4];
                for (int c = 0; c < 4; ++c)
                    sum[c] += pixel[c] * weight[k];
            }
            uint8_t *out = &result.rgba[(static_cast<size_t>(y) * outWidth + x) * 4];
            float alpha = sum[3];
            for (int c = 0; c < 3; ++c)
                out[c] = static_cast<uint8_t>(
                    alpha > 0 ? std::min(255.0f, sum[c] * 255.0f / alpha + 0.5f) : 0);
            out[3] = static_cast<uint8_t>(std::min(255.0f, alpha + 0.5f));
        }
    }
    // Resampling an opaque picture can leave 254 for a pixel; a see-through one has far less.
    result.opaque = true;
    for (size_t i = 3; i < result.rgba.size() && result.opaque; i += 4)
        result.opaque = result.rgba[i] >= 250;
    return result;
}

ImageLoader::ImageLoader(TextureStore &store, Fetch fetch, size_t byteBudget, unsigned workers,
                         Decoder decoder)
    : store_(store), fetch_(std::move(fetch)), decoder_(std::move(decoder)), budget_(byteBudget) {
    // Loading the codecs lazily from two workers at once would race.
    IMG_Init(IMG_INIT_JPG | IMG_INIT_PNG);
    for (unsigned i = 0; i < std::max(1u, workers); ++i)
        workers_.emplace_back(&ImageLoader::work, this);
}
ImageLoader::~ImageLoader() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    for (auto &worker : workers_)
        worker.join();
    for (auto &entry : slots_)
        for (auto &slot : entry.second)
            if (slot->texture)
                store_.destroy(slot->texture);
}
void ImageLoader::work() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
            if (stopping_)
                return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        DecodedImage image = decoder_(*job.bytes, job.slot->width, job.slot->height, job.slot->fit);
        std::lock_guard<std::mutex> lock(mutex_);
        finished_.push_back({std::move(job.slot), std::move(image)});
        --inFlight_;
    }
}
void ImageLoader::request(Slot &slot, double nowMs) {
    slot.state = Slot::State::Fetching;
    slot.retryAtMs = nowMs + kRetryMs; // also covers an app that never answers
    if (fetch_ && !fetch_(slot.id))
        slot.state = Slot::State::Failed;
}
ImageView ImageLoader::get(const std::string &id, int width, int height, ImageFit fit,
                           double nowMs) {
    auto &sizes = slots_[id];
    std::shared_ptr<Slot> slot;
    for (auto &candidate : sizes)
        if (candidate->width == width && candidate->height == height && candidate->fit == fit)
            slot = candidate;
    if (!slot) {
        slot = std::make_shared<Slot>(id, width, height, fit);
        sizes.push_back(slot);
        request(*slot, nowMs);
    }
    slot->usedTick = tick_;
    requested_ = true;
    if ((slot->state == Slot::State::Failed || slot->state == Slot::State::Fetching) &&
        nowMs >= slot->retryAtMs)
        request(*slot, nowMs);
    ImageView view;
    if (slot->state == Slot::State::Ready) {
        view.texture = slot->texture;
        view.width = slot->textureWidth;
        view.height = slot->textureHeight;
        view.fade = std::min(1.0f, static_cast<float>(nowMs - slot->arrivedMs) / kFadeMs);
        view.opaque = slot->opaque;
    }
    view.failed = slot->state == Slot::State::Failed;
    return view;
}
void ImageLoader::provide(const std::string &id, std::string bytes, double nowMs) {
    auto found = slots_.find(id);
    if (found == slots_.end())
        return;
    if (bytes.empty()) {
        fail(id, nowMs);
        return;
    }
    auto shared = std::make_shared<const std::string>(std::move(bytes));
    for (auto &slot : found->second)
        if (slot->state == Slot::State::Fetching) {
            slot->state = Slot::State::Decoding;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                jobs_.push_back({slot, shared});
                ++inFlight_;
            }
            wake_.notify_one();
        }
}
void ImageLoader::fail(const std::string &id, double nowMs) {
    auto found = slots_.find(id);
    if (found == slots_.end())
        return;
    for (auto &slot : found->second)
        if (slot->state == Slot::State::Fetching) {
            slot->state = Slot::State::Failed;
            slot->retryAtMs = nowMs + kRetryMs;
        }
}
// Evicts the least recently drawn pictures until `bytes` more fit.
bool ImageLoader::makeRoom(size_t bytes) {
    while (resident_ + bytes > budget_) {
        std::shared_ptr<Slot> oldest;
        for (auto &entry : slots_)
            for (auto &slot : entry.second)
                if (slot->state == Slot::State::Ready && slot->usedTick + 1 < tick_ &&
                    (!oldest || slot->usedTick < oldest->usedTick))
                    oldest = slot;
        if (!oldest)
            return false;
        store_.destroy(oldest->texture);
        resident_ -= oldest->bytes;
        auto &sizes = slots_[oldest->id];
        sizes.erase(std::find(sizes.begin(), sizes.end(), oldest));
        if (sizes.empty())
            slots_.erase(oldest->id);
    }
    return true;
}
size_t ImageLoader::pump(size_t uploadBudgetBytes, double nowMs) {
    if (requested_) {
        ++tick_;
        requested_ = false;
    }
    std::vector<Result> arrived;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        arrived.swap(finished_);
    }
    for (auto &result : arrived) {
        Slot &slot = *result.slot;
        if (result.image.rgba.empty() || result.image.rgba.size() > budget_) {
            slot.state = Slot::State::Failed;
            slot.retryAtMs = nowMs + kRetryMs;
            continue;
        }
        slot.pixels = std::move(result.image);
        slot.textureWidth = slot.pixels.width;
        slot.textureHeight = slot.pixels.height;
        slot.opaque = slot.pixels.opaque;
        slot.state = Slot::State::Uploading;
        uploads_.push_back(result.slot);
    }
    size_t spent = 0;
    while (!uploads_.empty() && spent < uploadBudgetBytes) {
        Slot &slot = *uploads_.front();
        size_t rowBytes = static_cast<size_t>(slot.textureWidth) * 4;
        if (!slot.texture) {
            size_t bytes = rowBytes * slot.textureHeight;
            if (!makeRoom(bytes))
                break;
            slot.texture =
                store_.create(slot.textureWidth, slot.textureHeight, TextureFormat::Rgba8);
            if (!slot.texture) {
                slot.state = Slot::State::Failed;
                slot.retryAtMs = nowMs + kRetryMs;
                uploads_.pop_front();
                continue;
            }
            slot.bytes = bytes;
            resident_ += bytes;
        }
        int rows = std::min<int>(
            slot.textureHeight - slot.rowsUploaded,
            static_cast<int>(std::max<size_t>(1, (uploadBudgetBytes - spent) / rowBytes)));
        store_.update(slot.texture, 0, slot.rowsUploaded, slot.textureWidth, rows,
                      slot.pixels.rgba.data() + static_cast<size_t>(slot.rowsUploaded) * rowBytes);
        slot.rowsUploaded += rows;
        spent += static_cast<size_t>(rows) * rowBytes;
        if (slot.rowsUploaded == slot.textureHeight) {
            slot.state = Slot::State::Ready;
            slot.arrivedMs = nowMs;
            std::vector<uint8_t>().swap(slot.pixels.rgba);
            uploads_.pop_front();
        }
    }
    return spent;
}
size_t ImageLoader::pendingDecodes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return inFlight_;
}
bool ImageLoader::fading(double nowMs) const {
    for (const auto &entry : slots_)
        for (const auto &slot : entry.second)
            if (slot->state == Slot::State::Ready && slot->usedTick + 1 >= tick_ &&
                nowMs - slot->arrivedMs < kFadeMs)
                return true;
    return false;
}
} // namespace nativetv
