#include "render/vulkan_presenter.h"
#include "render/vulkan_presenter_shaders.h"
#include "util/log.h"

#include <include/core/SkColorType.h>
#include <include/core/SkPixmap.h>
#include <include/core/SkSurface.h>

#include <cstring>

namespace bro::render {

void VulkanPresenter::cleanupOverlay() {
    VkDevice device = context_.device();
    if (device == VK_NULL_HANDLE) return;

    if (overlayFramebufferOffscreen_ != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(device, overlayFramebufferOffscreen_, nullptr);
        overlayFramebufferOffscreen_ = VK_NULL_HANDLE;
    }
    for (auto fb : overlayFramebuffersSwapchain_) {
        if (fb != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(device, fb, nullptr);
        }
    }
    overlayFramebuffersSwapchain_.clear();

    if (overlayPipelineOffscreen_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, overlayPipelineOffscreen_, nullptr);
        overlayPipelineOffscreen_ = VK_NULL_HANDLE;
    }
    if (overlayRenderPassOffscreen_ != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device, overlayRenderPassOffscreen_, nullptr);
        overlayRenderPassOffscreen_ = VK_NULL_HANDLE;
    }

    if (overlayPipelineSwapchain_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, overlayPipelineSwapchain_, nullptr);
        overlayPipelineSwapchain_ = VK_NULL_HANDLE;
    }
    if (overlayRenderPassSwapchain_ != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device, overlayRenderPassSwapchain_, nullptr);
        overlayRenderPassSwapchain_ = VK_NULL_HANDLE;
    }

    if (overlayPipelineLayout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, overlayPipelineLayout_, nullptr);
        overlayPipelineLayout_ = VK_NULL_HANDLE;
    }
    if (overlayDescriptorPool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, overlayDescriptorPool_, nullptr);
        overlayDescriptorPool_ = VK_NULL_HANDLE;
        overlayDescriptorSet_ = VK_NULL_HANDLE;
    }
    if (overlayDescriptorSetLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, overlayDescriptorSetLayout_, nullptr);
        overlayDescriptorSetLayout_ = VK_NULL_HANDLE;
    }
    if (overlaySampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device, overlaySampler_, nullptr);
        overlaySampler_ = VK_NULL_HANDLE;
    }

    if (overlayView_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device, overlayView_, nullptr);
        overlayView_ = VK_NULL_HANDLE;
    }
    if (overlayAllocId_ != 0) {
        context_.destroyImage(overlayImage_, overlayAllocId_);
    } else {
        if (overlayImage_ != VK_NULL_HANDLE) vkDestroyImage(device, overlayImage_, nullptr);
        if (overlayMemory_ != VK_NULL_HANDLE) vkFreeMemory(device, overlayMemory_, nullptr);
    }
    overlayImage_ = VK_NULL_HANDLE;
    overlayMemory_ = VK_NULL_HANDLE;
    overlayAllocId_ = 0;
    overlayOffset_ = 0;
    overlayW_ = 0;
    overlayH_ = 0;
}

