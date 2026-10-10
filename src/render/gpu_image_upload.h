#pragma once

// A texture made from immutable RGBA8 pixels ahead of its first draw.
//
// WHY. Skia uploads a raster image the first time it is drawn on the GPU, on
// the drawing thread and under the Skia lock: for a 24 MP photo that is a
// 96 MB staging copy, a CPU mip chain when the draw is smoothed, and the GPU
// copy, all inside the frame that first shows it (tens to hundreds of ms).
// Pixels that are known to be wanted on screen soon — an ImageBitmap the page
// received, a big <img> whose decode just landed — start their upload here
// instead, when they arrive: the staging buffer is written on the uploader
// thread, the copy and the mip chain (GPU blits) are recorded into a command
// buffer of its own and submitted through VulkanQueue, and the first draw
// finds a texture.
//
// ORDERING, NOT WAITING. An upload is usable as soon as its copy has been
// SUBMITTED, not when it has completed: image() hands Skia the texture with
// every level in TRANSFER_DST_OPTIMAL, so the draw that samples it records
// the transition out of TRANSFER_DST, whose first scope is every transfer
// write submitted to the queue before it — the upload's. A draw never waits
// on the upload's ticket.
//
// A DRAW BEFORE THE SUBMISSION. While the staging buffer is still being
// written (a draw in the very frame the pixels arrived — the write takes
// ~7 ms for 24 MP), the windowed frame does not wait for it: a canvas holds
// its replay back to a later frame (CanvasScene::rasterize(mayDefer), the
// submission wakes the loop) and keeps showing what it showed. Where the
// pixels must be there now — getImageData, a snapshot, a headless capture,
// the UI painter on the raster thread — ensureSubmitted() decides: an upload
// no thread has started yet runs on the caller's thread right there (no worse
// than Skia's own upload, minus the CPU mips); one the uploader thread is
// writing is waited for (the rest of one memcpy). Either way the draw then
// samples the texture.
//
// LIFETIME. The texture (a SkiaImage, sampled through a Skia image borrowed
// once and cached) lives as long as the upload object; dropping the last
// reference retires it to the SkiaGpu, which destroys it once the queue has
// finished every submission made before that. The pixels' owner is released
// as soon as the staging copy has been made, and the staging buffer once the
// copy's ticket completes.

#include "render/renderer.h"  // SharedPixels

#include <include/core/SkImage.h>
#include <include/core/SkRefCnt.h>

#include <vulkan/vulkan.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>

class GrBackendTexture;

namespace bro::render {

class SkiaGpu;
struct SkiaImage;

class GpuImageUpload {
public:
    enum class State : uint8_t { Queued, Writing, Submitted, Failed };

    GpuImageUpload(const GpuImageUpload&) = delete;
    GpuImageUpload& operator=(const GpuImageUpload&) = delete;
    ~GpuImageUpload();

    int width() const { return width_; }
    int height() const { return height_; }
    uint64_t pixelsId() const { return pixelsId_; }
    State state() const { return state_.load(std::memory_order_acquire); }
    bool submitted() const { return state() == State::Submitted; }
    /// The queue ticket of the copy (0 until submitted).
    uint64_t ticket() const { return ticket_.load(std::memory_order_acquire); }

    /// Bring the upload to Submitted: run it on this thread if nothing has
    /// started it, else wait for the thread that has. False when it failed
    /// (the caller then draws its raster image). Must not be called with the
    /// Skia lock held by a thread the uploader could need — it never takes
    /// that lock, so holding it here is allowed.
    bool ensureSubmitted();

    /// The texture as a Skia image (all mip levels), made once; null before
    /// the submission, after a failure, or for another SkiaGpu. The caller
    /// holds `gpu`'s lock.
    sk_sp<SkImage> image(SkiaGpu& gpu);

private:
    friend class SkiaGpu;
    GpuImageUpload() = default;

    SkiaGpu* gpu_ = nullptr;
    SharedPixels px_;  // until the staging copy is made
    int width_ = 0;
    int height_ = 0;
    uint64_t pixelsId_ = 0;
    std::atomic<State> state_{State::Queued};
    std::mutex mu_;
    std::condition_variable cv_;

    // Set before state_ goes Submitted, read-only after.
    std::shared_ptr<SkiaImage> vkImage_;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkDeviceSize memoryOffset_ = 0;
    VkDeviceSize memorySize_ = 0;
    uint32_t levels_ = 1;
    std::atomic<uint64_t> ticket_{0};

    // Guarded by the Skia lock. One GrBackendTexture for the image's life:
    // it carries the layout Skia tracks, so a re-wrap would not start over
    // from TRANSFER_DST.
    std::unique_ptr<GrBackendTexture> backend_;
    sk_sp<SkImage> image_;
};

/// Pixels at least this many are uploaded ahead of their first draw; smaller
/// ones upload in the draw as before (cheap, and not worth a texture each).
inline constexpr int64_t kEagerUploadMinPixels = 512 * 512;

}  // namespace bro::render
