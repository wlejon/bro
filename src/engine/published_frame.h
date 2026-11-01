#pragma once

#include "render/skia_image.h"

#include <mutex>

namespace bro::engine {

/// The last frame a sub-document (iframe, secondary window) rasterized, handed
/// from the thread that replays it to the thread that composites it.
///
/// It is the surface's GPU image, published only once the work that drew it
/// is submitted; the raster thread then draws the next frame into its other
/// surface, so the image the compositor holds is never redrawn underneath it.
class PublishedFrame {
public:
    void publish(render::SkiaImageRef image) {
        std::lock_guard<std::mutex> lock(mutex_);
        gpu_ = std::move(image);
        ++generation_;
    }
    /// Bumped by every publish (and clear): the compositor tells a new frame
    /// from the one it last showed by it, whatever image it lands in.
    uint64_t generation() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return generation_;
    }
    /// The GPU image (null before the first frame).
    render::SkiaImageRef gpu() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return gpu_;
    }
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        gpu_.reset();
        ++generation_;
    }

private:
    mutable std::mutex mutex_;
    uint64_t generation_ = 0;
    render::SkiaImageRef gpu_;
};

/// What a sub-document's command buffer was last recorded under, so a frame
/// that changed nothing in it (its document clean, its canvases idle, the
/// same box and hover) neither re-records nor re-replays it, and the frame
/// it published stands. Written by the main thread at record time and read
/// by the raster thread at replay, ordered by the frame handshake.
struct SubDocRecord {
    int boxW = -1, boxH = -1;
    const void* hovered = nullptr;
    bool needsReplay = true;
};

} // namespace bro::engine