bool VulkanPresenter::ensureOverlayImage(uint32_t width, uint32_t height) {
    if (overlayImage_ != VK_NULL_HANDLE && overlayW_ == width && overlayH_ == height) {
        return true;
    }

    VkDevice device = context_.device();
    if (overlayView_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device, overlayView_, nullptr);
        overlayView_ = VK_NULL_HANDLE;
    }
    if (overlayAllocId_ != 0) {
        context_.destroyImage(overlayImage_, overlayAllocId_);
    } else {
        if (overlayImage_ != VK_NULL_HANDLE) vkDestroyImage(device, overlayImage_, nullptr);
        if (overlayMemory_ != VK_NULL_HANDLE) vkFreeMemory(device, overlayMemory_, nullptr);
    }
    overlayImage_ = VK_NULL_HANDLE;
    overlayMemory_ = VK_NULL_HANDLE;
    overlayAllocId_ = 0;
    overlayOffset_ = 0;

    overlayW_ = width;
    overlayH_ = height;

    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!context_.createImage(width, height, VK_FORMAT_R8G8B8A8_UNORM,
                              VK_IMAGE_TILING_OPTIMAL, usage,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                              overlayImage_, overlayMemory_,
                              overlayOffset_, overlayAllocId_)) {
        LOG_ERROR("VulkanPresenter: Failed to create overlay image");
        return false;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = overlayImage_;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(device, &viewInfo, nullptr, &overlayView_) != VK_SUCCESS) {
        LOG_ERROR("VulkanPresenter: Failed to create overlay image view");
        return false;
    }

    if (overlayDescriptorSet_ != VK_NULL_HANDLE && overlaySampler_ != VK_NULL_HANDLE) {
        VkDescriptorImageInfo descImage{};
        descImage.sampler = overlaySampler_;
        descImage.imageView = overlayView_;
        descImage.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = overlayDescriptorSet_;
        write.dstBinding = 0;
        write.dstArrayElement = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo = &descImage;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }

    return true;
}

bool VulkanPresenter::uploadOverlaySurface(SkSurface* surface, VkCommandBuffer cmd) {
    if (!surface) return false;

    SkPixmap pixmap;
    if (!surface->peekPixels(&pixmap)) return false;

    uint32_t w = static_cast<uint32_t>(pixmap.width());
    uint32_t h = static_cast<uint32_t>(pixmap.height());
    if (w == 0 || h == 0) return false;

    if (!ensureOverlayImage(w, h)) return false;

    VkDeviceSize imageBytes = static_cast<VkDeviceSize>(w) * h * 4;
    if (!ensureStagingBuffer(imageBytes)) return false;

    void* mapped = stagingMapped_;
    if (!mapped) {
        if (vkMapMemory(context_.device(), stagingMemory_, stagingOffset_, imageBytes, 0, &mapped) != VK_SUCCESS) {
            return false;
        }
    }

    bool isBgra = (pixmap.colorType() == kBGRA_8888_SkColorType);
    const uint8_t* src = reinterpret_cast<const uint8_t*>(pixmap.addr());
    uint8_t* dst = reinterpret_cast<uint8_t*>(mapped);
    uint32_t stride = static_cast<uint32_t>(pixmap.rowBytes());
    if (stride == 0) stride = w * 4;

    if (isBgra) {
        for (uint32_t y = 0; y < h; ++y) {
            const uint8_t* rowSrc = src + y * stride;
            uint8_t* rowDst = dst + y * w * 4;
            for (uint32_t x = 0; x < w; ++x) {
                rowDst[x * 4 + 0] = rowSrc[x * 4 + 2];
                rowDst[x * 4 + 1] = rowSrc[x * 4 + 1];
                rowDst[x * 4 + 2] = rowSrc[x * 4 + 0];
                rowDst[x * 4 + 3] = rowSrc[x * 4 + 3];
            }
        }
    } else {
        if (stride == w * 4) {
            std::memcpy(dst, src, imageBytes);
        } else {
            for (uint32_t y = 0; y < h; ++y) {
                std::memcpy(dst + y * w * 4, src + y * stride, w * 4);
            }
        }
    }

    if (!stagingMapped_) {
        vkUnmapMemory(context_.device(), stagingMemory_);
    }

    context_.transitionImageLayout(overlayImage_, VK_FORMAT_R8G8B8A8_UNORM,
                                   VK_IMAGE_LAYOUT_UNDEFINED,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   cmd);
    context_.copyBufferToImage(stagingBuffer_, overlayImage_, w, h, cmd);
    context_.transitionImageLayout(overlayImage_, VK_FORMAT_R8G8B8A8_UNORM,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                   cmd);
    return true;
}

