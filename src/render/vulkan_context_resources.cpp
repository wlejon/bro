// VulkanContext resource helpers: single-time command submission, memory-type
// lookup, pooled and dedicated buffer/image creation, copies and layout
// transitions.

#include "render/vulkan_context.h"
#include "render/vulkan_util.h"
#include "util/log.h"

namespace bro::render {

VkCommandBuffer VulkanContext::beginSingleTimeCommands() const {
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = commandPool_;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    {
        std::lock_guard<std::mutex> lock(commandPoolMutex_);
        if (vkAllocateCommandBuffers(device_, &allocInfo, &commandBuffer) != VK_SUCCESS) {
            LOG_ERROR("VulkanContext: failed to allocate a single-time command buffer");
            return VK_NULL_HANDLE;
        }
    }

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vkBeginCommandBuffer(commandBuffer, &beginInfo);
    return commandBuffer;
}

void VulkanContext::endSingleTimeCommands(VkCommandBuffer commandBuffer) const {
    if (commandBuffer == VK_NULL_HANDLE) return;
    vkEndCommandBuffer(commandBuffer);

    // Through the queue owner, waiting on this submission's own ticket.
    QueueSubmit batch;
    batch.commandBuffers.push_back(commandBuffer);
    const uint64_t ticket = queue_.submit(batch);
    if (ticket != 0) queue_.wait(ticket);

    std::lock_guard<std::mutex> lock(commandPoolMutex_);
    vkFreeCommandBuffers(device_, commandPool_, 1, &commandBuffer);
}

uint32_t VulkanContext::findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const {
    if (auto index = render::findMemoryType(memoryProperties_, typeFilter, properties)) return *index;
    LOG_ERROR("Vulkan: no memory type in 0x%x has property flags 0x%x", typeFilter, properties);
    return kNoMemoryType;
}

bool VulkanContext::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                 VkMemoryPropertyFlags properties,
                                 VkBuffer& buffer, VkDeviceMemory& memory,
                                 VkDeviceSize& outOffset, uint64_t& outAllocId,
                                 void*& outMappedData) {
    if (size == 0) return false;

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device_, &bufferInfo, nullptr, &buffer) != VK_SUCCESS) {
        LOG_ERROR("VulkanContext: Failed to create buffer of size %zu", static_cast<size_t>(size));
        return false;
    }

    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(device_, buffer, &memRequirements);

    uint32_t memType = findMemoryType(memRequirements.memoryTypeBits, properties);
    if (memType == kNoMemoryType) {
        vkDestroyBuffer(device_, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
        return false;
    }

    if (!memoryPool_.allocate(device_, memRequirements.size, memRequirements.alignment,
                              memType, properties, /*isImage=*/false,
                              outAllocId, memory, outOffset, outMappedData)) {
        LOG_ERROR("VulkanContext: Failed to allocate pooled memory for buffer (%zu bytes)", static_cast<size_t>(memRequirements.size));
        vkDestroyBuffer(device_, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
        return false;
    }

    if (vkBindBufferMemory(device_, buffer, memory, outOffset) != VK_SUCCESS) {
        LOG_ERROR("VulkanContext: Failed to bind buffer memory at offset %zu", static_cast<size_t>(outOffset));
        vkDestroyBuffer(device_, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
        memoryPool_.free(device_, outAllocId);
        outAllocId = 0;
        memory = VK_NULL_HANDLE;
        outOffset = 0;
        outMappedData = nullptr;
        return false;
    }

    return true;
}

void VulkanContext::destroyBuffer(VkBuffer buffer, uint64_t allocId) {
    if (buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_, buffer, nullptr);
    }
    if (allocId != 0) {
        memoryPool_.free(device_, allocId);
    }
}

bool VulkanContext::createImage(uint32_t width, uint32_t height, VkFormat format,
                                VkImageTiling tiling, VkImageUsageFlags usage,
                                VkMemoryPropertyFlags properties,
                                VkImage& image, VkDeviceMemory& memory,
                                VkDeviceSize& outOffset, uint64_t& outAllocId,
                                uint32_t mipLevels, uint32_t arrayLayers,
                                VkImageCreateFlags flags) {
    if (width == 0 || height == 0 || arrayLayers == 0) return false;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.flags = flags;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = mipLevels;
    imageInfo.arrayLayers = arrayLayers;
    imageInfo.format = format;
    imageInfo.tiling = tiling;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = usage;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(device_, &imageInfo, nullptr, &image) != VK_SUCCESS) {
        LOG_ERROR("VulkanContext: Failed to create image (%ux%u)", width, height);
        return false;
    }

    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(device_, image, &memRequirements);

    uint32_t memType = findMemoryType(memRequirements.memoryTypeBits, properties);
    if (memType == kNoMemoryType) {
        vkDestroyImage(device_, image, nullptr);
        image = VK_NULL_HANDLE;
        return false;
    }
    void* dummyMapped = nullptr;

    if (!memoryPool_.allocate(device_, memRequirements.size, memRequirements.alignment,
                              memType, properties, /*isImage=*/true,
                              outAllocId, memory, outOffset, dummyMapped)) {
        LOG_ERROR("VulkanContext: Failed to allocate pooled memory for image");
        vkDestroyImage(device_, image, nullptr);
        image = VK_NULL_HANDLE;
        return false;
    }

    if (vkBindImageMemory(device_, image, memory, outOffset) != VK_SUCCESS) {
        LOG_ERROR("VulkanContext: Failed to bind image memory at offset %zu", static_cast<size_t>(outOffset));
        vkDestroyImage(device_, image, nullptr);
        image = VK_NULL_HANDLE;
        memoryPool_.free(device_, outAllocId);
        outAllocId = 0;
        memory = VK_NULL_HANDLE;
        outOffset = 0;
        return false;
    }

    return true;
}

