#pragma once

#include "render/layer_source.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#if defined(__linux__)
#include <brodmabuf/kms.h>
#include <brodmabuf/buffer.h>
#include <brodmabuf/gbm.h>
#include <brodmabuf/vulkan.h>
#include <vulkan/vulkan.h>
#endif

#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace bro::render {

class VulkanContext;
class VulkanPresenter;
struct PresentFrame;

struct KmsScanoutSlot {
#if defined(__linux__)
    std::unique_ptr<brodmabuf::GbmBuffer> gbm;
    std::unique_ptr<brodmabuf::KmsFramebuffer> fb;
    std::unique_ptr<brodmabuf::VulkanImage> vkImage;
    VkImageView vkView = VK_NULL_HANDLE;
    brodmabuf::DmaBufAttributes dmabuf;  // the exported planes (the fds the slot keeps open)
#endif
    uint32_t width = 0;
    uint32_t height = 0;
};

/// A composited frame as it went to scanout, for a consumer that reads the
/// scanout buffer itself (bro.remote's encoder). The fds belong to the
/// presenter; a consumer that keeps reading after the listener returns
/// takes a hold on the slot (KmsDirectPresenter::holdSlot) and keeps it
/// until it is done.
struct KmsScanoutFrame {
    size_t slot = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t drmFormat = 0;
    uint64_t modifier = 0;
    uint32_t planeCount = 0;
    int fds[4] = {-1, -1, -1, -1};
    uint32_t offsets[4] = {};
    uint32_t strides[4] = {};
};

class KmsDirectPresenter {
public:
    KmsDirectPresenter() = default;
    ~KmsDirectPresenter();

    KmsDirectPresenter(const KmsDirectPresenter&) = delete;
    KmsDirectPresenter& operator=(const KmsDirectPresenter&) = delete;

    /// Initialize KMS pipeline with DRM device file descriptor
    bool init(int drmFd);

    /// Initialize scanout framebuffers for composited presentation
    bool initScanoutBuffers(VulkanContext& ctx, uint32_t count = 2);

    /// Check if a client DMA-BUF buffer can be directly scanned out without GPU compositing.
    /// Conditions:
    /// 1. KMS presenter is valid and active.
    /// 2. The layer destination matches output CRTC bounds exactly (no scaling, no rotation, at (0,0)).
    /// 3. The layer is not clipped or obscured by surrounding UI.
    bool canDirectScanout(const DmabufLayerSource& src, const LayerQuad& quad,
                          uint32_t crtcWidth, uint32_t crtcHeight) const;

    /// Present a client DMA-BUF directly via KMS atomic modesetting.
    bool directScanout(const DmabufLayerSource& src, int inFenceFd = -1, int* outFenceFd = nullptr);

    /// Present a composited frame rendered into a scanout buffer via atomic modesetting.
    bool presentComposited(VulkanContext& ctx, VulkanPresenter& presenter,
                           const PresentFrame& frame, int inFenceFd = -1, int* outFenceFd = nullptr);

    /// The last composited frame as it went to scanout, as tightly packed
    /// RGBA8 (alpha forced opaque: the scanout format is XRGB). Reads the
    /// scanout buffer itself, so it is exactly what the display was given.
    /// Call on the thread that presents (the engine thread): the slot read is
    /// the one the next-but-one present reuses. False — with `why` — before
    /// the first composited present, or while a client buffer is scanned out
    /// directly (the composited slot is then not what is on screen).
    bool readLastFrame(std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height,
                       std::string* why = nullptr);

    /// Process page-flip events via drmHandleEvent (vblank sync)
    bool handlePageFlipEvent(int timeoutMs = 100);

    /// The last flip the kernel reported complete: the vblank it landed on
    /// (CLOCK_MONOTONIC ms, the clock util::currentTimeMs reads) and the
    /// CRTC's vblank counter; `count` goes up by one per flip seen.
    struct FlipInfo {
        double vblankMs = 0.0;
        uint32_t sequence = 0;
        uint64_t count = 0;
    };
    const FlipInfo& lastFlip() const { return lastFlip_; }
    /// A flip has been committed and its completion event not read yet.
    /// Neither present waits for its own flip: the frame loop waits for it
    /// (polling pollFd() with its input), and the next present waits for it
    /// before it draws or commits.
    bool flipPending() const { return flipPending_; }
    /// Reads DRM events until the pending flip lands, for up to timeoutMs;
    /// true when no flip is pending any more.
    bool waitForFlip(int timeoutMs);
    /// The DRM fd flip events arrive on (readable when one has), or -1.
    int pollFd() const;
    /// Called on the presenting thread as each flip lands (after the frame
    /// tap's completed()). Null to remove.
    using FlipListener = std::function<void(const FlipInfo&)>;
    void setFlipListener(FlipListener listener) { flipListener_ = std::move(listener); }
    /// The mode's refresh period, ms (0 before init).
    double refreshPeriodMs() const;

    /// Where the last presentComposited spent its wait: on the composite's
    /// GPU work, and on the previous flip landing plus the commit.
    struct PresentTiming {
        double gpuWaitMs = 0.0;
        double flipWaitMs = 0.0;
    };
    const PresentTiming& lastPresentTiming() const { return lastTiming_; }

    /// A consumer that copies each composited frame out (the screen
    /// recorder): record() adds its commands to the frame's own command
    /// buffer, after the composite (the image is in GENERAL layout, and is
    /// a transfer source); completed() runs once that work is done and the
    /// frame is on screen. One tap at a time; null to remove.
    class FrameTap {
    public:
        virtual ~FrameTap() = default;
        virtual void record(VkCommandBuffer cmd, VkImage image, uint32_t width, uint32_t height) = 0;
        virtual void completed(const FlipInfo& flip) = 0;
    };
    void setFrameTap(FrameTap* tap) { frameTap_ = tap; }

