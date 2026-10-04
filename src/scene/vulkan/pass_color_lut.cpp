#include "scene/vulkan/pass_color_lut.h"
#include "scene/vulkan/scene_vk_shader_compiler.h"
#include "util/log.h"

namespace bro::scene::vk {

PassColorLut::~PassColorLut() = default;

bool PassColorLut::init(SceneVkDevice& device, SceneVkAllocator& allocator) {
    (void)allocator;
    return createPipeline(device.device());
}

void PassColorLut::cleanup(SceneVkDevice& device, SceneVkAllocator& allocator) {
    clearLut(allocator);

    VkDevice dev = device.device();
    if (pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, pipeline_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
    }
    if (pipelineLayout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(dev, pipelineLayout_, nullptr);
        pipelineLayout_ = VK_NULL_HANDLE;
    }
    if (descLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, descLayout_, nullptr);
        descLayout_ = VK_NULL_HANDLE;
    }
}

bool PassColorLut::createPipeline(VkDevice device) {
    SceneVkDescriptorLayoutBuilder descBuilder;
    descBuilder.addBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    descBuilder.addBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    descLayout_ = descBuilder.build(device);
    if (!descLayout_) {
        LOG_ERROR("PassColorLut: Failed creating descriptor set layout");
        return false;
    }

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(float) * 4;

    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts = &descLayout_;
    plInfo.pushConstantRangeCount = 1;
    plInfo.pPushConstantRanges = &pushRange;

    if (vkCreatePipelineLayout(device, &plInfo, nullptr, &pipelineLayout_) != VK_SUCCESS) {
        LOG_ERROR("PassColorLut: Failed creating pipeline layout");
        return false;
    }

    VkShaderModule vsModule = SceneVkShaderCompiler::createBuiltinModule(device, BuiltinSceneShader::PostFxVert);
    VkShaderModule fsModule = SceneVkShaderCompiler::createBuiltinModule(device, BuiltinSceneShader::ColorLutFrag);
    if (!vsModule || !fsModule) {
        LOG_ERROR("PassColorLut: Failed compiling builtin shaders");
        if (vsModule) SceneVkShaderCompiler::destroyModule(device, vsModule);
        if (fsModule) SceneVkShaderCompiler::destroyModule(device, fsModule);
        return false;
    }

    SceneVkPipelineBuilder builder;
    builder.setShaderStages(vsModule, fsModule)
           .setCullMode(VK_CULL_MODE_NONE)
           .disableBlending(1)
           .disableDepthTest()
           .setDynamicRendering({VK_FORMAT_R8G8B8A8_UNORM}, VK_FORMAT_UNDEFINED);

    pipeline_ = builder.build(device, pipelineLayout_);

    SceneVkShaderCompiler::destroyModule(device, vsModule);
    SceneVkShaderCompiler::destroyModule(device, fsModule);

    if (pipeline_ == VK_NULL_HANDLE) {
        LOG_ERROR("PassColorLut: Failed building pipeline");
        return false;
    }

    return true;
}

bool PassColorLut::updateLut(SceneVkDevice& device, SceneVkAllocator& allocator, int size, const uint8_t* rgbaVoxels) {
    (void)device;
    clearLut(allocator);
    lutSize_ = size;
    return allocator.createTexture3D(rgbaVoxels, size, VK_FORMAT_R8G8B8A8_UNORM, lutImage_);
}

void PassColorLut::clearLut(SceneVkAllocator& allocator) {
    if (lutImage_.isValid()) {
        allocator.destroyImage(lutImage_);
    }
    lutSize_ = 0;
}

void PassColorLut::render(VkCommandBuffer cmd, SceneVkDevice& device, SceneVkAllocator& allocator,
                          const SceneVkImage& inputImage,
                          VkImageView outputTargetView,
                          VkFormat outputFormat,
                          uint32_t width, uint32_t height,
                          float amount) {
    (void)allocator;
    if (!lutImage_.isValid() || lutSize_ <= 1 || amount <= 0.0f) return;

    VkDescriptorSet dSet = device.frameSet(descLayout_);
    if (dSet == VK_NULL_HANDLE) return;

    SceneVkDescriptorWriter writer;
    writer.writeImage(0, inputImage.view, inputImage.sampler);
    writer.writeImage(1, lutImage_.view, lutImage_.sampler);
    writer.updateSet(device.device(), dSet);

    VkRenderingAttachmentInfo colorAttachment{};
    colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAttachment.imageView = outputTargetView;
    colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo renderInfo{};
    renderInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    renderInfo.renderArea = {{0, 0}, {width, height}};
    renderInfo.layerCount = 1;
    renderInfo.colorAttachmentCount = 1;
    renderInfo.pColorAttachments = &colorAttachment;

    vkCmdBeginRendering(cmd, &renderInfo);

    VkViewport vp{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {width, height}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1, &dSet, 0, nullptr);

    float scale = (static_cast<float>(lutSize_) - 1.0f) / static_cast<float>(lutSize_);
    float offset = 0.5f / static_cast<float>(lutSize_);
    float pushData[4] = {amount, scale, offset, 0.0f};
    vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pushData), pushData);

    vkCmdDraw(cmd, 3, 1, 0, 0);

    vkCmdEndRendering(cmd);
}

} // namespace bro::scene::vk
