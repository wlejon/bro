#include "scene/vulkan/pass_postfx.h"
#include "scene/vulkan/scene_vk_shader_compiler.h"
#include "util/log.h"

#include <algorithm>
#include <cstring>

namespace bro::scene::vk {

struct alignas(16) PostFxBloomPush {
    float threshold;
    float knee;
    float blurRadius;
    int32_t passType;
};

struct alignas(16) PostFxTonemapPush {
    float exposure;
    float gamma;
    float bloomIntensity;
    int32_t tonemapMode;
    int32_t enableFxaa;
    float texelSizeX;
    float texelSizeY;
    float padding;
};

struct alignas(16) PostFxFxaaPush {
    float texelSizeX;
    float texelSizeY;
};

PassPostFx::~PassPostFx() {
}

bool PassPostFx::init(SceneVkDevice& device, SceneVkAllocator& allocator,
                     uint32_t width, uint32_t height) {
    return init(device, allocator, width, height, Config{});
}

bool PassPostFx::init(SceneVkDevice& device, SceneVkAllocator& allocator,
                     uint32_t width, uint32_t height,
                     const Config& config) {
    config_ = config;
    width_ = width;
    height_ = height;
    VkDevice dev = device.device();

    // 1. Single texture descriptor layout (for Bloom and FXAA)
    SceneVkDescriptorLayoutBuilder singleBuilder;
    singleBuilder.addBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    singleTexDescLayout_ = singleBuilder.build(dev);
    if (!singleTexDescLayout_) return false;

    // 2. Tonemap descriptor layout (Set 0: binding 0 = HDR, binding 1 = Bloom)
    SceneVkDescriptorLayoutBuilder tmBuilder;
    tmBuilder.addBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    tmBuilder.addBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    tonemapDescLayout_ = tmBuilder.build(dev);
    if (!tonemapDescLayout_) return false;

    // 3. Pipeline layouts
    VkPushConstantRange singlePush{};
    singlePush.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    singlePush.offset = 0;
    singlePush.size = sizeof(PostFxBloomPush);

    VkPipelineLayoutCreateInfo singleLayoutInfo{};
    singleLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    singleLayoutInfo.setLayoutCount = 1;
    singleLayoutInfo.pSetLayouts = &singleTexDescLayout_;
    singleLayoutInfo.pushConstantRangeCount = 1;
    singleLayoutInfo.pPushConstantRanges = &singlePush;

    if (vkCreatePipelineLayout(dev, &singleLayoutInfo, nullptr, &singleTexLayout_) != VK_SUCCESS) {
        LOG_ERROR("PassPostFx: Failed creating singleTexLayout");
        return false;
    }

    VkPushConstantRange tmPush{};
    tmPush.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    tmPush.offset = 0;
    tmPush.size = sizeof(PostFxTonemapPush);

    VkPipelineLayoutCreateInfo tmLayoutInfo{};
    tmLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    tmLayoutInfo.setLayoutCount = 1;
    tmLayoutInfo.pSetLayouts = &tonemapDescLayout_;
    tmLayoutInfo.pushConstantRangeCount = 1;
    tmLayoutInfo.pPushConstantRanges = &tmPush;

    if (vkCreatePipelineLayout(dev, &tmLayoutInfo, nullptr, &tonemapLayout_) != VK_SUCCESS) {
        LOG_ERROR("PassPostFx: Failed creating tonemapLayout");
        return false;
    }

    // 4. Create linear clamp sampler
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

    if (vkCreateSampler(dev, &samplerInfo, nullptr, &linearSampler_) != VK_SUCCESS) {
        LOG_ERROR("PassPostFx: Failed creating linear sampler");
        return false;
    }

    // 5. Create descriptor pool & sets
    if (!descPool_.init(dev, 8)) {
        LOG_ERROR("PassPostFx: Failed creating descriptor pool");
        return false;
    }
    bloomExtractSet_ = descPool_.allocate(singleTexDescLayout_);
    bloomBlurSet_    = descPool_.allocate(singleTexDescLayout_);
    tonemapSet_      = descPool_.allocate(tonemapDescLayout_);
    fxaaSet_         = descPool_.allocate(singleTexDescLayout_);

    // 6. Create pipelines
    if (!createPipelines(dev, config)) {
        LOG_ERROR("PassPostFx: Failed creating pipelines");
        return false;
    }

    // 7. Create intermediate targets
    if (!createIntermediateTargets(allocator, width, height)) {
        LOG_ERROR("PassPostFx: Failed creating intermediate targets");
        return false;
    }

    return true;
}

bool PassPostFx::createPipelines(VkDevice device, const Config& config) {
    VkShaderModule vs       = SceneVkShaderCompiler::createBuiltinModule(device, BuiltinSceneShader::PostFxVert);
    VkShaderModule fsBloom  = SceneVkShaderCompiler::createBuiltinModule(device, BuiltinSceneShader::BloomFrag);
    VkShaderModule fsTonemap = SceneVkShaderCompiler::createBuiltinModule(device, BuiltinSceneShader::TonemapFrag);
    VkShaderModule fsFxaa   = SceneVkShaderCompiler::createBuiltinModule(device, BuiltinSceneShader::FxaaFrag);

    if (!vs || !fsBloom || !fsTonemap || !fsFxaa) {
        LOG_ERROR("PassPostFx: Failed loading postfx shaders");
        return false;
    }

    // 1. Bloom Extraction Pipeline
    SceneVkPipelineBuilder bBloomExtract;
    bBloomExtract.addShaderStage(VK_SHADER_STAGE_VERTEX_BIT, vs)
                 .addShaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, fsBloom)
                 .setInputTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
                 .setPolygonMode(VK_POLYGON_MODE_FILL)
                 .setCullMode(VK_CULL_MODE_NONE)
                 .setMultisamplingNone()
                 .disableBlending(1)
                 .disableDepthTest()
                 .setDynamicRendering({config.hdrFormat});

