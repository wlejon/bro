#pragma once

#include <include/core/SkImage.h>
#include <include/core/SkRefCnt.h>

#include <mutex>

namespace bro::engine {

/// The last frame a sub-document (iframe, secondary window) rasterized, handed
/// from the thread that replays it to the thread that composites it.
///
/// The raster thread replays into a surface it owns and publishes an
/// immutable snapshot; the compositor only ever reads snapshots, so it never
/// touches a surface while it is being drawn or resized. (A raster snapshot
/// shares the surface's pixels until the next replay draws, when Skia copies
/// them because the snapshot is still referenced.)
class PublishedFrame {
public:
    void publish(sk_sp<SkImage> image) {
        std::lock_guard<std::mutex> lock(mutex_);
        image_ = std::move(image);
    }
    sk_sp<SkImage> get() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return image_;
    }
    void clear() { publish(nullptr); }

private:
    mutable std::mutex mutex_;
    sk_sp<SkImage> image_;
};

} // namespace bro::engine
