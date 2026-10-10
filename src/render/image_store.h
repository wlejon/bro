#pragma once

// The decoded-image store: every picture an <img>, a CSS background, a
// border-image or createImageBitmap(blob) shows is decoded here, OFF the page
// thread, once per source, and kept in one process-wide cache bounded by
// memory.
//
// WHY ONE STORE. A photo decodes in tens of milliseconds (an 8 MP JPEG ~45 ms)
// and a decode on the page thread is a frame the page cannot answer input in.
// Before this, `img.src = x` decoded synchronously, the paint path decoded the
// same bytes again on its own thread, and a second element given the same src
// decoded it a third time. Here a src names a request: the first asker starts
// the decode on a decoder thread, every later asker (another element, the
// painter, a recycled gallery cell) gets the same request, and once it is
// ready it is ready for all of them — synchronously, like a browser's list of
// available images.
//
// THREADING. The store and its requests are safe from any thread. The work
// (read the file, decode, orient) runs on the store's decoder threads; the
// page side learns of a settled request by polling it (layout/image_loading,
// once a frame) — a settle bumps settledCount() and wakes the main loop
// (util::wakeMainLoop), and nothing here ever calls back into script.
//
// MEMORY. Ready entries count their pixel bytes against a budget (default
// 512 MB, BRO_IMAGE_CACHE_MB to change it); past it the least recently used
// ready entries leave the cache. Leaving the cache frees nothing still in
// use: an element (or a recording) holding the request keeps its pixels, and
// they go when the last holder lets go. A pending request is never evicted.

#include "render/renderer.h"  // SharedPixels

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace bro::render {

// One decoded picture.
struct DecodedImage {
    // Natural size: of the pixels as stored, i.e. after the EXIF orientation
    // was applied when `oriented` (90-degree turns swap the axes).
    int width = 0;
    int height = 0;
    // Straight-alpha RGBA8, top-down, width*height*4. For an SVG, the markup
    // rasterized at its intrinsic size (empty when it has none) — what a
    // canvas or WebGL consumer draws; the painter draws `svgMarkup` instead.
    std::vector<uint8_t> rgba;
    bool isSvg = false;
    std::string svgMarkup;
    // The source's EXIF orientation (1..8, 1 = upright as stored), and
    // whether it was applied to `rgba`.
    int orientation = 1;
    bool oriented = false;
    // Process-unique id for these pixels (SharedPixels::id).
    uint64_t id = 0;

    size_t bytes() const { return rgba.size() + svgMarkup.size(); }
};

// A process-unique id for a new buffer of pixels (DecodedImage::id): what an
// ImageBitmap's pixels made outside the store are named by.
uint64_t newPixelsId();

// The pixels of `img` as SharedPixels, kept alive by the image itself: a
// recording or a texture upload may outlive the caller's reference.
SharedPixels sharedPixelsOf(const std::shared_ptr<const DecodedImage>& img);

// Decode encoded bytes into `out`: SVG (markup kept, rasterized at its
// intrinsic size), WebP, then Skia's codecs, then broimage (stb). When
// `orient` is set a JPEG's EXIF orientation is applied. Any thread.
bool decodeImageData(const uint8_t* bytes, size_t len, bool orient, DecodedImage& out,
                     std::string& err);

// Natural size from the header alone, without decoding: cheap enough for the
// page thread. `orientation` reports the EXIF orientation (and the size is
// the oriented one when `orient`); `isSvg` an SVG, whose size is its
// intrinsic one (0 when it has none). False for bytes nothing recognises.
bool probeImageData(const uint8_t* bytes, size_t len, bool orient, int& width, int& height,
                    int& orientation, bool& isSvg);

class ImageRequest {
public:
    enum class State : uint8_t { Pending, Ready, Failed };

    State state() const { return state_.load(std::memory_order_acquire); }
    bool settled() const { return state() != State::Pending; }
    bool ready() const { return state() == State::Ready; }
    // The picture once Ready, else null.
    std::shared_ptr<const DecodedImage> image() const {
        return ready() ? image_ : nullptr;
    }
    // Why it failed (after Failed).
    const std::string& error() const { return error_; }
    const std::string& key() const { return key_; }
    // Block until settled or `timeoutMs` passes; true when settled. For
    // headless runs, which settle loads deterministically.
    bool wait(double timeoutMs) const;

private:
    friend class ImageStore;
    std::string key_;
    std::atomic<State> state_{State::Pending};
    std::shared_ptr<const DecodedImage> image_;  // written once, before state_ goes Ready
    std::string error_;                          // written once, before state_ goes Failed
    mutable std::mutex mu_;
    mutable std::condition_variable cv_;
};

// The work a request runs on a decoder thread: produce the picture or say why
// not. It must not touch the page (no DOM, no script).
using ImageWork = std::function<bool(DecodedImage& out, std::string& err)>;

class ImageStore {
public:
    static ImageStore& instance();

    // The request for `key`: the cached one (pending, ready or failed), else
    // a new one whose `work` is queued for a decoder thread.
    std::shared_ptr<ImageRequest> request(const std::string& key, ImageWork work);
    // The cached request for `key`, or null. Never starts work.
    std::shared_ptr<ImageRequest> find(const std::string& key);
    // A request outside the cache (createImageBitmap(blob): bytes with no URL).
    std::shared_ptr<ImageRequest> submit(ImageWork work);

    // Bumped on every settle; the page compares it with what it last saw.
    uint64_t settledCount() const { return settled_.load(std::memory_order_acquire); }
    // Requests queued or decoding.
    size_t pendingCount() const;
    // Block until nothing is queued or decoding, or `timeoutMs` passes.
    bool waitIdle(double timeoutMs);

    size_t budgetBytes() const;
    void setBudgetBytes(size_t bytes);
    size_t cachedBytes() const;
    size_t cachedCount() const;

    ~ImageStore();

private:
    ImageStore();
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<uint64_t> settled_{0};
    void run(const std::shared_ptr<ImageRequest>& req, ImageWork work);
};

}  // namespace bro::render
