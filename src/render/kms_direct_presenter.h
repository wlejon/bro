#pragma once

#include "render/layer_source.h"

#include <cstdint>
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
#endif
    uint32_t width = 0;
    uint32_t height = 0;
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

    /// Restore KMS modeset after VT switch resume
    bool restoreModeset();

    /// Pause presentation during VT switch away
    void pause();

    bool isActive() const { return active_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    void close();

private:
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
    VkDevice vkDevice_ = VK_NULL_HANDLE;
#endif
};

} // namespace bro::render
