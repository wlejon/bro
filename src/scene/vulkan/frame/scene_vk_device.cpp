#include "scene/vulkan/frame/scene_vk_device.h"
#include "render/vulkan_util.h"
#include "util/log.h"

#include <cstring>

namespace bro::scene::vk {

SceneVkDevice::SceneVkDevice(render::VulkanContext& context)
    : context_(context)
{
}

SceneVkDevice::~SceneVkDevice() {
    shutdown();
}

bool SceneVkDevice::init() {
    if (initialized_) return true;
    if (!context_.isValid()) {
        LOG_ERROR("SceneVkDevice: VulkanContext is not valid");
        return false;
    }
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    samplerInfo.compareEnable = VK_TRUE;
    samplerInfo.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 1.0f;
    if (vkCreateSampler(context_.device(), &samplerInfo, nullptr, &shadowCompareSampler_) != VK_SUCCESS) {
        LOG_ERROR("SceneVkDevice: Failed to create the shadow comparison sampler");
        shadowCompareSampler_ = VK_NULL_HANDLE;
        return false;
    }
    frameEndHook_ = context_.frames().addFrameEndHook([this] { flushUploads(); });
    initialized_ = true;
    return true;
}

void SceneVkDevice::shutdown() {
    if (!initialized_) return;
    flushUploads();
    context_.frames().removeFrameEndHook(frameEndHook_);
    frameEndHook_ = 0;
    // Layouts that bake the sampler in are destroyed by now; sets still in
    // flight may reference it, so it goes once the GPU is done.
    if (shadowCompareSampler_ != VK_NULL_HANDLE) {
        context_.frames().defer([dev = context_.device(), s = shadowCompareSampler_] {
            vkDestroySampler(dev, s, nullptr);
        });
        shadowCompareSampler_ = VK_NULL_HANDLE;
    }
    initialized_ = false;
}

void SceneVkDevice::waitIdle() const {
    if (context_.isValid()) context_.waitIdle();
}

VkCommandBuffer SceneVkDevice::beginFrame() {
    return context_.frames().beginCommands();
}

bool SceneVkDevice::submitFrame(VkCommandBuffer cmd) {
    render::VulkanFrames& frames = context_.frames();
    flushUploads();
    uint64_t computeTicket = 0;
    if (computeCmd_ != VK_NULL_HANDLE) {
        // What compute writes is versioned by frame slot, so it never waits
        // for the previous frame's graphics, except for inputs the graphics
        // queue wrote, or when an earlier render this frame still reads this
        // slot's results.
        std::vector<render::SemaphoreWait> waits;
        render::VulkanQueue& graphics = context_.queue();
        if (computeAfterGraphics_ || lastFrameSerial_ == frames.frameSerial())
            if (const uint64_t t = graphics.lastSubmittedTicket())
                waits.push_back({graphics.timeline(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, t});
        computeTicket = frames.submitCompute(computeCmd_, std::move(waits));
        if (computeTicket == 0) LOG_ERROR("SceneVkDevice: Failed to submit the frame's compute");
    }
    if (headCmd_ != VK_NULL_HANDLE && frames.submit(headCmd_) == 0)
        LOG_ERROR("SceneVkDevice: Failed to submit the frame's graphics before its compute join");
    std::vector<render::SemaphoreWait> waits;
    if (computeTicket != 0)
        waits.push_back({computeQueue().timeline(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, computeTicket});
    const uint64_t ticket = frames.submit(cmd, std::move(waits));
    computeCmd_ = headCmd_ = VK_NULL_HANDLE;
    computeAfterGraphics_ = false;
    lastFrameSerial_ = frames.frameSerial();
    lastComputeTicket_ = computeTicket;
    if (ticket == 0) {
        LOG_ERROR("SceneVkDevice: Failed to submit the frame command buffer");
        return false;
    }
    lastFrameTicket_ = ticket;
    return true;
}

render::VulkanQueue& SceneVkDevice::computeQueue() {
    render::VulkanQueue* async = context_.asyncComputeQueue();
    return async ? *async : context_.queue();
}

VkCommandBuffer SceneVkDevice::computeCommands(VkCommandBuffer frameCmd) {
    if (!asyncCompute()) return frameCmd;
    if (computeCmd_ == VK_NULL_HANDLE) computeCmd_ = context_.frames().beginComputeCommands();
    return computeCmd_ != VK_NULL_HANDLE ? computeCmd_ : frameCmd;
}

VkCommandBuffer SceneVkDevice::joinCompute(VkCommandBuffer cmd) {
    if (computeCmd_ == VK_NULL_HANDLE || headCmd_ != VK_NULL_HANDLE) return cmd;
    VkCommandBuffer rest = context_.frames().beginCommands();
    if (rest == VK_NULL_HANDLE) return cmd;
    headCmd_ = cmd;
    return rest;
}

VkCommandBuffer SceneVkDevice::uploadCommands() {
    if (uploadCmd_ != VK_NULL_HANDLE) return uploadCmd_;
    uploadCmd_ = context_.frames().beginCommands();
    // Copies into resources earlier submissions still read or wrote.
    render::cmdMemoryBarrier(uploadCmd_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    return uploadCmd_;
}

void SceneVkDevice::flushUploads() {
    if (uploadCmd_ == VK_NULL_HANDLE) return;
    // Uploaded data is visible to every later reader.
    render::cmdMemoryBarrier(uploadCmd_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT);
    if (context_.frames().submit(uploadCmd_) == 0)
        LOG_ERROR("SceneVkDevice: Failed to submit scene uploads");
    uploadCmd_ = VK_NULL_HANDLE;
}

render::UploadSlice SceneVkDevice::frameUpload(VkDeviceSize size, VkDeviceSize alignment) {
    render::UploadSlice slice = context_.frames().allocUpload(size, alignment);
    if (!slice) LOG_ERROR("SceneVkDevice: out of frame upload memory (%llu bytes)",
                          static_cast<unsigned long long>(size));
    return slice;
}

VkDescriptorBufferInfo SceneVkDevice::frameUniform(const void* data, VkDeviceSize size) {
    render::UploadSlice slice = frameUpload(size, context_.deviceProperties().limits.minUniformBufferOffsetAlignment);
    if (!slice) return {};
    std::memcpy(slice.mapped, data, static_cast<size_t>(size));
    return {slice.buffer, slice.offset, size};
}

VkDescriptorSet SceneVkDevice::frameSet(VkDescriptorSetLayout layout) {
    return context_.frames().allocDescriptorSet(layout);
}

void SceneVkDevice::defer(std::function<void()> destroy) {
    context_.frames().defer(std::move(destroy));
}

void SceneVkDevice::cmdBeginRendering(VkCommandBuffer cmd, const VkRenderingInfo* renderingInfo) const {
    vkCmdBeginRendering(cmd, renderingInfo);
}

void SceneVkDevice::cmdEndRendering(VkCommandBuffer cmd) const {
    vkCmdEndRendering(cmd);
}

} // namespace bro::scene::vk
