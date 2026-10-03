#include "scene/vulkan/pass_environment.h"
#include "scene/vulkan/scene_vk_shader_compiler.h"
#include "util/log.h"

#include <cstring>

namespace bro::scene::vk {

PassEnvironment::~PassEnvironment() {
}

bool PassEnvironment::init(SceneVkDevice& device, SceneVkAllocator& allocator) {
    return init(device, allocator, Config{});
}

bool PassEnvironment::init(SceneVkDevice& device, SceneVkAllocator& allocator, const Config& config) {
    VkDevice dev = device.device();

    SceneVkDescriptorLayoutBuilder builder;
    builder.addBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    cubemapLayout_ = builder.build(dev);
    if (!cubemapLayout_) {
        LOG_ERROR("PassEnvironment: Failed creating cubemap descriptor layout");
        return false;
    }

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(EnvironmentPushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &cubemapLayout_;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;

    if (vkCreatePipelineLayout(dev, &layoutInfo, nullptr, &pipelineLayout_) != VK_SUCCESS) {
        LOG_ERROR("PassEnvironment: Failed creating pipeline layout");
        return false;
    }

    if (!createDefaultCubemap(device, allocator)) {
        LOG_ERROR("PassEnvironment: Failed creating default cubemap");
        return false;
    }

    if (!createPipeline(dev, config)) {
        LOG_ERROR("PassEnvironment: Failed creating pipeline");
        return false;
    }

    return true;
}

void PassEnvironment::cleanup(SceneVkDevice& device, SceneVkAllocator& allocator) {
    VkDevice dev = device.device();

    if (pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, pipeline_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
    }
    if (pipelineLayout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(dev, pipelineLayout_, nullptr);
        pipelineLayout_ = VK_NULL_HANDLE;
    }

    descPool_.destroy();

    dummyCubemapImage_.sampler = VK_NULL_HANDLE;
    if (cubemapSampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(dev, cubemapSampler_, nullptr);
        cubemapSampler_ = VK_NULL_HANDLE;
    }

    allocator.destroyImage(dummyCubemapImage_);

    if (cubemapLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, cubemapLayout_, nullptr);
        cubemapLayout_ = VK_NULL_HANDLE;
    }
}

bool PassEnvironment::createDefaultCubemap(SceneVkDevice& device, SceneVkAllocator& allocator) {
    VkDevice dev = device.device();

    // 1x1x6 cube map image
    VkImageCreateInfo imgInfo{};
    imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgInfo.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    imgInfo.imageType = VK_IMAGE_TYPE_2D;
    imgInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imgInfo.extent = {1, 1, 1};
    imgInfo.mipLevels = 1;
    imgInfo.arrayLayers = 6;
    imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imgInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(dev, &imgInfo, nullptr, &dummyCubemapImage_.image) != VK_SUCCESS) {
        LOG_ERROR("PassEnvironment: Failed creating dummy cubemap VkImage");
        return false;
    }

    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(dev, dummyCubemapImage_.image, &memReq);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;

    // Find device local memory type
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(device.physicalDevice(), &memProperties);
    uint32_t memTypeIndex = 0;
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; ++i) {
        if ((memReq.memoryTypeBits & (1 << i)) &&
            (memProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            memTypeIndex = i;
            break;
        }
    }
    allocInfo.memoryTypeIndex = memTypeIndex;

    if (vkAllocateMemory(dev, &allocInfo, nullptr, &dummyCubemapImage_.memory) != VK_SUCCESS) {
        LOG_ERROR("PassEnvironment: Failed allocating dummy cubemap memory");
        return false;
    }

    vkBindImageMemory(dev, dummyCubemapImage_.image, dummyCubemapImage_.memory, 0);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = dummyCubemapImage_.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 6;

    if (vkCreateImageView(dev, &viewInfo, nullptr, &dummyCubemapImage_.view) != VK_SUCCESS) {
        LOG_ERROR("PassEnvironment: Failed creating dummy cubemap view");
        return false;
    }

    dummyCubemapImage_.format = VK_FORMAT_R8G8B8A8_UNORM;
    dummyCubemapImage_.width = 1;
    dummyCubemapImage_.height = 1;
    dummyCubemapImage_.mipLevels = 1;

    // Transition image layout to SHADER_READ_ONLY_OPTIMAL
    device.executeImmediate([&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = dummyCubemapImage_.image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 6;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &barrier);
    });
    dummyCubemapImage_.currentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // Create cubemap sampler
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 1.0f;

    if (vkCreateSampler(dev, &samplerInfo, nullptr, &cubemapSampler_) != VK_SUCCESS) {
        LOG_ERROR("PassEnvironment: Failed creating cubemap sampler");
        return false;
    }
    dummyCubemapImage_.sampler = cubemapSampler_;

    if (!descPool_.init(dev, 2)) return false;
    defaultCubemapSet_ = descPool_.allocate(cubemapLayout_);
    if (!defaultCubemapSet_) return false;

    SceneVkDescriptorWriter writer;
    writer.writeImage(0, dummyCubemapImage_.view, cubemapSampler_);
    writer.updateSet(dev, defaultCubemapSet_);

    return true;
}

