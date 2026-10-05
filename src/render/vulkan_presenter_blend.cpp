// VulkanPresenter's blended layers: each GPU image over what is below it, and
// the CPU layer above each over that, drawn from per-frame-slot textures as
// premultiplied-alpha textured triangles under dynamic rendering.

#include "render/vulkan_presenter.h"
#include "render/pixel_convert.h"
#include "render/vulkan_util.h"
#include "util/log.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace bro::render {

namespace {

// Built from src/render/shaders/ by bro_spirv_embed.
const uint32_t kPresentQuadVertSpv[] =
#include "present_quad.vert.spv.h"
;
const uint32_t kPresentOverlayFragSpv[] =
#include "present_overlay.frag.spv.h"
;

} // namespace

bool VulkanPresenter::initBlendResources() {
    VkDevice device = context_.device();

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;  // drawn 1:1
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(device, &samplerInfo, nullptr, &blendSampler_) != VK_SUCCESS) {
        LOG_ERROR("VulkanPresenter: Failed to create blend sampler");
        return false;
    }
    samplerInfo.magFilter = VK_FILTER_LINEAR;  // an image drawn at another size
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    if (vkCreateSampler(device, &samplerInfo, nullptr, &blendSamplerLinear_) != VK_SUCCESS) {
        LOG_ERROR("VulkanPresenter: Failed to create blend sampler");
        return false;
    }

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &blendSetLayout_) != VK_SUCCESS) {
        LOG_ERROR("VulkanPresenter: Failed to create blend descriptor set layout");
        return false;
    }

    VkPipelineLayoutCreateInfo pipeLayoutInfo{};
    pipeLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeLayoutInfo.setLayoutCount = 1;
    pipeLayoutInfo.pSetLayouts = &blendSetLayout_;
    if (vkCreatePipelineLayout(device, &pipeLayoutInfo, nullptr, &blendPipelineLayout_) != VK_SUCCESS) {
        LOG_ERROR("VulkanPresenter: Failed to create blend pipeline layout");
        return false;
    }
    return true;
}

void VulkanPresenter::destroyBlendResources() {
    VkDevice device = context_.device();
    for (auto& [format, pipeline] : blendPipelines_) vkDestroyPipeline(device, pipeline, nullptr);
    blendPipelines_.clear();
    for (auto& slot : aboveTex_)
        for (auto& tex : slot) destroyImageNow(tex);
    for (auto& slot : imageTex_)
        for (auto& tex : slot) destroyImageNow(tex);
    if (blendPipelineLayout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, blendPipelineLayout_, nullptr);
    if (blendSetLayout_ != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, blendSetLayout_, nullptr);
    if (blendSampler_ != VK_NULL_HANDLE) vkDestroySampler(device, blendSampler_, nullptr);
    if (blendSamplerLinear_ != VK_NULL_HANDLE) vkDestroySampler(device, blendSamplerLinear_, nullptr);
    blendPipelineLayout_ = VK_NULL_HANDLE;
    blendSetLayout_ = VK_NULL_HANDLE;
    blendSampler_ = VK_NULL_HANDLE;
    blendSamplerLinear_ = VK_NULL_HANDLE;
}

