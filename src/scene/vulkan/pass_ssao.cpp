#include "scene/vulkan/pass_ssao.h"
#include "scene/vulkan/scene_vk_shader_compiler.h"
#include "util/log.h"

#include <cmath>
#include <cstring>
#include <random>

namespace bro::scene::vk {

PassSSAO::~PassSSAO() = default;

bool PassSSAO::init(SceneVkDevice& device, SceneVkAllocator& allocator, uint32_t width, uint32_t height) {
    generateKernel();

    if (!allocator.createUniformBuffer(sizeof(SSAOUBOData), ssaoUbo_)) {
        LOG_ERROR("PassSSAO: Failed creating SSAO uniform buffer");
        return false;
    }

    if (!createNoiseTexture(device, allocator)) {
        LOG_ERROR("PassSSAO: Failed creating noise texture");
        return false;
    }

    VkSamplerCreateInfo ptSamp{};
    ptSamp.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    ptSamp.magFilter = VK_FILTER_LINEAR;
    ptSamp.minFilter = VK_FILTER_LINEAR;
    ptSamp.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ptSamp.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ptSamp.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(device.device(), &ptSamp, nullptr, &pointClampSampler_) != VK_SUCCESS) {
        LOG_ERROR("PassSSAO: Failed creating sampler");
        return false;
    }

    if (!createPipelines(device.device())) {
        return false;
    }

    return createTargets(allocator, width, height);
}

void PassSSAO::cleanup(SceneVkDevice& device, SceneVkAllocator& allocator) {
    destroyTargets(allocator);

    allocator.destroyBuffer(ssaoUbo_);
    allocator.destroyImage(noiseTex_);

    VkDevice dev = device.device();
    if (pointClampSampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(dev, pointClampSampler_, nullptr);
        pointClampSampler_ = VK_NULL_HANDLE;
    }
    if (linearRepeatSampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(dev, linearRepeatSampler_, nullptr);
        linearRepeatSampler_ = VK_NULL_HANDLE;
    }

    if (ssaoPipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, ssaoPipeline_, nullptr);
        ssaoPipeline_ = VK_NULL_HANDLE;
    }
    if (ssaoPipelineLayout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(dev, ssaoPipelineLayout_, nullptr);
        ssaoPipelineLayout_ = VK_NULL_HANDLE;
    }
    if (ssaoDescLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, ssaoDescLayout_, nullptr);
        ssaoDescLayout_ = VK_NULL_HANDLE;
    }

    if (blurPipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, blurPipeline_, nullptr);
        blurPipeline_ = VK_NULL_HANDLE;
    }
    if (blurPipelineLayout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(dev, blurPipelineLayout_, nullptr);
        blurPipelineLayout_ = VK_NULL_HANDLE;
    }
    if (blurDescLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, blurDescLayout_, nullptr);
        blurDescLayout_ = VK_NULL_HANDLE;
    }

    if (applyAoPipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, applyAoPipeline_, nullptr);
        applyAoPipeline_ = VK_NULL_HANDLE;
    }
    if (applyAoPipelineLayout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(dev, applyAoPipelineLayout_, nullptr);
        applyAoPipelineLayout_ = VK_NULL_HANDLE;
    }
    if (applyAoDescLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, applyAoDescLayout_, nullptr);
        applyAoDescLayout_ = VK_NULL_HANDLE;
    }
}

bool PassSSAO::resize(SceneVkDevice& device, SceneVkAllocator& allocator, uint32_t width, uint32_t height) {
    (void)device;
    destroyTargets(allocator);
    return createTargets(allocator, width, height);
}