    pipelineBloomExtract_ = bBloomExtract.build(device, singleTexLayout_);

    // 2. Bloom Blur Pipeline
    SceneVkPipelineBuilder bBloomBlur;
    bBloomBlur.addShaderStage(VK_SHADER_STAGE_VERTEX_BIT, vs)
              .addShaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, fsBloom)
              .setInputTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
              .setPolygonMode(VK_POLYGON_MODE_FILL)
              .setCullMode(VK_CULL_MODE_NONE)
              .setMultisamplingNone()
              .disableBlending(1)
              .disableDepthTest()
              .setDynamicRendering({config.hdrFormat});

    pipelineBloomBlur_ = bBloomBlur.build(device, singleTexLayout_);

    // 3. Tonemapping Pipeline
    SceneVkPipelineBuilder bTonemap;
    bTonemap.addShaderStage(VK_SHADER_STAGE_VERTEX_BIT, vs)
            .addShaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, fsTonemap)
            .setInputTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
            .setPolygonMode(VK_POLYGON_MODE_FILL)
            .setCullMode(VK_CULL_MODE_NONE)
            .setMultisamplingNone()
            .disableBlending(1)
            .disableDepthTest()
            .setDynamicRendering({config.presentationFormat});

    pipelineTonemap_ = bTonemap.build(device, tonemapLayout_);

    // 4. FXAA Pipeline
    SceneVkPipelineBuilder bFxaa;
    bFxaa.addShaderStage(VK_SHADER_STAGE_VERTEX_BIT, vs)
         .addShaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, fsFxaa)
         .setInputTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
         .setPolygonMode(VK_POLYGON_MODE_FILL)
         .setCullMode(VK_CULL_MODE_NONE)
         .setMultisamplingNone()
         .disableBlending(1)
         .disableDepthTest()
         .setDynamicRendering({config.presentationFormat});

    pipelineFxaa_ = bFxaa.build(device, singleTexLayout_);

    SceneVkShaderModule::destroy(device, vs);
    SceneVkShaderModule::destroy(device, fsBloom);
    SceneVkShaderModule::destroy(device, fsTonemap);
    SceneVkShaderModule::destroy(device, fsFxaa);

    return (pipelineBloomExtract_ && pipelineBloomBlur_ && pipelineTonemap_ && pipelineFxaa_);
}