VkPipeline VulkanPresenter::blendPipeline(VkFormat targetFormat) {
    auto it = blendPipelines_.find(targetFormat);
    if (it != blendPipelines_.end()) return it->second;

    VkDevice device = context_.device();
    VkShaderModule modules[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    const uint32_t* code[2] = {kPresentQuadVertSpv, kPresentOverlayFragSpv};
    const size_t size[2] = {sizeof(kPresentQuadVertSpv), sizeof(kPresentOverlayFragSpv)};
    for (int i = 0; i < 2; ++i) {
        VkShaderModuleCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = size[i];
        info.pCode = code[i];
        if (vkCreateShaderModule(device, &info, nullptr, &modules[i]) != VK_SUCCESS) {
            LOG_ERROR("VulkanPresenter: Failed to create blend shader module");
            if (modules[0]) vkDestroyShaderModule(device, modules[0], nullptr);
            return VK_NULL_HANDLE;
        }
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    for (int i = 0; i < 2; ++i) {
        stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[i].stage = i == 0 ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[i].module = modules[i];
        stages[i].pName = "main";
    }

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.lineWidth = 1.0f;
    raster.cullMode = VK_CULL_MODE_NONE;
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;  // premultiplied
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blend;

    const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;

    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &targetFormat;

    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.pNext = &rendering;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &inputAssembly;
    info.pViewportState = &viewportState;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pColorBlendState = &colorBlend;
    info.pDynamicState = &dynamic;
    info.layout = blendPipelineLayout_;

    VkPipeline pipeline = VK_NULL_HANDLE;
    VkResult res = vkCreateGraphicsPipelines(device, context_.pipelineCache(), 1, &info, nullptr, &pipeline);
    vkDestroyShaderModule(device, modules[0], nullptr);
    vkDestroyShaderModule(device, modules[1], nullptr);
    if (res != VK_SUCCESS) {
        LOG_ERROR("VulkanPresenter: Failed to create blend pipeline (%d)", res);
        return VK_NULL_HANDLE;
    }
    blendPipelines_[targetFormat] = pipeline;
    return pipeline;
}

// This slot's texture `index` from `ring`, at the given size and format. A
// second use in one frame (a second present) must not overwrite the first's
// texture before the GPU has read it, so it gets a fresh one (the old one is
// retired).
VulkanPresenter::Image* VulkanPresenter::slotTexture(TextureRing& ring, size_t index, uint32_t width,
                                                     uint32_t height, VkFormat format) {
    auto& frames = context_.frames();
    std::vector<Image>& slot = ring[frames.frameIndex()];
    if (slot.size() <= index) slot.resize(index + 1);
    Image& tex = slot[index];
    if (tex.lastUseSerial == frames.frameSerial()) retireImage(tex);
    constexpr VkImageUsageFlags kUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!ensureImage(tex, width, height, format, kUsage)) return nullptr;
    tex.lastUseSerial = frames.frameSerial();
    return &tex;
}

bool VulkanPresenter::describeTexture(const Image& tex, VkSampler sampler, BlendDraw& out) {
    out.set = context_.frames().allocDescriptorSet(blendSetLayout_);
    if (out.set == VK_NULL_HANDLE) return false;
    VkDescriptorImageInfo imageInfo{sampler, tex.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = out.set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(context_.device(), 1, &write, 0, nullptr);
    return true;
}

namespace {

// `r` cut to the target; empty when they do not meet.
VkRect2D clampToTarget(int64_t x0, int64_t y0, int64_t x1, int64_t y1, uint32_t targetW, uint32_t targetH) {
    x0 = std::clamp<int64_t>(x0, 0, targetW);
    y0 = std::clamp<int64_t>(y0, 0, targetH);
    x1 = std::clamp<int64_t>(x1, x0, targetW);
    y1 = std::clamp<int64_t>(y1, y0, targetH);
    return {{static_cast<int32_t>(x0), static_cast<int32_t>(y0)},
            {static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(y1 - y0)}};
}

} // namespace

// The CPU layer into this slot's texture, in its own byte order (a BGRA
// texture samples as RGBA, so there is no swizzle), drawn 1:1 at the top-left.
bool VulkanPresenter::uploadLayerTexture(VkCommandBuffer cmd, const PresentPixels& layer, size_t index,
                                         const Target& target, BlendDraw& out) {
    const VkFormat format = layer.bgra ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
    Image* tex = slotTexture(aboveTex_, index, layer.width, layer.height, format);
    UploadSlice staging = context_.frames().allocUpload(static_cast<VkDeviceSize>(layer.width) * layer.height * 4);
    if (!tex || !staging || !describeTexture(*tex, blendSampler_, out)) return false;
    out.dst = {0.0f, 0.0f, static_cast<float>(layer.width), static_cast<float>(layer.height), 0.0f, 1.0f};
    out.scissor = clampToTarget(0, 0, layer.width, layer.height, target.width, target.height);

    copyPixels32(staging.mapped, static_cast<size_t>(layer.width) * 4, layer.pixels,
                 layer.stride ? layer.stride : static_cast<size_t>(layer.width) * 4,
                 layer.width, layer.height, /*swapRB=*/false);
    cmdTransitionImage(cmd, tex->image, colorRange(), VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy region{};
    region.bufferOffset = staging.offset;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {layer.width, layer.height, 1};
    vkCmdCopyBufferToImage(cmd, staging.buffer, tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    cmdTransitionImage(cmd, tex->image, colorRange(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return true;
}

// A GPU image blitted into this slot's texture, so it can be sampled whatever
// its own usage flags and format, and drawn into its destination rectangle.
bool VulkanPresenter::copyImageTexture(VkCommandBuffer cmd, const PresentImage& image, size_t index,
                                       const Target& target, BlendDraw& out) {
    Image* tex = slotTexture(imageTex_, index, image.width, image.height, VK_FORMAT_R8G8B8A8_UNORM);
    const bool scaled = image.dstW != static_cast<float>(image.width) ||
                        image.dstH != static_cast<float>(image.height);
    if (!tex || !describeTexture(*tex, scaled ? blendSamplerLinear_ : blendSampler_, out)) return false;
    out.dst = {image.dstX, image.dstY, image.dstW, image.dstH, 0.0f, 1.0f};
    int64_t x0 = static_cast<int64_t>(std::floor(image.dstX));
    int64_t y0 = static_cast<int64_t>(std::floor(image.dstY));
    int64_t x1 = static_cast<int64_t>(std::ceil(image.dstX + image.dstW));
    int64_t y1 = static_cast<int64_t>(std::ceil(image.dstY + image.dstH));
    if (image.clipped) {
        x0 = std::max<int64_t>(x0, image.clip.offset.x);
        y0 = std::max<int64_t>(y0, image.clip.offset.y);
        x1 = std::min<int64_t>(x1, int64_t{image.clip.offset.x} + image.clip.extent.width);
        y1 = std::min<int64_t>(y1, int64_t{image.clip.offset.y} + image.clip.extent.height);
    }
    out.scissor = clampToTarget(x0, y0, x1, y1, target.width, target.height);

    const bool toSrc = image.layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    if (toSrc)
        cmdTransitionImage(cmd, image.image, colorRange(), image.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    cmdTransitionImage(cmd, tex->image, colorRange(), VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {static_cast<int32_t>(image.width), static_cast<int32_t>(image.height), 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[1] = blit.srcOffsets[1];
    vkCmdBlitImage(cmd, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    cmdTransitionImage(cmd, tex->image, colorRange(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (toSrc)
        cmdTransitionImage(cmd, image.image, colorRange(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image.layout);
    return true;
}

// Blend each draw onto the target (in COLOR_ATTACHMENT_OPTIMAL), in order.
void VulkanPresenter::recordBlendDraws(VkCommandBuffer cmd, const Target& target,
                                       const std::vector<BlendDraw>& draws) {
    VkPipeline pipeline = blendPipeline(target.format);
    if (pipeline == VK_NULL_HANDLE) return;

    VkRenderingAttachmentInfo color{};
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView = target.view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea = {{0, 0}, {target.width, target.height}};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;

    vkCmdBeginRendering(cmd, &rendering);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    for (const BlendDraw& d : draws) {
        if (d.scissor.extent.width == 0 || d.scissor.extent.height == 0) continue;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, blendPipelineLayout_, 0, 1, &d.set,
                                0, nullptr);
        vkCmdSetViewport(cmd, 0, 1, &d.dst);
        vkCmdSetScissor(cmd, 0, 1, &d.scissor);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }
    vkCmdEndRendering(cmd);
}

} // namespace bro::render
