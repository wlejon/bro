#include "webgl/vulkan/webgl_vk_context.h"
#include "util/log.h"

#include <algorithm>

namespace bro::webgl::vk {

void WebGLVkContext::ensureCommandBuffer() {
    if (currentCmd_ != VK_NULL_HANDLE) return;

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = commandPool_;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    if (vkAllocateCommandBuffers(context_.device(), &allocInfo, &currentCmd_) != VK_SUCCESS) {
        LOG_ERROR("WebGLVkContext: Failed to allocate command buffer");
        return;
    }

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(currentCmd_, &beginInfo);
}

void WebGLVkContext::beginRendering() {
    if (inRenderPass_) return;

    ensureCommandBuffer();
    if (currentCmd_ == VK_NULL_HANDLE) return;

    if (currentFboId_ != 0) {
        auto itFbo = framebuffers_.find(currentFboId_);
        if (itFbo == framebuffers_.end() || itFbo->second.colorAttachmentTex == 0) return;
        auto itTex = textures_.find(itFbo->second.colorAttachmentTex);
        if (itTex == textures_.end() || !itTex->second.isValid()) return;

        VkTextureResource& colorTex = itTex->second;
        if (colorTex.currentLayout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
            context_.transitionImageLayout(colorTex.image, colorTex.format,
                                           colorTex.currentLayout,
                                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                           currentCmd_);
            colorTex.currentLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }

        VkRenderingAttachmentInfoKHR colorAttachment{};
        colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
        colorAttachment.imageView = colorTex.view;
        colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

        VkRenderingInfoKHR renderingInfo{};
        renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO_KHR;
        renderingInfo.renderArea.offset = {0, 0};
        renderingInfo.renderArea.extent = {colorTex.width, colorTex.height};
        renderingInfo.layerCount = 1;
        renderingInfo.colorAttachmentCount = 1;
        renderingInfo.pColorAttachments = &colorAttachment;
        renderingInfo.pDepthAttachment = nullptr;
        renderingInfo.pStencilAttachment = nullptr;

        if (pfnCmdBeginRendering_) {
            pfnCmdBeginRendering_(currentCmd_, &renderingInfo);
            inRenderPass_ = true;
        } else {
            LOG_ERROR("WebGLVkContext: Dynamic rendering function vkCmdBeginRendering unavailable");
        }
        return;
    }

    // Transition canvas attachments to optimal layout
    canvas_.transitionToColorAttachment(currentCmd_);
    if (canvas_.depthImage() != VK_NULL_HANDLE) {
        canvas_.transitionDepth(currentCmd_, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    }

    VkRenderingAttachmentInfoKHR colorAttachment{};
    colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
    colorAttachment.imageView = canvas_.colorView();
    colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingAttachmentInfoKHR depthAttachment{};
    depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
    depthAttachment.imageView = canvas_.depthView();
    depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfoKHR renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO_KHR;
    renderingInfo.renderArea.offset = {0, 0};
    renderingInfo.renderArea.extent = {canvas_.width(), canvas_.height()};
    renderingInfo.layerCount = 1;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachments = &colorAttachment;
    renderingInfo.pDepthAttachment = (canvas_.depthView() != VK_NULL_HANDLE) ? &depthAttachment : nullptr;
    renderingInfo.pStencilAttachment = (canvas_.depthView() != VK_NULL_HANDLE &&
        (canvas_.depthFormat() == VK_FORMAT_D32_SFLOAT_S8_UINT ||
         canvas_.depthFormat() == VK_FORMAT_D24_UNORM_S8_UINT)) ? &depthAttachment : nullptr;

    if (pfnCmdBeginRendering_) {
        pfnCmdBeginRendering_(currentCmd_, &renderingInfo);
        inRenderPass_ = true;
    } else {
        LOG_ERROR("WebGLVkContext: Dynamic rendering function vkCmdBeginRendering unavailable");
    }
}

void WebGLVkContext::endRendering() {
    if (inRenderPass_ && currentCmd_ != VK_NULL_HANDLE) {
        if (pfnCmdEndRendering_) {
            pfnCmdEndRendering_(currentCmd_);
        }
        inRenderPass_ = false;

        if (currentFboId_ != 0) {
            auto itFbo = framebuffers_.find(currentFboId_);
            if (itFbo != framebuffers_.end() && itFbo->second.colorAttachmentTex != 0) {
                auto itTex = textures_.find(itFbo->second.colorAttachmentTex);
                if (itTex != textures_.end() && itTex->second.isValid()) {
                    context_.transitionImageLayout(itTex->second.image, itTex->second.format,
                                                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                   currentCmd_);
                    itTex->second.currentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                }
            }
        }
    }
}

void WebGLVkContext::submitAndFlush() {
    endRendering();

    if (currentCmd_ != VK_NULL_HANDLE) {
        vkEndCommandBuffer(currentCmd_);

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &currentCmd_;

        vkQueueSubmit(context_.graphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE);
        context_.waitIdle();

        vkFreeCommandBuffers(context_.device(), commandPool_, 1, &currentCmd_);
        currentCmd_ = VK_NULL_HANDLE;
    }
}

void WebGLVkContext::clear(GLbitfield mask) {
    beginRendering();
    if (!inRenderPass_ || currentCmd_ == VK_NULL_HANDLE) return;

    VkClearAttachment clearAttachments[2]{};
    uint32_t clearCount = 0;

    if (mask & GL_COLOR_BUFFER_BIT) {
        clearAttachments[clearCount].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        clearAttachments[clearCount].colorAttachment = 0;
        clearAttachments[clearCount].clearValue.color = {
            {clearColor_[0], clearColor_[1], clearColor_[2], clearColor_[3]}
        };
        clearCount++;
    }

    if ((mask & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)) && canvas_.depthImage() != VK_NULL_HANDLE) {
        VkImageAspectFlags aspect = 0;
        if (mask & GL_DEPTH_BUFFER_BIT) aspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
        if (mask & GL_STENCIL_BUFFER_BIT) aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;

        clearAttachments[clearCount].aspectMask = aspect;
        clearAttachments[clearCount].clearValue.depthStencil = {
            clearDepth_, static_cast<uint32_t>(clearStencil_)
        };
        clearCount++;
    }

    if (clearCount > 0) {
        VkClearRect clearRect{};
        if (scissorTest_) {
            clearRect.rect = scissor_;
        } else {
            clearRect.rect.offset = {0, 0};
            clearRect.rect.extent = {canvas_.width(), canvas_.height()};
        }
        clearRect.baseArrayLayer = 0;
        clearRect.layerCount = 1;

        vkCmdClearAttachments(currentCmd_, clearCount, clearAttachments, 1, &clearRect);
    }
}

void WebGLVkContext::drawArrays(GLenum mode, GLint first, GLsizei count) {
    drawArraysInstanced(mode, first, count, 1);
}

void WebGLVkContext::drawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instanceCount) {
    auto itProg = programs_.find(currentProgramId_);
    if (itProg == programs_.end() || !itProg->second.linkStatus || count <= 0 || instanceCount <= 0) return;

    VkProgramResource& prog = itProg->second;
    if (prog.vertModule == VK_NULL_HANDLE || prog.fragModule == VK_NULL_HANDLE) return;

    beginRendering();
    if (!inRenderPass_ || currentCmd_ == VK_NULL_HANDLE) return;

    // Build PipelineKey
    PipelineKey key{};
    key.vertShader = prog.vertModule;
    key.fragShader = prog.fragModule;
    key.topology = glTopologyToVk(mode);

    key.cullFaceEnable = cullFaceEnabled_ ? VK_TRUE : VK_FALSE;
    key.cullMode = glCullModeToVk(cullFaceMode_);
    key.frontFace = glFrontFaceToVk(frontFaceMode_);

    key.depthTestEnable = depthTestEnabled_ ? VK_TRUE : VK_FALSE;
    key.depthWriteEnable = depthMask_ ? VK_TRUE : VK_FALSE;
    key.depthCompareOp = glCompareOpToVk(depthFunc_);

    key.blendEnable = blendEnabled_ ? VK_TRUE : VK_FALSE;
    key.srcColorBlendFactor = glBlendFactorToVk(blendSrcRGB_);
    key.dstColorBlendFactor = glBlendFactorToVk(blendDstRGB_);
    key.colorBlendOp = glBlendOpToVk(blendEqRGB_);
    key.srcAlphaBlendFactor = glBlendFactorToVk(blendSrcAlpha_);
    key.dstAlphaBlendFactor = glBlendFactorToVk(blendDstAlpha_);
    key.alphaBlendOp = glBlendOpToVk(blendEqAlpha_);

    key.colorWriteMask = 0;
    if (colorMask_[0]) key.colorWriteMask |= VK_COLOR_COMPONENT_R_BIT;
    if (colorMask_[1]) key.colorWriteMask |= VK_COLOR_COMPONENT_G_BIT;
    if (colorMask_[2]) key.colorWriteMask |= VK_COLOR_COMPONENT_B_BIT;
    if (colorMask_[3]) key.colorWriteMask |= VK_COLOR_COMPONENT_A_BIT;

    VkFormat targetColorFormat = canvas_.colorFormat();
    VkFormat targetDepthFormat = canvas_.depthFormat();
    VkExtent2D targetExtent = {canvas_.width(), canvas_.height()};

    if (currentFboId_ != 0) {
        auto itFbo = framebuffers_.find(currentFboId_);
        if (itFbo != framebuffers_.end() && itFbo->second.colorAttachmentTex != 0) {
            auto itTex = textures_.find(itFbo->second.colorAttachmentTex);
            if (itTex != textures_.end() && itTex->second.isValid()) {
                targetColorFormat = itTex->second.format;
                targetDepthFormat = VK_FORMAT_UNDEFINED;
                targetExtent = {itTex->second.width, itTex->second.height};
            }
        }
    }

    key.colorAttachmentFormat = targetColorFormat;
    key.depthAttachmentFormat = targetDepthFormat;

    // Map vertex attributes and bindings from active VAO
    VkVAOResource& vao = vaos_[currentVaoId_];
    uint32_t activeAttrCount = 0;
    std::vector<VkBuffer> boundVBOs;
    std::vector<VkDeviceSize> boundOffsets;

    for (uint32_t i = 0; i < 16; ++i) {
        const VkVertexAttribute& attr = vao.attributes[i];
        if (attr.enabled && attr.bufferId != 0) {
            auto bIt = buffers_.find(attr.bufferId);
            if (bIt != buffers_.end() && bIt->second.isValid()) {
                uint32_t bindingIdx = activeAttrCount;

                key.attributes[activeAttrCount].location = i;
                key.attributes[activeAttrCount].binding = bindingIdx;
                key.attributes[activeAttrCount].format = glTypeToVkFormat(attr.type, attr.size, attr.normalized);
                key.attributes[activeAttrCount].offset = 0;

                key.bindings[activeAttrCount].binding = bindingIdx;
                key.bindings[activeAttrCount].stride = attr.stride;
                key.bindings[activeAttrCount].inputRate = (attr.divisor > 0)
                    ? VK_VERTEX_INPUT_RATE_INSTANCE
                    : VK_VERTEX_INPUT_RATE_VERTEX;

                boundVBOs.push_back(bIt->second.buffer);
                boundOffsets.push_back(attr.offset);
                activeAttrCount++;
            }
        }
    }
    key.attributeCount = activeAttrCount;
    key.bindingCount = activeAttrCount;

    VkPipeline pipeline = pipelineCache_.getOrCreatePipeline(key, pipelineLayout_);
    if (pipeline == VK_NULL_HANDLE) {
        LOG_ERROR("WebGLVkContext: Failed to obtain graphics pipeline for drawArrays");
        return;
    }

    vkCmdBindPipeline(currentCmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    // Negative viewport height for OpenGL NDC compatibility
    VkViewport vp{};
    vp.x = viewport_.x;
    vp.y = viewport_.y + viewport_.height;
    vp.width = viewport_.width;
    vp.height = -viewport_.height;
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(currentCmd_, 0, 1, &vp);

    VkRect2D sc{};
    if (scissorTest_) {
        sc = scissor_;
    } else {
        sc.offset = {0, 0};
        sc.extent = targetExtent;
    }
    vkCmdSetScissor(currentCmd_, 0, 1, &sc);

    // Bind descriptor set for samplers
    if (prog.descriptorSet != VK_NULL_HANDLE) {
        std::vector<VkDescriptorImageInfo> imageInfos;
        std::vector<VkWriteDescriptorSet> writes;
        imageInfos.reserve(8);
        writes.reserve(8);

        for (uint32_t binding = 0; binding < 8; ++binding) {
            uint32_t texUnit = 0;
            auto sIt = prog.samplerBindings.find(binding);
            if (sIt != prog.samplerBindings.end()) {
                texUnit = sIt->second;
            }
            GLuint texId = (texUnit < boundTextures2D_.size()) ? boundTextures2D_[texUnit] : 0;
            VkImageView view = dummyView_;
            VkSampler sampler = dummySampler_;

            if (texId != 0) {
                auto tIt = textures_.find(texId);
                if (tIt != textures_.end() && tIt->second.isValid()) {
                    updateTextureSampler(tIt->second);
                    view = tIt->second.view;
                    sampler = tIt->second.sampler;
                }
            }

            VkDescriptorImageInfo imgInfo{};
            imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            imgInfo.imageView = view;
            imgInfo.sampler = sampler;
            imageInfos.push_back(imgInfo);

            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = prog.descriptorSet;
            write.dstBinding = binding;
            write.dstArrayElement = 0;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.descriptorCount = 1;
            write.pImageInfo = &imageInfos.back();
            writes.push_back(write);
        }

        vkUpdateDescriptorSets(context_.device(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        vkCmdBindDescriptorSets(currentCmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_,
                                0, 1, &prog.descriptorSet, 0, nullptr);
    }

    // Push uniform constants
    if (!prog.uniformBytes.empty()) {
        vkCmdPushConstants(currentCmd_, pipelineLayout_,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, static_cast<uint32_t>(prog.uniformBytes.size()),
                           prog.uniformBytes.data());
    }

    // Bind vertex buffers
    for (uint32_t b = 0; b < activeAttrCount; ++b) {
        vkCmdBindVertexBuffers(currentCmd_, b, 1, &boundVBOs[b], &boundOffsets[b]);
    }

    vkCmdDraw(currentCmd_, static_cast<uint32_t>(count), static_cast<uint32_t>(instanceCount),
              static_cast<uint32_t>(first), 0);
}

void WebGLVkContext::drawElements(GLenum mode, GLsizei count, GLenum type, uintptr_t offset) {
    drawElementsInstanced(mode, count, type, offset, 1);
}

void WebGLVkContext::drawElementsInstanced(GLenum mode, GLsizei count, GLenum type,
                                          uintptr_t offset, GLsizei instanceCount) {
    if (currentProgramId_ == 0 || count <= 0 || instanceCount <= 0) return;

    GLuint iboId = vaos_[currentVaoId_].elementArrayBufferId;
    if (iboId == 0) return;
    auto iboIt = buffers_.find(iboId);
    if (iboIt == buffers_.end() || !iboIt->second.isValid()) return;

    auto itProg = programs_.find(currentProgramId_);
    if (itProg == programs_.end() || !itProg->second.linkStatus) return;
    VkProgramResource& prog = itProg->second;
    if (prog.vertModule == VK_NULL_HANDLE || prog.fragModule == VK_NULL_HANDLE) return;

    beginRendering();
    if (!inRenderPass_ || currentCmd_ == VK_NULL_HANDLE) return;

    PipelineKey key{};
    key.vertShader = prog.vertModule;
    key.fragShader = prog.fragModule;
    key.topology = glTopologyToVk(mode);

    key.cullFaceEnable = cullFaceEnabled_ ? VK_TRUE : VK_FALSE;
    key.cullMode = glCullModeToVk(cullFaceMode_);
    key.frontFace = glFrontFaceToVk(frontFaceMode_);

    key.depthTestEnable = depthTestEnabled_ ? VK_TRUE : VK_FALSE;
    key.depthWriteEnable = depthMask_ ? VK_TRUE : VK_FALSE;
    key.depthCompareOp = glCompareOpToVk(depthFunc_);

    key.blendEnable = blendEnabled_ ? VK_TRUE : VK_FALSE;
    key.srcColorBlendFactor = glBlendFactorToVk(blendSrcRGB_);
    key.dstColorBlendFactor = glBlendFactorToVk(blendDstRGB_);
    key.colorBlendOp = glBlendOpToVk(blendEqRGB_);
    key.srcAlphaBlendFactor = glBlendFactorToVk(blendSrcAlpha_);
    key.dstAlphaBlendFactor = glBlendFactorToVk(blendDstAlpha_);
    key.alphaBlendOp = glBlendOpToVk(blendEqAlpha_);

    key.colorWriteMask = 0;
    if (colorMask_[0]) key.colorWriteMask |= VK_COLOR_COMPONENT_R_BIT;
    if (colorMask_[1]) key.colorWriteMask |= VK_COLOR_COMPONENT_G_BIT;
    if (colorMask_[2]) key.colorWriteMask |= VK_COLOR_COMPONENT_B_BIT;
    if (colorMask_[3]) key.colorWriteMask |= VK_COLOR_COMPONENT_A_BIT;

    VkFormat targetColorFormat = canvas_.colorFormat();
    VkFormat targetDepthFormat = canvas_.depthFormat();
    VkExtent2D targetExtent = {canvas_.width(), canvas_.height()};

    if (currentFboId_ != 0) {
        auto itFbo = framebuffers_.find(currentFboId_);
        if (itFbo != framebuffers_.end() && itFbo->second.colorAttachmentTex != 0) {
            auto itTex = textures_.find(itFbo->second.colorAttachmentTex);
            if (itTex != textures_.end() && itTex->second.isValid()) {
                targetColorFormat = itTex->second.format;
                targetDepthFormat = VK_FORMAT_UNDEFINED;
                targetExtent = {itTex->second.width, itTex->second.height};
            }
        }
    }

    key.colorAttachmentFormat = targetColorFormat;
    key.depthAttachmentFormat = targetDepthFormat;

    VkVAOResource& vao = vaos_[currentVaoId_];
    uint32_t activeAttrCount = 0;
    std::vector<VkBuffer> boundVBOs;
    std::vector<VkDeviceSize> boundOffsets;

    for (uint32_t i = 0; i < 16; ++i) {
        const VkVertexAttribute& attr = vao.attributes[i];
        if (attr.enabled && attr.bufferId != 0) {
            auto bIt = buffers_.find(attr.bufferId);
            if (bIt != buffers_.end() && bIt->second.isValid()) {
                uint32_t bindingIdx = activeAttrCount;

                key.attributes[activeAttrCount].location = i;
                key.attributes[activeAttrCount].binding = bindingIdx;
                key.attributes[activeAttrCount].format = glTypeToVkFormat(attr.type, attr.size, attr.normalized);
                key.attributes[activeAttrCount].offset = 0;

                key.bindings[activeAttrCount].binding = bindingIdx;
                key.bindings[activeAttrCount].stride = attr.stride;
                key.bindings[activeAttrCount].inputRate = (attr.divisor > 0)
                    ? VK_VERTEX_INPUT_RATE_INSTANCE
                    : VK_VERTEX_INPUT_RATE_VERTEX;

                boundVBOs.push_back(bIt->second.buffer);
                boundOffsets.push_back(attr.offset);
                activeAttrCount++;
            }
        }
    }
    key.attributeCount = activeAttrCount;
    key.bindingCount = activeAttrCount;

    VkPipeline pipeline = pipelineCache_.getOrCreatePipeline(key, pipelineLayout_);
    if (pipeline == VK_NULL_HANDLE) {
        LOG_ERROR("WebGLVkContext: Failed to obtain graphics pipeline for drawElements");
        return;
    }

    vkCmdBindPipeline(currentCmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    VkViewport vp{};
    vp.x = viewport_.x;
    vp.y = viewport_.y + viewport_.height;
    vp.width = viewport_.width;
    vp.height = -viewport_.height;
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(currentCmd_, 0, 1, &vp);

    VkRect2D sc{};
    if (scissorTest_) {
        sc = scissor_;
    } else {
        sc.offset = {0, 0};
        sc.extent = targetExtent;
    }
    vkCmdSetScissor(currentCmd_, 0, 1, &sc);

    // Bind descriptor set for samplers
    if (prog.descriptorSet != VK_NULL_HANDLE) {
        std::vector<VkDescriptorImageInfo> imageInfos;
        std::vector<VkWriteDescriptorSet> writes;
        imageInfos.reserve(8);
        writes.reserve(8);

        for (uint32_t binding = 0; binding < 8; ++binding) {
            uint32_t texUnit = 0;
            auto sIt = prog.samplerBindings.find(binding);
            if (sIt != prog.samplerBindings.end()) {
                texUnit = sIt->second;
            }
            GLuint texId = (texUnit < boundTextures2D_.size()) ? boundTextures2D_[texUnit] : 0;
            VkImageView view = dummyView_;
            VkSampler sampler = dummySampler_;

            if (texId != 0) {
                auto tIt = textures_.find(texId);
                if (tIt != textures_.end() && tIt->second.isValid()) {
                    updateTextureSampler(tIt->second);
                    view = tIt->second.view;
                    sampler = tIt->second.sampler;
                }
            }

            VkDescriptorImageInfo imgInfo{};
            imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            imgInfo.imageView = view;
            imgInfo.sampler = sampler;
            imageInfos.push_back(imgInfo);

            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = prog.descriptorSet;
            write.dstBinding = binding;
            write.dstArrayElement = 0;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.descriptorCount = 1;
            write.pImageInfo = &imageInfos.back();
            writes.push_back(write);
        }

        vkUpdateDescriptorSets(context_.device(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        vkCmdBindDescriptorSets(currentCmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_,
                                0, 1, &prog.descriptorSet, 0, nullptr);
    }

    if (!prog.uniformBytes.empty()) {
        vkCmdPushConstants(currentCmd_, pipelineLayout_,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, static_cast<uint32_t>(prog.uniformBytes.size()),
                           prog.uniformBytes.data());
    }

    for (uint32_t b = 0; b < activeAttrCount; ++b) {
        vkCmdBindVertexBuffers(currentCmd_, b, 1, &boundVBOs[b], &boundOffsets[b]);
    }

    VkIndexType idxType = (type == GL_UNSIGNED_SHORT) ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
    vkCmdBindIndexBuffer(currentCmd_, iboIt->second.buffer, offset, idxType);

    vkCmdDrawIndexed(currentCmd_, static_cast<uint32_t>(count), static_cast<uint32_t>(instanceCount), 0, 0, 0);
}

} // namespace bro::webgl::vk