void PassSSAO::generateKernel() {
    std::mt19937 gen(1337);
    std::uniform_real_distribution<float> dis(0.0f, 1.0f);

    for (int i = 0; i < 16; ++i) {
        float x = dis(gen) * 2.0f - 1.0f;
        float y = dis(gen) * 2.0f - 1.0f;
        float z = dis(gen); // Hemisphere [0, 1]
        float len = std::sqrt(x * x + y * y + z * z);
        if (len < 1e-4f) { x = 0; y = 0; z = 1; len = 1; }

        float scale = static_cast<float>(i) / 16.0f;
        scale = 0.1f + scale * scale * 0.9f; // Accelerate towards origin

        kernel_[i * 4 + 0] = (x / len) * scale;
        kernel_[i * 4 + 1] = (y / len) * scale;
        kernel_[i * 4 + 2] = (z / len) * scale;
        kernel_[i * 4 + 3] = 0.0f;
    }
}

bool PassSSAO::createNoiseTexture(SceneVkDevice& device, SceneVkAllocator& allocator) {
    std::mt19937 gen(42);
    std::uniform_real_distribution<float> dis(0.0f, 1.0f);

    uint8_t noisePixels[16 * 4];
    for (int i = 0; i < 16; ++i) {
        float angle = dis(gen) * 6.2831853f;
        float nx = std::cos(angle) * 0.5f + 0.5f;
        float ny = std::sin(angle) * 0.5f + 0.5f;
        noisePixels[i * 4 + 0] = static_cast<uint8_t>(nx * 255.0f);
        noisePixels[i * 4 + 1] = static_cast<uint8_t>(ny * 255.0f);
        noisePixels[i * 4 + 2] = 0;
        noisePixels[i * 4 + 3] = 255;
    }

    TextureDesc desc{};
    desc.width = 4;
    desc.height = 4;
    desc.format = VK_FORMAT_R8G8B8A8_UNORM;
    desc.magFilter = VK_FILTER_NEAREST;
    desc.minFilter = VK_FILTER_NEAREST;
    desc.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    desc.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    desc.generateMipmaps = false;

    if (!allocator.createTexture2D(noisePixels, desc, noiseTex_)) {
        return false;
    }

    VkSamplerCreateInfo repSamp{};
    repSamp.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    repSamp.magFilter = VK_FILTER_NEAREST;
    repSamp.minFilter = VK_FILTER_NEAREST;
    repSamp.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    repSamp.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    repSamp.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    if (vkCreateSampler(device.device(), &repSamp, nullptr, &linearRepeatSampler_) != VK_SUCCESS) {
        return false;
    }

    return true;
}

