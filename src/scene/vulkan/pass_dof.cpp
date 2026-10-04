#include "scene/vulkan/pass_dof.h"
#include "scene/vulkan/scene_vk_shader_compiler.h"
#include "util/log.h"
#include <algorithm>

namespace bro::scene::vk {

PassDoF::~PassDoF() = default;

bool PassDoF::init(SceneVkDevice& device, SceneVkAllocator& allocator, uint32_t width, uint32_t height) {
    VkSamplerCreateInfo sampInfo{};
    sampInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampInfo.magFilter = VK_FILTER_LINEAR;
    sampInfo.minFilter = VK_FILTER_LINEAR;
    sampInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(device.device(), &sampInfo, nullptr, &linearClampSampler_) != VK_SUCCESS) {
        LOG_ERROR("PassDoF: Failed creating sampler");
        return false;
    }

    if (!createPipelines(device.device())) {
        return false;
    }

    return createIntermediateTargets(allocator, width, height);
}

void PassDoF::cleanup(SceneVkDevice& device, SceneVkAllocator& allocator) {
    destroyIntermediateTargets(allocator);

    VkDevice dev = device.device();
    if (linearClampSampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(dev, linearClampSampler_, nullptr);
        linearClampSampler_ = VK_NULL_HANDLE;
    }
    if (blurPipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, blurPipeline_, nullptr);
        blurPipeline_ = VK_NULL_HANDLE;
    }
    if (singleTexPipelineLayout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(dev, singleTexPipelineLayout_, nullptr);
        singleTexPipelineLayout_ = VK_NULL_HANDLE;
    }
    if (singleTexDescLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, singleTexDescLayout_, nullptr);
        singleTexDescLayout_ = VK_NULL_HANDLE;
    }

    if (dofPipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, dofPipeline_, nullptr);
        dofPipeline_ = VK_NULL_HANDLE;
    }
    if (dofPipelineLayout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(dev, dofPipelineLayout_, nullptr);
        dofPipelineLayout_ = VK_NULL_HANDLE;
    }
    if (dofDescLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, dofDescLayout_, nullptr);
        dofDescLayout_ = VK_NULL_HANDLE;
    }
}

bool PassDoF::resize(SceneVkDevice& device, SceneVkAllocator& allocator, uint32_t width, uint32_t height) {
    (void)device;
    destroyIntermediateTargets(allocator);
    return createIntermediateTargets(allocator, width, height);
}

bool PassDoF::createPipelines(VkDevice device) {
    // 1. Blur pipeline
    SceneVkDescriptorLayoutBuilder singleBuilder;
    singleBuilder.addBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    singleTexDescLayout_ = singleBuilder.build(device);
    if (!singleTexDescLayout_) return false;

    VkPushConstantRange blurPushRange{};
    blurPushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    blurPushRange.offset = 0;
    blurPushRange.size = sizeof(float) * 4;

    VkPipelineLayoutCreateInfo blurPlInfo{};
    blurPlInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    blurPlInfo.setLayoutCount = 1;
    blurPlInfo.pSetLayouts = &singleTexDescLayout_;
    blurPlInfo.pushConstantRangeCount = 1;
    blurPlInfo.pPushConstantRanges = &blurPushRange;
    if (vkCreatePipelineLayout(device, &blurPlInfo, nullptr, &singleTexPipelineLayout_) != VK_SUCCESS) return false;

    VkShaderModule vsMod = SceneVkShaderCompiler::createBuiltinModule(device, BuiltinSceneShader::PostFxVert);
    VkShaderModule blurFsMod = SceneVkShaderCompiler::createBuiltinModule(device, BuiltinSceneShader::BlurFrag);
    if (!vsMod || !blurFsMod) {
        if (vsMod) SceneVkShaderCompiler::destroyModule(device, vsMod);
        if (blurFsMod) SceneVkShaderCompiler::destroyModule(device, blurFsMod);
        return false;
    }

    SceneVkPipelineBuilder b1;
    b1.setShaderStages(vsMod, blurFsMod)
      .setCullMode(VK_CULL_MODE_NONE)
      .disableBlending(1)
      .disableDepthTest()
      .setDynamicRendering({VK_FORMAT_R16G16B16A16_SFLOAT}, VK_FORMAT_UNDEFINED);
    blurPipeline_ = b1.build(device, singleTexPipelineLayout_);
    SceneVkShaderCompiler::destroyModule(device, blurFsMod);

    // 2. DoF composite pipeline
    SceneVkDescriptorLayoutBuilder dofBuilder;
    dofBuilder.addBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    dofBuilder.addBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    dofBuilder.addBinding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    dofDescLayout_ = dofBuilder.build(device);
    if (!dofDescLayout_) {
        SceneVkShaderCompiler::destroyModule(device, vsMod);
        return false;
    }

    VkPushConstantRange dofPushRange{};
    dofPushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    dofPushRange.offset = 0;
    dofPushRange.size = sizeof(float) * 5;

    VkPipelineLayoutCreateInfo dofPlInfo{};
    dofPlInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    dofPlInfo.setLayoutCount = 1;
    dofPlInfo.pSetLayouts = &dofDescLayout_;
    dofPlInfo.pushConstantRangeCount = 1;
    dofPlInfo.pPushConstantRanges = &dofPushRange;
    if (vkCreatePipelineLayout(device, &dofPlInfo, nullptr, &dofPipelineLayout_) != VK_SUCCESS) {
        SceneVkShaderCompiler::destroyModule(device, vsMod);
        return false;
    }

    VkShaderModule dofFsMod = SceneVkShaderCompiler::createBuiltinModule(device, BuiltinSceneShader::DofFrag);
    SceneVkPipelineBuilder b2;
    b2.setShaderStages(vsMod, dofFsMod)
      .setCullMode(VK_CULL_MODE_NONE)
      .disableBlending(1)
      .disableDepthTest()
      .setDynamicRendering({VK_FORMAT_R16G16B16A16_SFLOAT}, VK_FORMAT_UNDEFINED);
    dofPipeline_ = b2.build(device, dofPipelineLayout_);

    SceneVkShaderCompiler::destroyModule(device, vsMod);
    SceneVkShaderCompiler::destroyModule(device, dofFsMod);

    return blurPipeline_ != VK_NULL_HANDLE && dofPipeline_ != VK_NULL_HANDLE;
}