bool VulkanPresenter::initOverlayPipeline(VkFormat targetFormat, VkRenderPass& outRenderPass, VkPipeline& outPipeline) {
    if (outPipeline != VK_NULL_HANDLE && outRenderPass != VK_NULL_HANDLE) {
        return true;
    }

    VkDevice device = context_.device();

    if (overlaySampler_ == VK_NULL_HANDLE) {
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.maxLod = 0.0f;

        if (vkCreateSampler(device, &samplerInfo, nullptr, &overlaySampler_) != VK_SUCCESS) {
            LOG_ERROR("VulkanPresenter: Failed to create overlay sampler");
            return false;
        }
    }

    if (overlayDescriptorSetLayout_ == VK_NULL_HANDLE) {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = 1;
        layoutInfo.pBindings = &binding;

        if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &overlayDescriptorSetLayout_) != VK_SUCCESS) {
            LOG_ERROR("VulkanPresenter: Failed to create overlay descriptor set layout");
            return false;
        }
    }

    if (overlayDescriptorPool_ == VK_NULL_HANDLE) {
        VkDescriptorPoolSize poolSize{};
        poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSize.descriptorCount = 1;

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        poolInfo.maxSets = 1;

        if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &overlayDescriptorPool_) != VK_SUCCESS) {
            LOG_ERROR("VulkanPresenter: Failed to create overlay descriptor pool");
            return false;
        }

        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = overlayDescriptorPool_;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &overlayDescriptorSetLayout_;

        if (vkAllocateDescriptorSets(device, &allocInfo, &overlayDescriptorSet_) != VK_SUCCESS) {
            LOG_ERROR("VulkanPresenter: Failed to allocate overlay descriptor set");
            return false;
        }
    }

    if (overlayPipelineLayout_ == VK_NULL_HANDLE) {
        VkPipelineLayoutCreateInfo pipeLayoutInfo{};
        pipeLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipeLayoutInfo.setLayoutCount = 1;
        pipeLayoutInfo.pSetLayouts = &overlayDescriptorSetLayout_;

        if (vkCreatePipelineLayout(device, &pipeLayoutInfo, nullptr, &overlayPipelineLayout_) != VK_SUCCESS) {
            LOG_ERROR("VulkanPresenter: Failed to create overlay pipeline layout");
            return false;
        }
    }

    if (overlayView_ != VK_NULL_HANDLE) {
        VkDescriptorImageInfo descImage{};
        descImage.sampler = overlaySampler_;
        descImage.imageView = overlayView_;
        descImage.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = overlayDescriptorSet_;
        write.dstBinding = 0;
        write.dstArrayElement = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo = &descImage;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }

    if (outRenderPass == VK_NULL_HANDLE) {
        VkAttachmentDescription colorAttachment{};
        colorAttachment.format = targetFormat;
        colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAttachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkAttachmentReference colorAttachmentRef{};
        colorAttachmentRef.attachment = 0;
        colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorAttachmentRef;

        VkRenderPassCreateInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassInfo.attachmentCount = 1;
        renderPassInfo.pAttachments = &colorAttachment;
        renderPassInfo.subpassCount = 1;
        renderPassInfo.pSubpasses = &subpass;

        if (vkCreateRenderPass(device, &renderPassInfo, nullptr, &outRenderPass) != VK_SUCCESS) {
            LOG_ERROR("VulkanPresenter: Failed to create overlay render pass");
            return false;
        }
    }

    if (outPipeline == VK_NULL_HANDLE) {
        VkShaderModule vertModule = VK_NULL_HANDLE;
        VkShaderModule fragModule = VK_NULL_HANDLE;

        VkShaderModuleCreateInfo vertInfo{};
        vertInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        vertInfo.codeSize = sizeof(kQuadVertSpv);
        vertInfo.pCode = kQuadVertSpv;
        if (vkCreateShaderModule(device, &vertInfo, nullptr, &vertModule) != VK_SUCCESS) {
            LOG_ERROR("VulkanPresenter: Failed to create quad vertex shader module");
            return false;
        }

        VkShaderModuleCreateInfo fragInfo{};
        fragInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        fragInfo.codeSize = sizeof(kOverlayFragSpv);
        fragInfo.pCode = kOverlayFragSpv;
        if (vkCreateShaderModule(device, &fragInfo, nullptr, &fragModule) != VK_SUCCESS) {
            LOG_ERROR("VulkanPresenter: Failed to create overlay fragment shader module");
            vkDestroyShaderModule(device, vertModule, nullptr);
            return false;
        }

        VkPipelineShaderStageCreateInfo shaderStages[2]{};
        shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        shaderStages[0].module = vertModule;
        shaderStages[0].pName = "main";

        shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        shaderStages[1].module = fragModule;
        shaderStages[1].pName = "main";

        VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
        vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo viewportState{};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rasterizer{};
        rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizer.lineWidth = 1.0f;
        rasterizer.cullMode = VK_CULL_MODE_NONE;
        rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;

        VkPipelineMultisampleStateCreateInfo multisampling{};
        multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineColorBlendAttachmentState colorBlendAttachment{};
        colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        colorBlendAttachment.blendEnable = VK_TRUE;
        colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

        VkPipelineColorBlendStateCreateInfo colorBlending{};
        colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlending.attachmentCount = 1;
        colorBlending.pAttachments = &colorBlendAttachment;

        VkDynamicState dynamicStates[] = {
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR
        };
        VkPipelineDynamicStateCreateInfo dynamicState{};
        dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamicState.dynamicStateCount = 2;
        dynamicState.pDynamicStates = dynamicStates;

        VkGraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = shaderStages;
        pipelineInfo.pVertexInputState = &vertexInputInfo;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterizer;
        pipelineInfo.pMultisampleState = &multisampling;
        pipelineInfo.pColorBlendState = &colorBlending;
        pipelineInfo.pDynamicState = &dynamicState;
        pipelineInfo.layout = overlayPipelineLayout_;
        pipelineInfo.renderPass = outRenderPass;
        pipelineInfo.subpass = 0;

        VkResult res = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &outPipeline);
        vkDestroyShaderModule(device, vertModule, nullptr);
        vkDestroyShaderModule(device, fragModule, nullptr);

        if (res != VK_SUCCESS) {
            LOG_ERROR("VulkanPresenter: Failed to create overlay graphics pipeline");
            return false;
        }
    }

    return true;
}