    /// Restore KMS modeset after VT switch resume
    bool restoreModeset();

    /// Pause presentation during VT switch away
    void pause();

    bool isActive() const { return active_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    void close();

    /// Called on the presenting thread for each composited frame as soon as
    /// its GPU work is done, just before it is flipped to the screen (the
    /// content is complete: no acquire fence is needed; a reader that keeps
    /// it past the call holds the slot, holdSlot). Empty to stop.
    using ScanoutListener = std::function<void(const KmsScanoutFrame&)>;
    void setScanoutListener(ScanoutListener listener) { scanoutListener_ = std::move(listener); }

    /// Keeps `slot` from being rendered into until the returned release is
    /// called (exactly once, from any thread; it stays safe to call after the
    /// presenter is gone). presentComposited waits for a held slot before
    /// drawing into it — a bounded wait (kHoldWaitMs), after which it draws
    /// anyway and logs, because a stalled screen is worse than one torn
    /// frame in a stream. close() waits the same way.
    std::function<void()> holdSlot(size_t slot);
    static constexpr int kHoldWaitMs = 200;

    /// While set, canDirectScanout answers false, so a fullscreen client is
    /// composited into a scanout slot (where the scanout listener sees it)
    /// rather than flipped to directly.
    void setDirectScanoutInhibited(bool inhibited) { directScanoutInhibited_ = inhibited; }

    // --- The hardware cursor: the CRTC's cursor plane ---
    //
    // The pointer drawn by the display rather than into the frame, so moving
    // it redraws nothing and a client scanned out directly stays so. Every
    // present carries the cursor as last set; presentCursorOnly commits it
    // alone. A driver that refuses it over some frame turns it off
    // (cursorRefused) until its image changes; the caller draws it then.

    /// The CRTC has a cursor plane, and its buffers were made.
    bool hasCursorPlane() const;
    /// The size of the cursor plane's buffer (the image's limit), px.
    uint32_t cursorWidth() const;
    uint32_t cursorHeight() const;
    /// The image the cursor shows from the next commit: `w` x `h` premultiplied
    /// B G R A rows (`stride` bytes apart), placed at the buffer's top-left;
    /// at most cursorWidth x cursorHeight.
    bool setCursorImage(const uint8_t* bgra, uint32_t w, uint32_t h, size_t stride);
    /// Where the cursor buffer's top-left goes on the screen (device px), and
    /// whether it shows at all.
    void setCursor(bool visible, int32_t x, int32_t y);
    /// The cursor the screen shows is not the one set: a commit is due.
    bool cursorChanged() const;
    /// The driver refused the cursor plane over the last frame.
    bool cursorRefused() const;
    /// Commit the cursor plane alone, flipping at the next vblank like a
    /// present. Waits for a pending flip first. False when there is nothing
    /// to commit it with, or the commit failed.
    bool presentCursorOnly();
    /// The flip pending (or the last one landed) carried the cursor alone.
    bool cursorOnlyFlip() const { return cursorOnlyFlip_; }

private:
    // Hands the cursor as set to the brodmabuf presenter before a commit.
    void stageCursor();
    // After a commit that carried it: the screen shows the staged cursor.
    void cursorCommitted();
    bool cursorOnlyFlip_ = false;
    bool cursorRefused_ = false;  // sticky until the image changes
    // Waits (bounded) until no hold is left on `slot`; false on timeout.
    bool waitForSlotRelease(size_t slot);

    // The holds on each slot, shared with the release functions holdSlot
    // hands out, which can outlive the presenter.
    struct SlotHolds;
    std::shared_ptr<SlotHolds> holds_;
    // Reads pending DRM events for up to timeoutMs, noting a flip in lastFlip_.
    bool readEvents(int timeoutMs);
    FlipInfo lastFlip_;
    bool flipPending_ = false;
    bool compositedFlipPending_ = false;  // the pending flip is a composited frame's
    FlipListener flipListener_;
    PresentTiming lastTiming_;
    FrameTap* frameTap_ = nullptr;
    ScanoutListener scanoutListener_;
    bool directScanoutInhibited_ = false;

    bool active_ = false;
    bool paused_ = false;
    int drmFd_ = -1;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    size_t currentSlot_ = 0;
    bool composited_ = false;      // a composited frame has been presented
    bool directOnScreen_ = false;  // the last present was a direct client scanout

#if defined(__linux__)
    std::shared_ptr<brodmabuf::KmsDevice> device_;
    std::unique_ptr<brodmabuf::KmsPresenter> presenter_;
    std::unique_ptr<brodmabuf::GbmDevice> gbmDevice_;
    std::unique_ptr<brodmabuf::VulkanContext> dmabufVkCtx_;
    std::vector<KmsScanoutSlot> scanoutSlots_;
    // directScanout's framebuffers, alive while they may be on screen.
    std::vector<std::unique_ptr<brodmabuf::KmsFramebuffer>> directFbs_;
    // The cursor plane's two buffers (one shown, one written), and the
    // cursor as set and as last committed.
    std::unique_ptr<brodmabuf::KmsDumbBuffer> cursorBufs_[2];
    int cursorBuf_ = -1;  // the buffer holding the newest image (-1: none yet)
    brodmabuf::KmsCursor cursorWant_;
    brodmabuf::KmsCursor cursorShown_;
    VkDevice vkDevice_ = VK_NULL_HANDLE;
#endif
};

} // namespace bro::render