bool PassSSAO::createPipelines(VkDevice device) {
    // 1. SSAO pipeline
    SceneVkDescriptorLayoutBuilder ssaoBuilder;
    ssaoBuilder.addBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    ssaoBuilder.addBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    ssaoBuilder.addBinding(2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    ssaoDescLayout_ = ssaoBuilder.build(device);
    if (!ssaoDescLayout_) return false;

    VkPipelineLayoutCreateInfo ssaoPlInfo{};
    ssaoPlInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    ssaoPlInfo.setLayoutCount = 1;
    ssaoPlInfo.pSetLayouts = &ssaoDescLayout_;
    if (vkCreatePipelineLayout(device, &ssaoPlInfo, nullptr, &ssaoPipelineLayout_) != VK_SUCCESS) return false;

    VkShaderModule vsMod = SceneVkShaderCompiler::createBuiltinModule(device, BuiltinSceneShader::PostFxVert);
    VkShaderModule ssaoFsMod = SceneVkShaderCompiler::createBuiltinModule(device, BuiltinSceneShader::SsaoFrag);
    if (!vsMod || !ssaoFsMod) {
        if (vsMod) SceneVkShaderCompiler::destroyModule(device, vsMod);
        if (ssaoFsMod) SceneVkShaderCompiler::destroyModule(device, ssaoFsMod);
        return false;
    }

    SceneVkPipelineBuilder b1;
    b1.setShaderStages(vsMod, ssaoFsMod)
      .setCullMode(VK_CULL_MODE_NONE)
      .disableBlending(1)
      .disableDepthTest()
      .setDynamicRendering({VK_FORMAT_R8_UNORM}, VK_FORMAT_UNDEFINED);
    ssaoPipeline_ = b1.build(device, ssaoPipelineLayout_);
    SceneVkShaderCompiler::destroyModule(device, ssaoFsMod);

    // 2. Blur pipeline
    SceneVkDescriptorLayoutBuilder blurBuilder;
    blurBuilder.addBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    blurDescLayout_ = blurBuilder.build(device);
    if (!blurDescLayout_) return false;

    VkPushConstantRange blurPushRange{};
    blurPushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    blurPushRange.offset = 0;
    blurPushRange.size = sizeof(float) * 4;

    VkPipelineLayoutCreateInfo blurPlInfo{};
    blurPlInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    blurPlInfo.setLayoutCount = 1;
    blurPlInfo.pSetLayouts = &blurDescLayout_;
    blurPlInfo.pushConstantRangeCount = 1;
    blurPlInfo.pPushConstantRanges = &blurPushRange;
    if (vkCreatePipelineLayout(device, &blurPlInfo, nullptr, &blurPipelineLayout_) != VK_SUCCESS) return false;

    VkShaderModule blurFsMod = SceneVkShaderCompiler::createBuiltinModule(device, BuiltinSceneShader::BlurFrag);
    SceneVkPipelineBuilder b2;
    b2.setShaderStages(vsMod, blurFsMod)
      .setCullMode(VK_CULL_MODE_NONE)
      .disableBlending(1)
      .disableDepthTest()
      .setDynamicRendering({VK_FORMAT_R8_UNORM}, VK_FORMAT_UNDEFINED);
    blurPipeline_ = b2.build(device, blurPipelineLayout_);

    SceneVkShaderCompiler::destroyModule(device, blurFsMod);

    // 3. Apply AO Pipeline (Multiplicative blend onto HDR target)
    SceneVkDescriptorLayoutBuilder applyAoBuilder;
    applyAoBuilder.addBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    applyAoDescLayout_ = applyAoBuilder.build(device);
    if (!applyAoDescLayout_) {
        SceneVkShaderCompiler::destroyModule(device, vsMod);
        return false;
    }

    VkPushConstantRange aoPushRange{};
    aoPushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    aoPushRange.offset = 0;
    aoPushRange.size = sizeof(float) * 4;

    VkPipelineLayoutCreateInfo aoPlInfo{};
    aoPlInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    aoPlInfo.setLayoutCount = 1;
    aoPlInfo.pSetLayouts = &applyAoDescLayout_;
    aoPlInfo.pushConstantRangeCount = 1;
    aoPlInfo.pPushConstantRanges = &aoPushRange;
    if (vkCreatePipelineLayout(device, &aoPlInfo, nullptr, &applyAoPipelineLayout_) != VK_SUCCESS) {
        SceneVkShaderCompiler::destroyModule(device, vsMod);
        return false;
    }

    VkShaderModule aoFsMod = SceneVkShaderCompiler::createBuiltinModule(device, BuiltinSceneShader::ApplyAoFrag);
    if (!aoFsMod) {
        SceneVkShaderCompiler::destroyModule(device, vsMod);
        return false;
    }

    SceneVkPipelineBuilder b3;
    b3.setShaderStages(vsMod, aoFsMod)
      .setCullMode(VK_CULL_MODE_NONE)
      .disableDepthTest()
      .setDynamicRendering({VK_FORMAT_R16G16B16A16_SFLOAT}, VK_FORMAT_UNDEFINED);

    VkPipelineColorBlendAttachmentState blendState{};
    blendState.blendEnable = VK_TRUE;
    blendState.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
    blendState.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR;
    blendState.colorBlendOp = VK_BLEND_OP_ADD;
    blendState.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blendState.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendState.alphaBlendOp = VK_BLEND_OP_ADD;
    blendState.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    b3.setColorBlendAttachment(0, blendState);

    applyAoPipeline_ = b3.build(device, applyAoPipelineLayout_);

    SceneVkShaderCompiler::destroyModule(device, vsMod);
    SceneVkShaderCompiler::destroyModule(device, aoFsMod);

    return ssaoPipeline_ != VK_NULL_HANDLE && blurPipeline_ != VK_NULL_HANDLE && applyAoPipeline_ != VK_NULL_HANDLE;
}

bool PassSSAO::createTargets(SceneVkAllocator& allocator, uint32_t width, uint32_t height) {
    aoWidth_ = std::max(1u, width / 2);
    aoHeight_ = std::max(1u, height / 2);

    for (int i = 0; i < 2; ++i) {
        if (!allocator.createImage(aoWidth_, aoHeight_, VK_FORMAT_R8_UNORM,
                                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                  ssaoTex_[i])) {
            LOG_ERROR("PassSSAO: Failed creating SSAO render target %d", i);
            return false;
        }
    }
    return true;
}

void PassSSAO::destroyTargets(SceneVkAllocator& allocator) {
    for (int i = 0; i < 2; ++i) {
        allocator.destroyImage(ssaoTex_[i]);
    }
    aoWidth_ = 0;
    aoHeight_ = 0;
}

void PassSSAO::render(VkCommandBuffer cmd, SceneVkDevice& device, SceneVkAllocator& allocator,
                      SceneVkDescriptorPool& descPool,
                      const SceneVkImage& depthImage,
                      const float* projMatrix,
                      const float* invProjMatrix,
                      float radius, float bias) {
    if (!ssaoTex_[0].isValid() || aoWidth_ == 0 || aoHeight_ == 0) return;

    SSAOUBOData uboData{};
    std::memcpy(uboData.proj, projMatrix, sizeof(uboData.proj));
    std::memcpy(uboData.invProj, invProjMatrix, sizeof(uboData.invProj));
    std::memcpy(uboData.kernel, kernel_, sizeof(uboData.kernel));
    uboData.params[0] = radius;
    uboData.params[1] = bias;
    uboData.params[2] = static_cast<float>(aoWidth_) / 4.0f;
    uboData.params[3] = static_cast<float>(aoHeight_) / 4.0f;
    allocator.updateUniformBuffer(ssaoUbo_, &uboData, sizeof(uboData));

    VkViewport vp{0.0f, 0.0f, static_cast<float>(aoWidth_), static_cast<float>(aoHeight_), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {aoWidth_, aoHeight_}};

    // 1. SSAO estimate -> ssaoTex_[0]
    allocator.transitionImageLayout(cmd, ssaoTex_[0].image, VK_FORMAT_R8_UNORM,
                                   ssaoTex_[0].currentLayout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    ssaoTex_[0].currentLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkDescriptorSet ssaoSet = descPool.allocate(ssaoDescLayout_);
    SceneVkDescriptorWriter writer;
    writer.writeImage(0, depthImage.view, pointClampSampler_);
    writer.writeImage(1, noiseTex_.view, linearRepeatSampler_);
    writer.writeBuffer(2, ssaoUbo_.buffer, sizeof(SSAOUBOData));
    writer.updateSet(device.device(), ssaoSet);

    VkRenderingAttachmentInfo attInfo{};
    attInfo.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    attInfo.imageView = ssaoTex_[0].view;
    attInfo.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attInfo.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attInfo.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo rInfo{};
    rInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rInfo.renderArea = {{0, 0}, {aoWidth_, aoHeight_}};
    rInfo.layerCount = 1;
    rInfo.colorAttachmentCount = 1;
    rInfo.pColorAttachments = &attInfo;

    vkCmdBeginRendering(cmd, &rInfo);
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ssaoPipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ssaoPipelineLayout_, 0, 1, &ssaoSet, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);

    // 2. Horizontal blur: ssaoTex_[0] -> ssaoTex_[1]
    allocator.transitionImageLayout(cmd, ssaoTex_[0].image, VK_FORMAT_R8_UNORM,
                                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    ssaoTex_[0].currentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    allocator.transitionImageLayout(cmd, ssaoTex_[1].image, VK_FORMAT_R8_UNORM,
                                   ssaoTex_[1].currentLayout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    ssaoTex_[1].currentLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkDescriptorSet blurSetH = descPool.allocate(blurDescLayout_);
    SceneVkDescriptorWriter writerH;
    writerH.writeImage(0, ssaoTex_[0].view, pointClampSampler_);
    writerH.updateSet(device.device(), blurSetH);

    attInfo.imageView = ssaoTex_[1].view;
    vkCmdBeginRendering(cmd, &rInfo);
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, blurPipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, blurPipelineLayout_, 0, 1, &blurSetH, 0, nullptr);
    float pushH[4] = {1.0f / static_cast<float>(aoWidth_), 0.0f, 0.0f, 0.0f};
    vkCmdPushConstants(cmd, blurPipelineLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pushH), pushH);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);

    // 3. Vertical blur: ssaoTex_[1] -> ssaoTex_[0]
    allocator.transitionImageLayout(cmd, ssaoTex_[1].image, VK_FORMAT_R8_UNORM,
                                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    ssaoTex_[1].currentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    allocator.transitionImageLayout(cmd, ssaoTex_[0].image, VK_FORMAT_R8_UNORM,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    ssaoTex_[0].currentLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkDescriptorSet blurSetV = descPool.allocate(blurDescLayout_);
    SceneVkDescriptorWriter writerV;
    writerV.writeImage(0, ssaoTex_[1].view, pointClampSampler_);
    writerV.updateSet(device.device(), blurSetV);

    attInfo.imageView = ssaoTex_[0].view;
    vkCmdBeginRendering(cmd, &rInfo);
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, blurPipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, blurPipelineLayout_, 0, 1, &blurSetV, 0, nullptr);
    float pushV[4] = {0.0f, 1.0f / static_cast<float>(aoHeight_), 0.0f, 0.0f};
    vkCmdPushConstants(cmd, blurPipelineLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pushV), pushV);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);

    allocator.transitionImageLayout(cmd, ssaoTex_[0].image, VK_FORMAT_R8_UNORM,
                                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    ssaoTex_[0].currentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

void PassSSAO::applyAO(VkCommandBuffer cmd, SceneVkDevice& device, SceneVkAllocator& allocator,
                       SceneVkDescriptorPool& descPool,
                       VkImageView hdrTargetView,
                       uint32_t width, uint32_t height,
                       float intensity) {
    (void)allocator;
    if (applyAoPipeline_ == VK_NULL_HANDLE || !ssaoTex_[0].isValid() || intensity <= 0.0f) return;

    VkDescriptorSet dSet = descPool.allocate(applyAoDescLayout_);
    if (dSet == VK_NULL_HANDLE) return;

    SceneVkDescriptorWriter writer;
    writer.writeImage(0, ssaoTex_[0].view, pointClampSampler_);
    writer.updateSet(device.device(), dSet);

    VkRenderingAttachmentInfo attInfo{};
    attInfo.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    attInfo.imageView = hdrTargetView;
    attInfo.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attInfo.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attInfo.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo rInfo{};
    rInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rInfo.renderArea = {{0, 0}, {width, height}};
    rInfo.layerCount = 1;
    rInfo.colorAttachmentCount = 1;
    rInfo.pColorAttachments = &attInfo;

    vkCmdBeginRendering(cmd, &rInfo);

    VkViewport vp{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {width, height}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, applyAoPipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, applyAoPipelineLayout_, 0, 1, &dSet, 0, nullptr);

    float pushData[4] = {intensity, 0.0f, 0.0f, 0.0f};
    vkCmdPushConstants(cmd, applyAoPipelineLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pushData), pushData);

    vkCmdDraw(cmd, 3, 1, 0, 0);

    vkCmdEndRendering(cmd);
}

} // namespace bro::scene::vk
