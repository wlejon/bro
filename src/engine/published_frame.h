#pragma once

#include "render/skia_gpu.h"

#include <include/core/SkImage.h>
#include <include/core/SkRefCnt.h>

#include <mutex>

namespace bro::engine {

/// The last frame a sub-document (iframe, secondary window) rasterized, handed
/// from the thread that replays it to the thread that composites it.
///
/// On the CPU it is an immutable snapshot of the surface the raster thread
/// drew (shares the pixels until the next replay draws, when Skia copies them
/// because the snapshot is still referenced). On the GPU it is the surface's
/// image, published only once the work that drew it is submitted; the raster
/// thread then draws the next frame into its other surface, so the image the
/// compositor holds is never redrawn underneath it.
class PublishedFrame {
public:
    void publish(sk_sp<SkImage> image) {
        std::lock_guard<std::mutex> lock(mutex_);
        image_ = std::move(image);
        gpu_.reset();
    }
    void publish(render::SkiaImageRef image) {
        std::lock_guard<std::mutex> lock(mutex_);
        gpu_ = std::move(image);
        image_.reset();
    }
    /// The CPU snapshot (null for a GPU frame).
    sk_sp<SkImage> get() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return image_;
    }
    /// The GPU image (null for a CPU frame).
    render::SkiaImageRef gpu() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return gpu_;
    }
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        image_.reset();
        gpu_.reset();
    }

private:
    mutable std::mutex mutex_;
    sk_sp<SkImage> image_;
    render::SkiaImageRef gpu_;
};

} // namespace bro::engine