bool PassPostFx::createIntermediateTargets(SceneVkAllocator& allocator, uint32_t width, uint32_t height) {
    uint32_t halfW = std::max(1u, width / 2);
    uint32_t halfH = std::max(1u, height / 2);

    VkImageUsageFlags hdrUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!allocator.createImage(halfW, halfH, config_.hdrFormat, hdrUsage,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, bloomExtractImage_)) {
        return false;
    }
    if (!allocator.createImage(halfW, halfH, config_.hdrFormat, hdrUsage,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, bloomBlurImage_)) {
        return false;
    }

    VkImageUsageFlags ldrUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!allocator.createImage(width, height, config_.presentationFormat, ldrUsage,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, intermediateLdrImage_)) {
        return false;
    }

    return true;
}

void PassPostFx::destroyIntermediateTargets(SceneVkAllocator& allocator) {
    allocator.destroyImage(bloomExtractImage_);
    allocator.destroyImage(bloomBlurImage_);
    allocator.destroyImage(intermediateLdrImage_);
}

bool PassPostFx::resize(SceneVkDevice& device, SceneVkAllocator& allocator, uint32_t width, uint32_t height) {
    if (width == width_ && height == height_) return true;
    width_ = width;
    height_ = height;
    destroyIntermediateTargets(allocator);
    return createIntermediateTargets(allocator, width, height);
}

void PassPostFx::cleanup(SceneVkDevice& device, SceneVkAllocator& allocator) {
    VkDevice dev = device.device();

    destroyIntermediateTargets(allocator);

    if (pipelineBloomExtract_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, pipelineBloomExtract_, nullptr);
        pipelineBloomExtract_ = VK_NULL_HANDLE;
    }
    if (pipelineBloomBlur_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, pipelineBloomBlur_, nullptr);
        pipelineBloomBlur_ = VK_NULL_HANDLE;
    }
    if (pipelineTonemap_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, pipelineTonemap_, nullptr);
        pipelineTonemap_ = VK_NULL_HANDLE;
    }
    if (pipelineFxaa_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, pipelineFxaa_, nullptr);
        pipelineFxaa_ = VK_NULL_HANDLE;
    }

    if (singleTexLayout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(dev, singleTexLayout_, nullptr);
        singleTexLayout_ = VK_NULL_HANDLE;
    }
    if (tonemapLayout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(dev, tonemapLayout_, nullptr);
        tonemapLayout_ = VK_NULL_HANDLE;
    }

    descPool_.destroy();

    if (linearSampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(dev, linearSampler_, nullptr);
        linearSampler_ = VK_NULL_HANDLE;
    }

    if (singleTexDescLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, singleTexDescLayout_, nullptr);
        singleTexDescLayout_ = VK_NULL_HANDLE;
    }
    if (tonemapDescLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, tonemapDescLayout_, nullptr);
        tonemapDescLayout_ = VK_NULL_HANDLE;
    }
}

