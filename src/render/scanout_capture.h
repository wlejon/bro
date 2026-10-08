#pragma once

// Copies every composited frame out of the scanout buffer, scaled down, for
// the screen recorder (bro-ctl record, engine/control_record.cpp). It is a
// KmsDirectPresenter frame tap: the copy is a blit recorded into the frame's
// own command buffer after the composite, then a copy into a host buffer —
// work the GPU does alongside the frame, which the presenter already waits
// for — so recording costs the frame a small blit and a memcpy, not a
// readback stall. Each frame is handed to the sink with the flip it landed on.

#include "render/kms_direct_presenter.h"

#include <cstdint>
#include <functional>

namespace bro::render {

class VulkanContext;

class ScanoutCapture : public KmsDirectPresenter::FrameTap {
public:
    /// BGRA8 pixels, tightly packed (width*4 per row), valid during the call.
    using Sink = std::function<void(const uint8_t* bgra, uint32_t width, uint32_t height,
                                    const KmsDirectPresenter::FlipInfo& flip)>;

    ScanoutCapture(VulkanContext& ctx, uint32_t width, uint32_t height, Sink sink);
    ~ScanoutCapture() override;

    bool valid() const { return image_ != VK_NULL_HANDLE && buffer_ != VK_NULL_HANDLE && mapped_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

    void record(VkCommandBuffer cmd, VkImage image, uint32_t width, uint32_t height) override;
    void completed(const KmsDirectPresenter::FlipInfo& flip) override;

private:
    VulkanContext& ctx_;
    uint32_t width_ = 0, height_ = 0;
    Sink sink_;
    VkImage image_ = VK_NULL_HANDLE;
    VkDeviceMemory imageMemory_ = VK_NULL_HANDLE;
    VkDeviceSize imageOffset_ = 0;
    uint64_t imageAlloc_ = 0;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory bufferMemory_ = VK_NULL_HANDLE;
    VkDeviceSize bufferOffset_ = 0;
    uint64_t bufferAlloc_ = 0;
    void* mapped_ = nullptr;
    bool pending_ = false;  // a copy was recorded for the frame being presented
    bool imageInitialized_ = false;
};

}  // namespace bro::render
