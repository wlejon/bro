#include "render/image_cache.h"

#include "util/log.h"

#include <include/codec/SkCodec.h>
#include <include/core/SkBitmap.h>
#include <include/core/SkData.h>
#include <include/core/SkImageInfo.h>

#include "broimage/decode.h"

namespace bro::render {

namespace {
// A cached image survives this many frames without being drawn before it is
// evicted. Generous on purpose: it absorbs brief off-screen gaps (and any
// renderer that happens to call beginFrame more than once per frame) without
// thrashing the decode path, while still reclaiming memory within ~1s at 60fps.
constexpr uint64_t kEvictAfterFrames = 60;
} // namespace

sk_sp<SkImage> decodeImageBytes(const void* data, size_t len) {
    if (!data || len == 0) return nullptr;

    // Skia owns its own copy of the bytes: the cached SkImage may outlive the
    // caller's buffer (the command-buffer arena is recycled every frame).
    sk_sp<SkData> skData = SkData::MakeWithCopy(data, len);

    // Try Skia's built-in codecs first (fast path when available).
    if (auto codec = SkCodec::MakeFromData(skData)) {
        auto [image, result] = codec->getImage();
        if (image) return image;
    }

    // Fallback: decode via broimage (stb-backed). Our Skia build may not link
    // PNG/JPEG codecs. Heap-allocated so the pixel buffer outlives this scope —
    // SkBitmap::installPixels references the bytes zero-copy, and the release
    // callback frees the Image when Skia is done with it.
    auto* img = new broimage::Image();
    std::string err;
    if (!broimage::decode_memory(static_cast<const uint8_t*>(data), len, *img, &err)) {
        LOG_WARN("decodeImageBytes: decode failed (len=%zu): %s", len, err.c_str());
        delete img;
        return nullptr;
    }

    SkImageInfo info = SkImageInfo::Make(img->width, img->height,
                                         kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
    SkBitmap bmp;
    if (!bmp.installPixels(info, img->pixels.data(), img->width * 4,
            [](void*, void* ctx) {
                delete static_cast<broimage::Image*>(ctx);
            }, img)) {
        delete img;
        return nullptr;
    }
    bmp.setImmutable();
    return bmp.asImage();
}

sk_sp<SkImage> DecodedImageCache::resolve(uint64_t id, const void* data, size_t len) {
    // id == 0 marks an uncacheable source (e.g. a negative-cached miss): always
    // decode fresh so a later frame with valid bytes is not poisoned.
    if (id == 0) return decodeImageBytes(data, len);

    auto it = entries_.find(id);
    if (it != entries_.end()) {
        it->second.lastFrame = frame_;
        return it->second.image;
    }

    sk_sp<SkImage> img = decodeImageBytes(data, len);
    // Only retain successful decodes; a null result is left uncached so a
    // subsequent frame can retry rather than serving a permanent blank.
    if (img) entries_[id] = Entry{img, frame_};
    return img;
}

void DecodedImageCache::beginFrame() {
    ++frame_;
    if (frame_ <= kEvictAfterFrames) return;
    const uint64_t cutoff = frame_ - kEvictAfterFrames;
    for (auto it = entries_.begin(); it != entries_.end(); ) {
        if (it->second.lastFrame < cutoff) it = entries_.erase(it);
        else ++it;
    }
}

} // namespace bro::render