void PassPostFx::render(VkCommandBuffer cmd, SceneVkDevice& device, SceneVkAllocator& allocator,
                       const SceneVkImage& hdrSceneImage,
                       VkImageView presentationTargetView,
                       VkFormat presentationFormat,
                       uint32_t width, uint32_t height,
                       const PostFxParams& params) {
    if (width != width_ || height != height_) {
        resize(device, allocator, width, height);
    }

    uint32_t halfW = std::max(1u, width / 2);
    uint32_t halfH = std::max(1u, height / 2);

    VkDevice dev = device.device();

    // 1. Bloom Extraction Pass (if enabled)
    if (params.enableBloom && params.bloomIntensity > 0.0f) {
        // Transition bloomExtractImage_ to COLOR_ATTACHMENT_OPTIMAL
        allocator.transitionImageLayout(cmd, bloomExtractImage_.image, config_.hdrFormat,
                                       bloomExtractImage_.currentLayout,
                                       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        bloomExtractImage_.currentLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        // Bind HDR scene image to bloomExtractSet_
        SceneVkDescriptorWriter writer;
        writer.writeImage(0, hdrSceneImage.view, linearSampler_);
        writer.updateSet(dev, bloomExtractSet_);

        VkRenderingAttachmentInfoKHR colorAttach{};
        colorAttach.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
        colorAttach.imageView = bloomExtractImage_.view;
        colorAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttach.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

        VkRenderingInfoKHR renderInfo{};
        renderInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO_KHR;
        renderInfo.renderArea = {{0, 0}, {halfW, halfH}};
        renderInfo.layerCount = 1;
        renderInfo.colorAttachmentCount = 1;
        renderInfo.pColorAttachments = &colorAttach;

        device.cmdBeginRendering(cmd, &renderInfo);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineBloomExtract_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, singleTexLayout_, 0, 1, &bloomExtractSet_, 0, nullptr);

        VkViewport vp{0.0f, 0.0f, static_cast<float>(halfW), static_cast<float>(halfH), 0.0f, 1.0f};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        VkRect2D sc{{0, 0}, {halfW, halfH}};
        vkCmdSetScissor(cmd, 0, 1, &sc);

        PostFxBloomPush bloomPush{};
        bloomPush.threshold = params.bloomThreshold;
        bloomPush.knee = params.bloomKnee;
        bloomPush.blurRadius = 0.0f;
        bloomPush.passType = 0; // threshold
        vkCmdPushConstants(cmd, singleTexLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(bloomPush), &bloomPush);

        vkCmdDraw(cmd, 3, 1, 0, 0);
        device.cmdEndRendering(cmd);

        // Transition bloomExtract to SHADER_READ_ONLY_OPTIMAL
        allocator.transitionImageLayout(cmd, bloomExtractImage_.image, config_.hdrFormat,
                                       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        bloomExtractImage_.currentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        // Transition bloomBlur to COLOR_ATTACHMENT_OPTIMAL
        allocator.transitionImageLayout(cmd, bloomBlurImage_.image, config_.hdrFormat,
                                       bloomBlurImage_.currentLayout,
                                       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        bloomBlurImage_.currentLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        // Blur pass
        SceneVkDescriptorWriter blurWriter;
        blurWriter.writeImage(0, bloomExtractImage_.view, linearSampler_);
        blurWriter.updateSet(dev, bloomBlurSet_);

        colorAttach.imageView = bloomBlurImage_.view;
        device.cmdBeginRendering(cmd, &renderInfo);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineBloomBlur_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, singleTexLayout_, 0, 1, &bloomBlurSet_, 0, nullptr);

        bloomPush.blurRadius = 1.0f / static_cast<float>(halfW);
        bloomPush.passType = 1; // gaussian blur
        vkCmdPushConstants(cmd, singleTexLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(bloomPush), &bloomPush);

        vkCmdDraw(cmd, 3, 1, 0, 0);
        device.cmdEndRendering(cmd);

        // Transition bloomBlur to SHADER_READ_ONLY_OPTIMAL
        allocator.transitionImageLayout(cmd, bloomBlurImage_.image, config_.hdrFormat,
                                       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        bloomBlurImage_.currentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

    // 2. Tonemapping Pass
    // Target is either intermediateLdrImage_ (if FXAA is active) or presentationTargetView
    bool runFxaa = params.enableFxaa;
    VkImageView tmTargetView = runFxaa ? intermediateLdrImage_.view : presentationTargetView;

    if (runFxaa) {
        allocator.transitionImageLayout(cmd, intermediateLdrImage_.image, config_.presentationFormat,
                                       intermediateLdrImage_.currentLayout,
                                       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        intermediateLdrImage_.currentLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    // Update tonemap descriptor set: binding 0 = HDR, binding 1 = Bloom (or dummy/fallback)
    VkImageView bloomView = (params.enableBloom && params.bloomIntensity > 0.0f) ? bloomBlurImage_.view : hdrSceneImage.view;
    SceneVkDescriptorWriter tmWriter;
    tmWriter.writeImage(0, hdrSceneImage.view, linearSampler_);
    tmWriter.writeImage(1, bloomView, linearSampler_);
    tmWriter.updateSet(dev, tonemapSet_);

    VkRenderingAttachmentInfoKHR tmColorAttach{};
    tmColorAttach.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
    tmColorAttach.imageView = tmTargetView;
    tmColorAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    tmColorAttach.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    tmColorAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfoKHR tmRenderInfo{};
    tmRenderInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO_KHR;
    tmRenderInfo.renderArea = {{0, 0}, {width, height}};
    tmRenderInfo.layerCount = 1;
    tmRenderInfo.colorAttachmentCount = 1;
    tmRenderInfo.pColorAttachments = &tmColorAttach;

    device.cmdBeginRendering(cmd, &tmRenderInfo);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineTonemap_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tonemapLayout_, 0, 1, &tonemapSet_, 0, nullptr);

    VkViewport fullVp{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &fullVp);
    VkRect2D fullSc{{0, 0}, {width, height}};
    vkCmdSetScissor(cmd, 0, 1, &fullSc);

    PostFxTonemapPush tmPush{};
    tmPush.exposure = params.exposure;
    tmPush.gamma = params.gamma;
    tmPush.bloomIntensity = (params.enableBloom ? params.bloomIntensity : 0.0f);
    tmPush.tonemapMode = static_cast<int32_t>(params.tonemapMode);
    tmPush.enableFxaa = runFxaa ? 1 : 0;
    tmPush.texelSizeX = 1.0f / static_cast<float>(width);
    tmPush.texelSizeY = 1.0f / static_cast<float>(height);
    vkCmdPushConstants(cmd, tonemapLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(tmPush), &tmPush);

    vkCmdDraw(cmd, 3, 1, 0, 0);
    device.cmdEndRendering(cmd);

    // 3. FXAA Pass (if enabled)
    if (runFxaa) {
        allocator.transitionImageLayout(cmd, intermediateLdrImage_.image, config_.presentationFormat,
                                       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        intermediateLdrImage_.currentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        SceneVkDescriptorWriter fxaaWriter;
        fxaaWriter.writeImage(0, intermediateLdrImage_.view, linearSampler_);
        fxaaWriter.updateSet(dev, fxaaSet_);

        VkRenderingAttachmentInfoKHR fxaaAttach{};
        fxaaAttach.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
        fxaaAttach.imageView = presentationTargetView;
        fxaaAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        fxaaAttach.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        fxaaAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

        VkRenderingInfoKHR fxaaRenderInfo{};
        fxaaRenderInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO_KHR;
        fxaaRenderInfo.renderArea = {{0, 0}, {width, height}};
        fxaaRenderInfo.layerCount = 1;
        fxaaRenderInfo.colorAttachmentCount = 1;
        fxaaRenderInfo.pColorAttachments = &fxaaAttach;

        device.cmdBeginRendering(cmd, &fxaaRenderInfo);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineFxaa_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, singleTexLayout_, 0, 1, &fxaaSet_, 0, nullptr);

        vkCmdSetViewport(cmd, 0, 1, &fullVp);
        vkCmdSetScissor(cmd, 0, 1, &fullSc);

        PostFxFxaaPush fxaaPush{};
        fxaaPush.texelSizeX = 1.0f / static_cast<float>(width);
        fxaaPush.texelSizeY = 1.0f / static_cast<float>(height);
        vkCmdPushConstants(cmd, singleTexLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(fxaaPush), &fxaaPush);

        vkCmdDraw(cmd, 3, 1, 0, 0);
        device.cmdEndRendering(cmd);
    }
}

} // namespace bro::scene::vk
