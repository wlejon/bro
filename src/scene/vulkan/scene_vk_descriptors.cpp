#include "scene/vulkan/scene_vk_descriptors.h"
#include "util/log.h"

#include <cassert>

namespace bro::scene::vk {

void SceneVkDescriptorLayoutBuilder::addBinding(uint32_t binding, VkDescriptorType type,
                                               uint32_t count, VkShaderStageFlags stageFlags) {
    VkDescriptorSetLayoutBinding b{};
    b.binding = binding;
    b.descriptorType = type;
    b.descriptorCount = count;
    b.stageFlags = stageFlags;
    b.pImmutableSamplers = nullptr;
    bindings_.push_back(b);
}

void SceneVkDescriptorLayoutBuilder::clear() {
    bindings_.clear();
}

VkDescriptorSetLayout SceneVkDescriptorLayoutBuilder::build(VkDevice device) {
    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = static_cast<uint32_t>(bindings_.size());
    info.pBindings = bindings_.empty() ? nullptr : bindings_.data();

    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(device, &info, nullptr, &layout) != VK_SUCCESS) {
        LOG_ERROR("SceneVkDescriptorLayoutBuilder: Failed to create descriptor set layout");
        return VK_NULL_HANDLE;
    }
    return layout;
}

VkDescriptorSetLayout SceneVkDescriptorLayoutBuilder::createCameraLayout(VkDevice device) {
    SceneVkDescriptorLayoutBuilder builder;
    builder.addBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
    return builder.build(device);
}

VkDescriptorSetLayout SceneVkDescriptorLayoutBuilder::createLightingLayout(VkDevice device) {
    SceneVkDescriptorLayoutBuilder builder;
    builder.addBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    return builder.build(device);
}

VkDescriptorSetLayout SceneVkDescriptorLayoutBuilder::createMaterialLayout(VkDevice device, uint32_t samplerCount) {
    SceneVkDescriptorLayoutBuilder builder;
    for (uint32_t i = 0; i < samplerCount; ++i) {
        builder.addBinding(i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    }
    return builder.build(device);
}

void SceneVkDescriptorWriter::writeBuffer(uint32_t binding, VkBuffer buffer, VkDeviceSize size,
                                          VkDeviceSize offset, VkDescriptorType type) {
    VkDescriptorBufferInfo bufInfo{};
    bufInfo.buffer = buffer;
    bufInfo.offset = offset;
    bufInfo.range = size;
    bufferInfos_.push_back(bufInfo);

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = type;
    // pBufferInfo will be resolved during updateSet to prevent vector reallocation invalidation
    writes_.push_back(write);
}

void SceneVkDescriptorWriter::writeImage(uint32_t binding, VkImageView imageView, VkSampler sampler,
                                         VkImageLayout layout, VkDescriptorType type) {
    VkDescriptorImageInfo imgInfo{};
    imgInfo.imageView = imageView;
    imgInfo.sampler = sampler;
    imgInfo.imageLayout = layout;
    imageInfos_.push_back(imgInfo);

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = type;
    writes_.push_back(write);
}

void SceneVkDescriptorWriter::clear() {
    bufferInfos_.clear();
    imageInfos_.clear();
    writes_.clear();
}

void SceneVkDescriptorWriter::updateSet(VkDevice device, VkDescriptorSet set) {
    size_t bufIdx = 0;
    size_t imgIdx = 0;

    for (auto& write : writes_) {
        write.dstSet = set;
        if (write.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
            write.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
            write.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) {
            write.pBufferInfo = &bufferInfos_[bufIdx++];
        } else if (write.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
                   write.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
                   write.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
            write.pImageInfo = &imageInfos_[imgIdx++];
        }
    }

    if (!writes_.empty()) {
        vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes_.size()), writes_.data(), 0, nullptr);
    }
}

SceneVkDescriptorPool::~SceneVkDescriptorPool() {
    destroy();
}

bool SceneVkDescriptorPool::init(VkDevice device, uint32_t maxSets,
                                 const std::vector<VkDescriptorPoolSize>& customSizes) {
    destroy();
    device_ = device;

    std::vector<VkDescriptorPoolSize> poolSizes = customSizes;
    if (poolSizes.empty()) {
        poolSizes = {
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, maxSets * 2 },
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, maxSets * 4 },
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, maxSets },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, maxSets }
        };
    }

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = 0;
    poolInfo.maxSets = maxSets;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();

    if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &pool_) != VK_SUCCESS) {
        LOG_ERROR("SceneVkDescriptorPool: Failed to create descriptor pool");
        return false;
    }
    return true;
}

void SceneVkDescriptorPool::destroy() {
    if (device_ != VK_NULL_HANDLE && pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, pool_, nullptr);
        pool_ = VK_NULL_HANDLE;
    }
}

VkDescriptorSet SceneVkDescriptorPool::allocate(VkDescriptorSetLayout layout) {
    assert(pool_ != VK_NULL_HANDLE);

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = pool_;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &layout;

    VkDescriptorSet set = VK_NULL_HANDLE;
    VkResult res = vkAllocateDescriptorSets(device_, &allocInfo, &set);
    if (res != VK_SUCCESS) {
        LOG_ERROR("SceneVkDescriptorPool: Failed to allocate descriptor set: %d", res);
        return VK_NULL_HANDLE;
    }
    return set;
}

void SceneVkDescriptorPool::reset() {
    if (device_ != VK_NULL_HANDLE && pool_ != VK_NULL_HANDLE) {
        vkResetDescriptorPool(device_, pool_, 0);
    }
}

SceneVkDescriptorCache::SceneVkDescriptorCache(VkDevice device)
    : device_(device)
{
}

SceneVkDescriptorCache::~SceneVkDescriptorCache() {
    destroy();
}

bool SceneVkDescriptorCache::init(uint32_t maxSetsPerFrame) {
    return pool_.init(device_, maxSetsPerFrame);
}

void SceneVkDescriptorCache::destroy() {
    pool_.destroy();
}

VkDescriptorSet SceneVkDescriptorCache::allocate(VkDescriptorSetLayout layout) {
    return pool_.allocate(layout);
}

void SceneVkDescriptorCache::reset() {
    pool_.reset();
}

} // namespace bro::scene::vk