bool PassDoF::createIntermediateTargets(SceneVkAllocator& allocator, uint32_t width, uint32_t height) {
    halfW_ = std::max(1u, width / 2);
    halfH_ = std::max(1u, height / 2);

    for (int i = 0; i < 2; ++i) {
        if (!allocator.createImage(halfW_, halfH_, VK_FORMAT_R16G16B16A16_SFLOAT,
                                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                  blurTex_[i])) {
            LOG_ERROR("PassDoF: Failed creating half-res target %d", i);
            return false;
        }
    }
    return true;
}

void PassDoF::destroyIntermediateTargets(SceneVkAllocator& allocator) {
    for (int i = 0; i < 2; ++i) {
        allocator.destroyImage(blurTex_[i]);
    }
    halfW_ = 0;
    halfH_ = 0;
}

void PassDoF::render(VkCommandBuffer cmd, SceneVkDevice& device, SceneVkAllocator& allocator,
                     SceneVkDescriptorPool& descPool,
                     const SceneVkImage& sharpHdrImage,
                     const SceneVkImage& depthImage,
                     VkImageView outputTargetView,
                     uint32_t width, uint32_t height,
                     const DoFParams& params) {
    if (!blurTex_[0].isValid() || halfW_ == 0 || halfH_ == 0) return;

    // 1. Blit downsample sharpHdrImage -> blurTex_[0]
    allocator.transitionImageLayout(cmd, sharpHdrImage.image, VK_FORMAT_R16G16B16A16_SFLOAT,
                                   sharpHdrImage.currentLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    allocator.transitionImageLayout(cmd, blurTex_[0].image, VK_FORMAT_R16G16B16A16_SFLOAT,
                                   blurTex_[0].currentLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkImageBlit blitRegion{};
    blitRegion.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blitRegion.srcOffsets[0] = {0, 0, 0};
    blitRegion.srcOffsets[1] = {static_cast<int32_t>(width), static_cast<int32_t>(height), 1};
    blitRegion.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blitRegion.dstOffsets[0] = {0, 0, 0};
    blitRegion.dstOffsets[1] = {static_cast<int32_t>(halfW_), static_cast<int32_t>(halfH_), 1};

    vkCmdBlitImage(cmd,
                   sharpHdrImage.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   blurTex_[0].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &blitRegion, VK_FILTER_LINEAR);

    allocator.transitionImageLayout(cmd, sharpHdrImage.image, VK_FORMAT_R16G16B16A16_SFLOAT,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    allocator.transitionImageLayout(cmd, blurTex_[0].image, VK_FORMAT_R16G16B16A16_SFLOAT,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    blurTex_[0].currentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // 2. Horizontal Gaussian blur: blurTex_[0] -> blurTex_[1]
    allocator.transitionImageLayout(cmd, blurTex_[1].image, VK_FORMAT_R16G16B16A16_SFLOAT,
                                   blurTex_[1].currentLayout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    blurTex_[1].currentLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkDescriptorSet setH = descPool.allocate(singleTexDescLayout_);
    SceneVkDescriptorWriter writerH;
    writerH.writeImage(0, blurTex_[0].view, linearClampSampler_);
    writerH.updateSet(device.device(), setH);

    VkViewport halfVp{0.0f, 0.0f, static_cast<float>(halfW_), static_cast<float>(halfH_), 0.0f, 1.0f};
    VkRect2D halfScissor{{0, 0}, {halfW_, halfH_}};

    VkRenderingAttachmentInfo attInfo{};
    attInfo.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    attInfo.imageView = blurTex_[1].view;
    attInfo.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attInfo.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attInfo.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo rInfo{};
    rInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rInfo.renderArea = {{0, 0}, {halfW_, halfH_}};
    rInfo.layerCount = 1;
    rInfo.colorAttachmentCount = 1;
    rInfo.pColorAttachments = &attInfo;

    vkCmdBeginRendering(cmd, &rInfo);
    vkCmdSetViewport(cmd, 0, 1, &halfVp);
    vkCmdSetScissor(cmd, 0, 1, &halfScissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, blurPipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, singleTexPipelineLayout_, 0, 1, &setH, 0, nullptr);
    float pushH[4] = {params.maxBlur / static_cast<float>(halfW_), 0.0f, 0.0f, 0.0f};
    vkCmdPushConstants(cmd, singleTexPipelineLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pushH), pushH);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);

    // 3. Vertical Gaussian blur: blurTex_[1] -> blurTex_[0]
    allocator.transitionImageLayout(cmd, blurTex_[1].image, VK_FORMAT_R16G16B16A16_SFLOAT,
                                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    blurTex_[1].currentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    allocator.transitionImageLayout(cmd, blurTex_[0].image, VK_FORMAT_R16G16B16A16_SFLOAT,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    blurTex_[0].currentLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkDescriptorSet setV = descPool.allocate(singleTexDescLayout_);
    SceneVkDescriptorWriter writerV;
    writerV.writeImage(0, blurTex_[1].view, linearClampSampler_);
    writerV.updateSet(device.device(), setV);

    attInfo.imageView = blurTex_[0].view;
    vkCmdBeginRendering(cmd, &rInfo);
    vkCmdSetViewport(cmd, 0, 1, &halfVp);
    vkCmdSetScissor(cmd, 0, 1, &halfScissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, blurPipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, singleTexPipelineLayout_, 0, 1, &setV, 0, nullptr);
    float pushV[4] = {0.0f, params.maxBlur / static_cast<float>(halfH_), 0.0f, 0.0f};
    vkCmdPushConstants(cmd, singleTexPipelineLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pushV), pushV);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);

    allocator.transitionImageLayout(cmd, blurTex_[0].image, VK_FORMAT_R16G16B16A16_SFLOAT,
                                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    blurTex_[0].currentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // 4. DoF composite pass
    VkDescriptorSet dofSet = descPool.allocate(dofDescLayout_);
    SceneVkDescriptorWriter dofWriter;
    dofWriter.writeImage(0, sharpHdrImage.view, linearClampSampler_);
    dofWriter.writeImage(1, blurTex_[0].view, linearClampSampler_);
    dofWriter.writeImage(2, depthImage.view, linearClampSampler_);
    dofWriter.updateSet(device.device(), dofSet);

    VkViewport fullVp{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    VkRect2D fullScissor{{0, 0}, {width, height}};

    attInfo.imageView = outputTargetView;
    rInfo.renderArea = {{0, 0}, {width, height}};
    rInfo.pColorAttachments = &attInfo;

    vkCmdBeginRendering(cmd, &rInfo);
    vkCmdSetViewport(cmd, 0, 1, &fullVp);
    vkCmdSetScissor(cmd, 0, 1, &fullScissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, dofPipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, dofPipelineLayout_, 0, 1, &dofSet, 0, nullptr);

    float dofPush[5] = {
        params.focusDistance,
        params.focusRange,
        params.nearPlane,
        params.farPlane,
        params.isPerspective ? 1.0f : 0.0f
    };
    vkCmdPushConstants(cmd, dofPipelineLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(dofPush), dofPush);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

} // namespace bro::scene::vk