bool PassEnvironment::createPipeline(VkDevice device, const Config& config) {
    VkShaderModule vs = SceneVkShaderCompiler::createBuiltinModule(device, BuiltinSceneShader::SkyboxVert);
    VkShaderModule fs = SceneVkShaderCompiler::createBuiltinModule(device, BuiltinSceneShader::EnvironmentFrag);

    if (!vs || !fs) {
        LOG_ERROR("PassEnvironment: Failed creating shader modules");
        return false;
    }

    SceneVkPipelineBuilder builder;
    builder.addShaderStage(VK_SHADER_STAGE_VERTEX_BIT, vs)
           .addShaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, fs)
           .setInputTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
           .setPolygonMode(VK_POLYGON_MODE_FILL)
           .setCullMode(VK_CULL_MODE_NONE)
           .setMultisamplingNone()
           .disableBlending(1);

    if (config.depthTest) {
        // reversed-Z: test passes at far plane depth 0.0, depth write disabled
        builder.enableDepthTest(false, config.depthCompareOp);
    } else {
        builder.disableDepthTest();
    }

    builder.setDynamicRendering({config.colorFormat}, config.depthFormat);

    pipeline_ = builder.build(device, pipelineLayout_);

    SceneVkShaderModule::destroy(device, vs);
    SceneVkShaderModule::destroy(device, fs);

    return pipeline_ != VK_NULL_HANDLE;
}

void PassEnvironment::render(VkCommandBuffer cmd, uint32_t width, uint32_t height,
                             const EnvironmentParams& params,
                             VkDescriptorSet cubemapSet) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);

    VkViewport vp{};
    vp.x = 0.0f;
    vp.y = 0.0f;
    vp.width = static_cast<float>(width);
    vp.height = static_cast<float>(height);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = {width, height};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    VkDescriptorSet set = cubemapSet ? cubemapSet : defaultCubemapSet_;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1, &set, 0, nullptr);

    EnvironmentPushConstants push{};
    std::memcpy(push.invViewProj, params.invViewProj, sizeof(push.invViewProj));
    push.sunDir[0] = params.sunDirection[0];
    push.sunDir[1] = params.sunDirection[1];
    push.sunDir[2] = params.sunDirection[2];
    push.sunDir[3] = params.hasCubemap ? 1.0f : 0.0f;

    push.skyColor[0] = params.skyColor[0];
    push.skyColor[1] = params.skyColor[1];
    push.skyColor[2] = params.skyColor[2];
    push.skyColor[3] = params.skyIntensity;

    push.horizonColor[0] = params.horizonColor[0];
    push.horizonColor[1] = params.horizonColor[1];
    push.horizonColor[2] = params.horizonColor[2];
    push.horizonColor[3] = 1.0f;

    push.groundColor[0] = params.groundColor[0];
    push.groundColor[1] = params.groundColor[1];
    push.groundColor[2] = params.groundColor[2];
    push.groundColor[3] = params.sunIntensity;

    vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);

    // Fullscreen triangle (3 vertices generated by skybox.vert)
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

} // namespace bro::scene::vk
