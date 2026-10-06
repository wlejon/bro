#pragma once

#include "render/layer_source.h"

#include <vulkan/vulkan.h>
#include <memory>
#include <unordered_map>
#include <vector>

#if defined(__linux__)
#include <brodmabuf/vulkan.h>
#include <brodmabuf/buffer.h>
#include <brodmabuf/sync.h>
#endif

namespace bro::render {

class VulkanContext;

struct ImportedClientBuffer {
    uint64_t bufferId = 0;
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t lastUsedFrame = 0;
#if defined(__linux__)
    std::unique_ptr<brodmabuf::VulkanImage> vulkanImage;
#endif
};

class VulkanDmabufImporter {
public:
    explicit VulkanDmabufImporter(VulkanContext& context);
    ~VulkanDmabufImporter();

    VulkanDmabufImporter(const VulkanDmabufImporter&) = delete;
    VulkanDmabufImporter& operator=(const VulkanDmabufImporter&) = delete;

    bool init();

    /// Import or get cached imported client DMA-BUF, honoring explicit sync fences
    ImportedClientBuffer* getOrImport(const DmabufLayerSource& src, uint64_t frameSerial);

    /// Purge buffers older than maxAgeFrames
    void prune(uint64_t currentFrameSerial, uint64_t maxAgeFrames = 60);

    /// Invalidate or remove a specific buffer
    void releaseBuffer(uint64_t bufferId);

    void clear();

    bool isSupported() const { return initialized_; }

private:
    VulkanContext& context_;
    bool initialized_ = false;
#if defined(__linux__)
    std::unique_ptr<brodmabuf::VulkanContext> dmabufCtx_;
#endif
    std::unordered_map<uint64_t, std::unique_ptr<ImportedClientBuffer>> cache_;
};

} // namespace bro::render