void VulkanContext::destroyImage(VkImage image, uint64_t allocId) {
    if (image != VK_NULL_HANDLE) {
        vkDestroyImage(device_, image, nullptr);
    }
    if (allocId != 0) {
        memoryPool_.free(device_, allocId);
    }
}

bool VulkanContext::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                 VkMemoryPropertyFlags properties,
                                 VkBuffer& buffer, VkDeviceMemory& bufferMemory) const
{
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device_, &bufferInfo, nullptr, &buffer) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(device_, buffer, &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, properties);

    if (allocInfo.memoryTypeIndex == kNoMemoryType ||
        vkAllocateMemory(device_, &allocInfo, nullptr, &bufferMemory) != VK_SUCCESS) {
        vkDestroyBuffer(device_, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
        return false;
    }

    vkBindBufferMemory(device_, buffer, bufferMemory, 0);
    return true;
}

bool VulkanContext::createImage(uint32_t width, uint32_t height, VkFormat format,
                                VkImageTiling tiling, VkImageUsageFlags usage,
                                VkMemoryPropertyFlags properties,
                                VkImage& image, VkDeviceMemory& imageMemory,
                                uint32_t mipLevels, uint32_t arrayLayers,
                                VkImageCreateFlags flags) const
{
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.flags = flags;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = mipLevels;
    imageInfo.arrayLayers = arrayLayers;
    imageInfo.format = format;
    imageInfo.tiling = tiling;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = usage;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(device_, &imageInfo, nullptr, &image) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(device_, image, &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, properties);

    if (allocInfo.memoryTypeIndex == kNoMemoryType ||
        vkAllocateMemory(device_, &allocInfo, nullptr, &imageMemory) != VK_SUCCESS) {
        vkDestroyImage(device_, image, nullptr);
        image = VK_NULL_HANDLE;
        return false;
    }

    vkBindImageMemory(device_, image, imageMemory, 0);
    return true;
}

void VulkanContext::copyBufferToImage(VkBuffer buffer, VkImage image,
                                      uint32_t width, uint32_t height,
                                      VkCommandBuffer cmd,
                                      uint32_t mipLevel,
                                      uint32_t baseArrayLayer,
                                      uint32_t layerCount) const
{
    bool ownsCmd = (cmd == VK_NULL_HANDLE);
    if (ownsCmd) cmd = beginSingleTimeCommands();

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = mipLevel;
    region.imageSubresource.baseArrayLayer = baseArrayLayer;
    region.imageSubresource.layerCount = layerCount;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {width, height, 1};

    vkCmdCopyBufferToImage(cmd, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    if (ownsCmd) endSingleTimeCommands(cmd);
}

void VulkanContext::copyImageToBuffer(VkImage image, VkBuffer buffer,
                                      uint32_t width, uint32_t height,
                                      VkCommandBuffer cmd) const
{
    bool ownsCmd = (cmd == VK_NULL_HANDLE);
    if (ownsCmd) cmd = beginSingleTimeCommands();

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {width, height, 1};

    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);

    if (ownsCmd) endSingleTimeCommands(cmd);
}

void VulkanContext::transitionImageLayout(VkImage image, VkFormat format,
                                          VkImageLayout oldLayout, VkImageLayout newLayout,
                                          VkCommandBuffer cmd,
                                          uint32_t mipLevels, uint32_t baseMipLevel,
                                          uint32_t layerCount, uint32_t baseArrayLayer) const
{
    bool ownsCmd = (cmd == VK_NULL_HANDLE);
    if (ownsCmd) cmd = beginSingleTimeCommands();

    VkImageSubresourceRange range{};
    range.aspectMask = imageAspectFor(format);
    range.baseMipLevel = baseMipLevel;
    range.levelCount = mipLevels;
    range.baseArrayLayer = baseArrayLayer;
    range.layerCount = layerCount;
    cmdTransitionImage(cmd, image, range, oldLayout, newLayout);

    if (ownsCmd) endSingleTimeCommands(cmd);
}

void VulkanContext::waitIdle() const {
    if (device_ != VK_NULL_HANDLE) queue_.waitIdle();
}

} // namespace bro::render
