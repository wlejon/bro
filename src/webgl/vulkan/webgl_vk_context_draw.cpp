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
        if (itFbo == framebuffers_.end()) return;
        VkFramebufferResource& fbo = itFbo->second;

        std::vector<VkRenderingAttachmentInfoKHR> colorAttachments;
        uint32_t fboWidth = 0;
        uint32_t fboHeight = 0;

        for (GLenum db : fbo.drawBuffers) {
            GLuint texId = 0;
            if (db >= 0x8CE0 && db <= 0x8CE7) {
                uint32_t idx = db - 0x8CE0;
                if (idx < fbo.colorAttachments.size()) texId = fbo.colorAttachments[idx];
                if (texId == 0 && idx == 0) texId = fbo.colorAttachmentTex;
            }
            if (texId != 0) {
                auto itTex = textures_.find(texId);
                if (itTex != textures_.end() && itTex->second.isValid()) {
                    VkTextureResource& colorTex = itTex->second;
                    fboWidth = colorTex.width;
                    fboHeight = colorTex.height;
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
                    colorAttachments.push_back(colorAttachment);
                    continue;
                }
            }
            VkRenderingAttachmentInfoKHR nullAtt{};
            nullAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
            nullAtt.imageView = VK_NULL_HANDLE;
            colorAttachments.push_back(nullAtt);
        }

        if (colorAttachments.empty()) {
            GLuint singleTexId = (fbo.colorAttachments[0] != 0) ? fbo.colorAttachments[0] : fbo.colorAttachmentTex;
            if (singleTexId != 0) {
                auto itTex = textures_.find(singleTexId);
                if (itTex != textures_.end() && itTex->second.isValid()) {
                    VkTextureResource& colorTex = itTex->second;
                    fboWidth = colorTex.width;
                    fboHeight = colorTex.height;
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
                    colorAttachments.push_back(colorAttachment);
                }
            }
        }

        VkRenderingInfoKHR renderingInfo{};
        renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO_KHR;
        renderingInfo.renderArea.offset = {0, 0};
        renderingInfo.renderArea.extent = {fboWidth, fboHeight};
        renderingInfo.layerCount = 1;
        renderingInfo.colorAttachmentCount = static_cast<uint32_t>(colorAttachments.size());
        renderingInfo.pColorAttachments = colorAttachments.data();
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
            if (itFbo != framebuffers_.end()) {
                auto transitionTex = [&](GLuint texId) {
                    if (texId == 0) return;
                    auto itTex = textures_.find(texId);
                    if (itTex != textures_.end() && itTex->second.isValid() &&
                        itTex->second.currentLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
                        context_.transitionImageLayout(itTex->second.image, itTex->second.format,
                                                       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                       currentCmd_);
                        itTex->second.currentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    }
                };
                for (GLuint tid : itFbo->second.colorAttachments) transitionTex(tid);
                transitionTex(itFbo->second.colorAttachmentTex);
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
            if (currentFboId_ != 0) {
                auto itFbo = framebuffers_.find(currentFboId_);
                if (itFbo != framebuffers_.end() && itFbo->second.colorAttachmentTex != 0) {
                    auto itTex = textures_.find(itFbo->second.colorAttachmentTex);
                    if (itTex != textures_.end() && itTex->second.isValid()) {
                        clearRect.rect.extent = {itTex->second.width, itTex->second.height};
                    }
                }
            }
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

    key.stencilTestEnable = stencilTestEnabled_ ? VK_TRUE : VK_FALSE;
    key.stencilFront.failOp = glStencilOpToVk(stencilFailFront_);
    key.stencilFront.passOp = glStencilOpToVk(stencilPassDepthPassFront_);
    key.stencilFront.depthFailOp = glStencilOpToVk(stencilPassDepthFailFront_);
    key.stencilFront.compareOp = glCompareOpToVk(stencilFuncFront_);
    key.stencilFront.compareMask = stencilValueMaskFront_;
    key.stencilFront.writeMask = stencilWriteMaskFront_;
    key.stencilFront.reference = stencilRefFront_;

    key.stencilBack.failOp = glStencilOpToVk(stencilFailBack_);
    key.stencilBack.passOp = glStencilOpToVk(stencilPassDepthPassBack_);
    key.stencilBack.depthFailOp = glStencilOpToVk(stencilPassDepthFailBack_);
    key.stencilBack.compareOp = glCompareOpToVk(stencilFuncBack_);
    key.stencilBack.compareMask = stencilValueMaskBack_;
    key.stencilBack.writeMask = stencilWriteMaskBack_;
    key.stencilBack.reference = stencilRefBack_;

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

    key.colorAttachmentCount = 1;
    key.colorAttachmentFormats[0] = targetColorFormat;

    if (currentFboId_ != 0) {
        auto itFbo = framebuffers_.find(currentFboId_);
        if (itFbo != framebuffers_.end()) {
            const VkFramebufferResource& fbo = itFbo->second;
            uint32_t count = 0;
            for (GLenum db : fbo.drawBuffers) {
                GLuint texId = 0;
                if (db >= 0x8CE0 && db <= 0x8CE7) {
                    uint32_t idx = db - 0x8CE0;
                    if (idx < fbo.colorAttachments.size()) texId = fbo.colorAttachments[idx];
                    if (texId == 0 && idx == 0) texId = fbo.colorAttachmentTex;
                }
                VkFormat fmt = VK_FORMAT_UNDEFINED;
                if (texId != 0) {
                    auto itTex = textures_.find(texId);
                    if (itTex != textures_.end() && itTex->second.isValid()) {
                        fmt = itTex->second.format;
                        targetExtent = {itTex->second.width, itTex->second.height};
                    }
                }
                if (count < 8) {
                    key.colorAttachmentFormats[count] = fmt;
                    count++;
                }
            }
            if (count > 0) {
                key.colorAttachmentCount = count;
                key.colorAttachmentFormat = key.colorAttachmentFormats[0];
            } else if (fbo.colorAttachmentTex != 0) {
                auto itTex = textures_.find(fbo.colorAttachmentTex);
                if (itTex != textures_.end() && itTex->second.isValid()) {
                    targetColorFormat = itTex->second.format;
                    targetExtent = {itTex->second.width, itTex->second.height};
                    key.colorAttachmentFormats[0] = targetColorFormat;
                    key.colorAttachmentFormat = targetColorFormat;
                }
            }
            targetDepthFormat = VK_FORMAT_UNDEFINED;
            if (fbo.depthAttachmentTex != 0) {
                auto itD = textures_.find(fbo.depthAttachmentTex);
                if (itD != textures_.end() && itD->second.isValid()) {
                    targetDepthFormat = itD->second.format;
                }
            }
        }
    } else {
        key.colorAttachmentFormat = targetColorFormat;
    }
    key.depthAttachmentFormat = targetDepthFormat;

    // Map vertex attributes and bindings from active VAO
    VkVAOResource& vao = vaos_[currentVaoId_];
    uint32_t activeAttrCount = 0;
    std::vector<VkBuffer> boundVBOs;
    std::vector<VkDeviceSize> boundOffsets;

    for (const auto& aInfo : prog.activeAttribs) {
        uint32_t i = static_cast<uint32_t>(aInfo.location);
        if (i >= 16) continue;
        const VkVertexAttribute& attr = vao.attributes[i];
        uint32_t bindingIdx = activeAttrCount;

        if (attr.enabled && attr.bufferId != 0) {
            auto bIt = buffers_.find(attr.bufferId);
            if (bIt != buffers_.end() && bIt->second.isValid()) {
                key.attributes[activeAttrCount].location = i;
                key.attributes[activeAttrCount].binding = bindingIdx;
                key.attributes[activeAttrCount].format = glTypeToVkFormat(attr.type, attr.size, attr.normalized, attr.isInteger);
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
        } else if (fallbackConstantBuffer_ != VK_NULL_HANDLE) {
            bool isInt = (aInfo.type == GL_INT || aInfo.type == GL_UNSIGNED_INT ||
                          aInfo.type == GL_INT_VEC2 || aInfo.type == GL_UNSIGNED_INT_VEC2 ||
                          aInfo.type == GL_INT_VEC3 || aInfo.type == GL_UNSIGNED_INT_VEC3 ||
                          aInfo.type == GL_INT_VEC4 || aInfo.type == GL_UNSIGNED_INT_VEC4);
            key.attributes[activeAttrCount].location = i;
            key.attributes[activeAttrCount].binding = bindingIdx;
            key.attributes[activeAttrCount].format = isInt
                ? VK_FORMAT_R32G32B32A32_UINT
                : VK_FORMAT_R32G32B32A32_SFLOAT;
            key.attributes[activeAttrCount].offset = 0;

            key.bindings[activeAttrCount].binding = bindingIdx;
            key.bindings[activeAttrCount].stride = 0;
            key.bindings[activeAttrCount].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

            boundVBOs.push_back(fallbackConstantBuffer_);
            boundOffsets.push_back(i * 16);
            activeAttrCount++;
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
    vp.y = static_cast<float>(targetExtent.height) - viewport_.y;
    vp.width = viewport_.width;
    vp.height = -viewport_.height;
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(currentCmd_, 0, 1, &vp);

    VkRect2D sc{};
    if (scissorTest_) {
        sc.offset.x = scissor_.offset.x;
        sc.offset.y = static_cast<int32_t>(targetExtent.height) - (scissor_.offset.y + static_cast<int32_t>(scissor_.extent.height));
        sc.extent = scissor_.extent;
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
            GLenum sampType = GL_SAMPLER_2D;
            auto typeIt = prog.samplerTypes.find(binding);
            if (typeIt != prog.samplerTypes.end()) {
                sampType = typeIt->second;
            }
            GLuint texId = 0;
            if (sampType == GL_SAMPLER_CUBE || sampType == 0x8DC5) {
                if (texUnit < boundTexturesCubeMap_.size()) texId = boundTexturesCubeMap_[texUnit];
            } else if (sampType == 0x8DC1 || sampType == 0x8DC4) {
                if (texUnit < boundTextures2DArray_.size()) texId = boundTextures2DArray_[texUnit];
            } else if (sampType == 0x8B5F) {
                if (texUnit < boundTextures3D_.size()) texId = boundTextures3D_[texUnit];
            } else {
                if (texUnit < boundTextures2D_.size()) texId = boundTextures2D_[texUnit];
            }
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

            if (texUnit < boundSamplers_.size() && boundSamplers_[texUnit] != 0) {
                auto smpIt = samplers_.find(boundSamplers_[texUnit]);
                if (smpIt != samplers_.end()) {
                    updateSamplerObject(smpIt->second);
                    sampler = smpIt->second.sampler;
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

        std::vector<VkDescriptorBufferInfo> bufferInfos;
        bufferInfos.reserve(8);
        for (uint32_t binding = 8; binding < 16; ++binding) {
            VkDescriptorBufferInfo bufInfo{};
            bufInfo.buffer = dummyUniformBuffer_;
            bufInfo.offset = 0;
            bufInfo.range = 256;

            for (const auto& ub : prog.uniformBlocks) {
                if (ub.descriptorBinding == binding) {
                    GLuint blockIdx = ub.index;
                    GLuint bindingPoint = ub.binding;
                    auto itBind = prog.uniformBlockBindings.find(blockIdx);
                    if (itBind != prog.uniformBlockBindings.end()) {
                        bindingPoint = itBind->second;
                    }
                    if (bindingPoint < boundUniformBuffers_.size()) {
                        GLuint bufId = boundUniformBuffers_[bindingPoint];
                        if (bufId != 0) {
                            auto bIt = buffers_.find(bufId);
                            if (bIt != buffers_.end() && bIt->second.buffer != VK_NULL_HANDLE) {
                                bufInfo.buffer = bIt->second.buffer;
                                bufInfo.offset = 0;
                                bufInfo.range = (bIt->second.size > 0) ? bIt->second.size : 256;
                            }
                        }
                    }
                    break;
                }
            }

            bufferInfos.push_back(bufInfo);
            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = prog.descriptorSet;
            write.dstBinding = binding;
            write.dstArrayElement = 0;
            write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            write.descriptorCount = 1;
            write.pBufferInfo = &bufferInfos.back();
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

    key.stencilTestEnable = stencilTestEnabled_ ? VK_TRUE : VK_FALSE;
    key.stencilFront.failOp = glStencilOpToVk(stencilFailFront_);
    key.stencilFront.passOp = glStencilOpToVk(stencilPassDepthPassFront_);
    key.stencilFront.depthFailOp = glStencilOpToVk(stencilPassDepthFailFront_);
    key.stencilFront.compareOp = glCompareOpToVk(stencilFuncFront_);
    key.stencilFront.compareMask = stencilValueMaskFront_;
    key.stencilFront.writeMask = stencilWriteMaskFront_;
    key.stencilFront.reference = stencilRefFront_;

    key.stencilBack.failOp = glStencilOpToVk(stencilFailBack_);
    key.stencilBack.passOp = glStencilOpToVk(stencilPassDepthPassBack_);
    key.stencilBack.depthFailOp = glStencilOpToVk(stencilPassDepthFailBack_);
    key.stencilBack.compareOp = glCompareOpToVk(stencilFuncBack_);
    key.stencilBack.compareMask = stencilValueMaskBack_;
    key.stencilBack.writeMask = stencilWriteMaskBack_;
    key.stencilBack.reference = stencilRefBack_;

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

    key.colorAttachmentCount = 1;
    key.colorAttachmentFormats[0] = targetColorFormat;

    if (currentFboId_ != 0) {
        auto itFbo = framebuffers_.find(currentFboId_);
        if (itFbo != framebuffers_.end()) {
            const VkFramebufferResource& fbo = itFbo->second;
            uint32_t count = 0;
            for (GLenum db : fbo.drawBuffers) {
                GLuint texId = 0;
                if (db >= 0x8CE0 && db <= 0x8CE7) {
                    uint32_t idx = db - 0x8CE0;
                    if (idx < fbo.colorAttachments.size()) texId = fbo.colorAttachments[idx];
                    if (texId == 0 && idx == 0) texId = fbo.colorAttachmentTex;
                }
                VkFormat fmt = VK_FORMAT_UNDEFINED;
                if (texId != 0) {
                    auto itTex = textures_.find(texId);
                    if (itTex != textures_.end() && itTex->second.isValid()) {
                        fmt = itTex->second.format;
                        targetExtent = {itTex->second.width, itTex->second.height};
                    }
                }
                if (count < 8) {
                    key.colorAttachmentFormats[count] = fmt;
                    count++;
                }
            }
            if (count > 0) {
                key.colorAttachmentCount = count;
                key.colorAttachmentFormat = key.colorAttachmentFormats[0];
            } else if (fbo.colorAttachmentTex != 0) {
                auto itTex = textures_.find(fbo.colorAttachmentTex);
                if (itTex != textures_.end() && itTex->second.isValid()) {
                    targetColorFormat = itTex->second.format;
                    targetExtent = {itTex->second.width, itTex->second.height};
                    key.colorAttachmentFormats[0] = targetColorFormat;
                    key.colorAttachmentFormat = targetColorFormat;
                }
            }
            targetDepthFormat = VK_FORMAT_UNDEFINED;
            if (fbo.depthAttachmentTex != 0) {
                auto itD = textures_.find(fbo.depthAttachmentTex);
                if (itD != textures_.end() && itD->second.isValid()) {
                    targetDepthFormat = itD->second.format;
                }
            }
        }
    } else {
        key.colorAttachmentFormat = targetColorFormat;
    }
    key.depthAttachmentFormat = targetDepthFormat;

    VkVAOResource& vao = vaos_[currentVaoId_];
    uint32_t activeAttrCount = 0;
    std::vector<VkBuffer> boundVBOs;
    std::vector<VkDeviceSize> boundOffsets;

    for (const auto& aInfo : prog.activeAttribs) {
        uint32_t i = static_cast<uint32_t>(aInfo.location);
        if (i >= 16) continue;
        const VkVertexAttribute& attr = vao.attributes[i];
        uint32_t bindingIdx = activeAttrCount;

        if (attr.enabled && attr.bufferId != 0) {
            auto bIt = buffers_.find(attr.bufferId);
            if (bIt != buffers_.end() && bIt->second.isValid()) {
                key.attributes[activeAttrCount].location = i;
                key.attributes[activeAttrCount].binding = bindingIdx;
                key.attributes[activeAttrCount].format = glTypeToVkFormat(attr.type, attr.size, attr.normalized, attr.isInteger);
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
        } else if (fallbackConstantBuffer_ != VK_NULL_HANDLE) {
            bool isInt = (aInfo.type == GL_INT || aInfo.type == GL_UNSIGNED_INT ||
                          aInfo.type == GL_INT_VEC2 || aInfo.type == GL_UNSIGNED_INT_VEC2 ||
                          aInfo.type == GL_INT_VEC3 || aInfo.type == GL_UNSIGNED_INT_VEC3 ||
                          aInfo.type == GL_INT_VEC4 || aInfo.type == GL_UNSIGNED_INT_VEC4);
            key.attributes[activeAttrCount].location = i;
            key.attributes[activeAttrCount].binding = bindingIdx;
            key.attributes[activeAttrCount].format = isInt
                ? VK_FORMAT_R32G32B32A32_UINT
                : VK_FORMAT_R32G32B32A32_SFLOAT;
            key.attributes[activeAttrCount].offset = 0;

            key.bindings[activeAttrCount].binding = bindingIdx;
            key.bindings[activeAttrCount].stride = 0;
            key.bindings[activeAttrCount].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

            boundVBOs.push_back(fallbackConstantBuffer_);
            boundOffsets.push_back(i * 16);
            activeAttrCount++;
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
    vp.y = static_cast<float>(targetExtent.height) - viewport_.y;
    vp.width = viewport_.width;
    vp.height = -viewport_.height;
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(currentCmd_, 0, 1, &vp);

    VkRect2D sc{};
    if (scissorTest_) {
        sc.offset.x = scissor_.offset.x;
        sc.offset.y = static_cast<int32_t>(targetExtent.height) - (scissor_.offset.y + static_cast<int32_t>(scissor_.extent.height));
        sc.extent = scissor_.extent;
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
            GLenum sampType = GL_SAMPLER_2D;
            auto typeIt = prog.samplerTypes.find(binding);
            if (typeIt != prog.samplerTypes.end()) {
                sampType = typeIt->second;
            }
            GLuint texId = 0;
            if (sampType == GL_SAMPLER_CUBE || sampType == 0x8DC5) {
                if (texUnit < boundTexturesCubeMap_.size()) texId = boundTexturesCubeMap_[texUnit];
            } else if (sampType == 0x8DC1 || sampType == 0x8DC4) {
                if (texUnit < boundTextures2DArray_.size()) texId = boundTextures2DArray_[texUnit];
            } else if (sampType == 0x8B5F) {
                if (texUnit < boundTextures3D_.size()) texId = boundTextures3D_[texUnit];
            } else {
                if (texUnit < boundTextures2D_.size()) texId = boundTextures2D_[texUnit];
            }
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

            if (texUnit < boundSamplers_.size() && boundSamplers_[texUnit] != 0) {
                auto smpIt = samplers_.find(boundSamplers_[texUnit]);
                if (smpIt != samplers_.end()) {
                    updateSamplerObject(smpIt->second);
                    sampler = smpIt->second.sampler;
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

        std::vector<VkDescriptorBufferInfo> bufferInfos;
        bufferInfos.reserve(8);
        for (uint32_t binding = 8; binding < 16; ++binding) {
            VkDescriptorBufferInfo bufInfo{};
            bufInfo.buffer = dummyUniformBuffer_;
            bufInfo.offset = 0;
            bufInfo.range = 256;

            for (const auto& ub : prog.uniformBlocks) {
                if (ub.descriptorBinding == binding) {
                    GLuint blockIdx = ub.index;
                    GLuint bindingPoint = ub.binding;
                    auto itBind = prog.uniformBlockBindings.find(blockIdx);
                    if (itBind != prog.uniformBlockBindings.end()) {
                        bindingPoint = itBind->second;
                    }
                    if (bindingPoint < boundUniformBuffers_.size()) {
                        GLuint bufId = boundUniformBuffers_[bindingPoint];
                        if (bufId != 0) {
                            auto bIt = buffers_.find(bufId);
                            if (bIt != buffers_.end() && bIt->second.buffer != VK_NULL_HANDLE) {
                                bufInfo.buffer = bIt->second.buffer;
                                bufInfo.offset = 0;
                                bufInfo.range = (bIt->second.size > 0) ? bIt->second.size : 256;
                            }
                        }
                    }
                    break;
                }
            }

            bufferInfos.push_back(bufInfo);
            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = prog.descriptorSet;
            write.dstBinding = binding;
            write.dstArrayElement = 0;
            write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            write.descriptorCount = 1;
            write.pBufferInfo = &bufferInfos.back();
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

    VkBuffer indexBuf = iboIt->second.buffer;
    VkDeviceSize indexOffset = offset;
    VkIndexType idxType;

    if (type == GL_UNSIGNED_BYTE) {
        const auto& shadow = iboIt->second.shadowData;
        if (offset + count > shadow.size()) return;
        std::vector<uint16_t> expanded(count);
        for (GLsizei i = 0; i < count; ++i) {
            expanded[i] = static_cast<uint16_t>(shadow[offset + i]);
        }
        uploadScratchIndexBuffer(expanded.data(), count * sizeof(uint16_t));
        indexBuf = scratchIndexBuffer_;
        indexOffset = 0;
        idxType = VK_INDEX_TYPE_UINT16;
    } else {
        idxType = (type == GL_UNSIGNED_SHORT) ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
    }

    if (indexBuf == VK_NULL_HANDLE) return;

    vkCmdBindIndexBuffer(currentCmd_, indexBuf, indexOffset, idxType);

    vkCmdDrawIndexed(currentCmd_, static_cast<uint32_t>(count), static_cast<uint32_t>(instanceCount), 0, 0, 0);
}

} // namespace bro::webgl::vk
