#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>

#include <include/core/SkImage.h>
#include <include/core/SkRefCnt.h>

namespace bro::render {

// Decode encoded image bytes (PNG/JPEG/etc) into an SkImage. Tries Skia's
// built-in codecs first, then falls back to stb_image when the Skia build
// lacks the codec. Returns nullptr on failure.
sk_sp<SkImage> decodeImageBytes(const void* data, size_t len);

// Per-renderer cache of decoded SkImages, keyed by a stable, process-unique
// image id assigned by DrawTraversal (one id per cached source URL).
//
// Without this, the replay path re-decodes every <img> and background-image
// from its encoded bytes on every frame — catastrophic for animated pages and
// for tiled backgrounds, which issue one drawImage per tile.
//
// A cache lives inside a single renderer and is only touched on that renderer's
// draw thread, so it needs no locking. Entries untouched for kEvictAfterFrames
// consecutive frames are dropped, bounding memory to the recent working set and
// releasing images stranded by a document reload.
class DecodedImageCache {
public:
    // Resolve `id` to a decoded image. On a miss — or when id == 0, meaning
    // "not cacheable" — the bytes are decoded fresh; id != 0 results are
    // retained for reuse. Returns nullptr if decoding fails.
    sk_sp<SkImage> resolve(uint64_t id, const void* data, size_t len);

    // Advance the frame clock and evict entries not used recently. Call once
    // per frame from the owning renderer's beginFrame().
    void beginFrame();

    void clear() { entries_.clear(); }

private:
    struct Entry {
        sk_sp<SkImage> image;
        uint64_t lastFrame = 0;
    };
    std::unordered_map<uint64_t, Entry> entries_;
    uint64_t frame_ = 0;
};

} // namespace bro::render