bool VulkanPresenter::recordOverlayPass(VkCommandBuffer cmd, VkImage targetImage, VkImageView targetView,
                                        VkRenderPass renderPass, VkPipeline pipeline, VkFramebuffer& framebuffer,
                                        uint32_t targetWidth, uint32_t targetHeight, VkFormat targetFormat) {
    VkDevice device = context_.device();

    if (framebuffer == VK_NULL_HANDLE) {
        VkFramebufferCreateInfo fbInfo{};
        fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbInfo.renderPass = renderPass;
        fbInfo.attachmentCount = 1;
        fbInfo.pAttachments = &targetView;
        fbInfo.width = targetWidth;
        fbInfo.height = targetHeight;
        fbInfo.layers = 1;

        if (vkCreateFramebuffer(device, &fbInfo, nullptr, &framebuffer) != VK_SUCCESS) {
            LOG_ERROR("VulkanPresenter: Failed to create framebuffer for overlay pass");
            return false;
        }
    }

    context_.transitionImageLayout(targetImage, targetFormat,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                   cmd);

    VkRenderPassBeginInfo rpBegin{};
    rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBegin.renderPass = renderPass;
    rpBegin.framebuffer = framebuffer;
    rpBegin.renderArea.offset = {0, 0};
    rpBegin.renderArea.extent = {targetWidth, targetHeight};

    vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, overlayPipelineLayout_, 0, 1, &overlayDescriptorSet_, 0, nullptr);

    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(targetWidth);
    viewport.height = static_cast<float>(targetHeight);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = {targetWidth, targetHeight};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
    return true;
}

} // namespace bro::render
