#include "webgl/vulkan/webgl_vk_context.h"
#include "render/vulkan_util.h"
#include "util/log.h"

#include <algorithm>
#include <cstring>

namespace bro::webgl::vk {

namespace {

bool isIntegerAttribType(GLenum type) {
    return type == GL_INT || type == GL_UNSIGNED_INT ||
           type == GL_INT_VEC2 || type == GL_UNSIGNED_INT_VEC2 ||
           type == GL_INT_VEC3 || type == GL_UNSIGNED_INT_VEC3 ||
           type == GL_INT_VEC4 || type == GL_UNSIGNED_INT_VEC4;
}

uint64_t handleBits(const void* h) { return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(h)); }

} // namespace

void WebGLVkContext::clear(GLbitfield mask) {
    beginRendering();
    if (!inRenderPass_) return;

    VkExtent2D extent{canvas_.width(), canvas_.height()};
    bool hasColor = true;
    if (currentFboId_ != 0) {
        hasColor = false;
        auto itFbo = framebuffers_.find(currentFboId_);
        if (itFbo != framebuffers_.end() && itFbo->second.colorAttachmentTex != 0) {
            auto itTex = textures_.find(itFbo->second.colorAttachmentTex);
            if (itTex != textures_.end() && itTex->second.isValid()) {
                extent = {itTex->second.width, itTex->second.height};
                hasColor = true;
            }
        }
    }

    VkClearAttachment clearAttachments[2]{};
    uint32_t clearCount = 0;
    if ((mask & GL_COLOR_BUFFER_BIT) && hasColor) {
        clearAttachments[clearCount].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        clearAttachments[clearCount].colorAttachment = 0;
        clearAttachments[clearCount].clearValue.color = {{clearColor_[0], clearColor_[1], clearColor_[2], clearColor_[3]}};
        clearCount++;
    }
    // Framebuffer objects render without a depth attachment (yet); only the
    // canvas has one to clear.
    if ((mask & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)) && currentFboId_ == 0 &&
        canvas_.depthImage() != VK_NULL_HANDLE) {
        VkImageAspectFlags aspect = 0;
        const VkImageAspectFlags available = render::imageAspectFor(canvas_.depthFormat());
        if (mask & GL_DEPTH_BUFFER_BIT) aspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
        if (mask & GL_STENCIL_BUFFER_BIT) aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
        aspect &= available;
        if (aspect != 0) {
            clearAttachments[clearCount].aspectMask = aspect;
            clearAttachments[clearCount].clearValue.depthStencil = {clearDepth_, static_cast<uint32_t>(clearStencil_)};
            clearCount++;
        }
    }
    if (clearCount == 0) return;

    // GL's scissor box is bottom-up; the render target is top-down.
    int32_t x0 = 0, y0 = 0;
    int32_t x1 = static_cast<int32_t>(extent.width), y1 = static_cast<int32_t>(extent.height);
    if (scissorTest_) {
        x0 = std::max(x0, scissor_.offset.x);
        x1 = std::min(x1, scissor_.offset.x + static_cast<int32_t>(scissor_.extent.width));
        const int32_t top = static_cast<int32_t>(extent.height) -
                            (scissor_.offset.y + static_cast<int32_t>(scissor_.extent.height));
        y0 = std::max(y0, top);
        y1 = std::min(y1, top + static_cast<int32_t>(scissor_.extent.height));
    }
    if (x1 <= x0 || y1 <= y0) return;

    VkClearRect clearRect{};
    clearRect.rect.offset = {x0, y0};
    clearRect.rect.extent = {static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(y1 - y0)};
    clearRect.baseArrayLayer = 0;
    clearRect.layerCount = 1;
    vkCmdClearAttachments(currentCmd_, clearCount, clearAttachments, 1, &clearRect);
}

VkProgramResource* WebGLVkContext::drawProgram(const char* what) {
    auto itProg = programs_.find(currentProgramId_);
    if (itProg == programs_.end() || !itProg->second.linkStatus) {
        LOG_ERROR("WebGLVkContext: %s called with unlinked program (%u)", what, currentProgramId_);
        setSyntheticError(GL_INVALID_OPERATION);
        return nullptr;
    }
    VkProgramResource& prog = itProg->second;
    if (prog.vertModule == VK_NULL_HANDLE || prog.fragModule == VK_NULL_HANDLE) {
        LOG_ERROR("WebGLVkContext: Program %u missing valid shader module", currentProgramId_);
        setSyntheticError(GL_INVALID_OPERATION);
        return nullptr;
    }
    return &prog;
}

void WebGLVkContext::buildPipelineKey(GLenum mode, const VkProgramResource& prog, PipelineKey& key,
                                      VkExtent2D& extent) {
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

    extent = {canvas_.width(), canvas_.height()};
    key.colorAttachmentCount = 1;
    key.colorAttachmentFormats[0] = canvas_.colorFormat();
    key.colorAttachmentFormat = canvas_.colorFormat();
    key.depthAttachmentFormat = canvas_.depthFormat();
    if (currentFboId_ == 0) return;

    auto itFbo = framebuffers_.find(currentFboId_);
    if (itFbo == framebuffers_.end()) return;
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
        auto itTex = texId != 0 ? textures_.find(texId) : textures_.end();
        if (itTex != textures_.end() && itTex->second.isValid()) {
            fmt = itTex->second.format;
            extent = {itTex->second.width, itTex->second.height};
        }
        if (count < 8) key.colorAttachmentFormats[count++] = fmt;
    }
    if (count > 0) {
        key.colorAttachmentCount = count;
        key.colorAttachmentFormat = key.colorAttachmentFormats[0];
    } else if (fbo.colorAttachmentTex != 0) {
        auto itTex = textures_.find(fbo.colorAttachmentTex);
        if (itTex != textures_.end() && itTex->second.isValid()) {
            extent = {itTex->second.width, itTex->second.height};
            key.colorAttachmentFormats[0] = itTex->second.format;
            key.colorAttachmentFormat = itTex->second.format;
        }
    }
    // beginRendering gives framebuffer objects no depth attachment.
    key.depthAttachmentFormat = VK_FORMAT_UNDEFINED;
}

void WebGLVkContext::bindVertexInputs(const VkProgramResource& prog, PipelineKey& key,
                                      std::vector<VkBuffer>& vbos, std::vector<VkDeviceSize>& offsets) {
    VkVAOResource& vao = vaos_[currentVaoId_];
    uint32_t count = 0;
    for (const auto& aInfo : prog.activeAttribs) {
        const uint32_t loc = static_cast<uint32_t>(aInfo.location);
        if (loc >= 16) continue;
        const VkVertexAttribute& attr = vao.attributes[loc];
        auto bIt = (attr.enabled && attr.bufferId != 0) ? buffers_.find(attr.bufferId) : buffers_.end();
        if (bIt != buffers_.end() && bIt->second.isValid()) {
            key.attributes[count] = {loc, count, glTypeToVkFormat(attr.type, attr.size, attr.normalized, attr.isInteger), 0};
            key.bindings[count] = {count, static_cast<uint32_t>(attr.stride),
                                   attr.divisor > 0 ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
            vbos.push_back(bIt->second.buffer);
            offsets.push_back(attr.offset);
        } else {
            // A disabled array reads the attribute's constant value: a zero
            // stride binding over this frame's copy of the generic values.
            if (genericAttribSerial_ != context_.frames().frameSerial() || !genericAttribSlice_) {
                genericAttribSlice_ = stage(genericAttribs_.data(), sizeof(genericAttribs_));
                genericAttribSerial_ = context_.frames().frameSerial();
            }
            if (!genericAttribSlice_) continue;
            key.attributes[count] = {loc, count, isIntegerAttribType(aInfo.type) ? VK_FORMAT_R32G32B32A32_UINT
                                                                                 : VK_FORMAT_R32G32B32A32_SFLOAT, 0};
            key.bindings[count] = {count, 0, VK_VERTEX_INPUT_RATE_VERTEX};
            vbos.push_back(genericAttribSlice_.buffer);
            offsets.push_back(genericAttribSlice_.offset + loc * 16);
        }
        count++;
    }
    key.attributeCount = count;
    key.bindingCount = count;
}

// Bind the program's samplers, uniform blocks and default-block uniforms as
// one descriptor set. The set comes from the frame's descriptor arena and is
// written once, before any command uses it; a draw whose bindings match the
// previous draw of the same program in the same frame reuses that set.
bool WebGLVkContext::bindProgramResources(VkCommandBuffer cmd, VkProgramResource& prog) {
    const uint64_t serial = context_.frames().frameSerial();
    const bool sameFrame = prog.drawFrameSerial == serial;

    if (!sameFrame || prog.drawUniformBytes != prog.uniformBytes) {
        render::UploadSlice slice = stage(prog.uniformBytes.data(), prog.uniformBytes.size(),
                                          context_.deviceProperties().limits.minUniformBufferOffsetAlignment);
        if (!slice) return false;
        prog.drawUniforms = {slice.buffer, slice.offset, prog.uniformBytes.size()};
        prog.drawUniformBytes = prog.uniformBytes;
    }

    VkDescriptorImageInfo images[kMaxSamplerBindings]{};
    VkDescriptorBufferInfo blocks[kMaxUniformBlockBindings]{};
    std::vector<uint64_t> key;
    key.reserve(kMaxSamplerBindings * 2 + kMaxUniformBlockBindings * 3 + 2);

    for (uint32_t binding = 0; binding < kMaxSamplerBindings; ++binding) {
        uint32_t texUnit = 0;
        if (auto sIt = prog.samplerBindings.find(binding); sIt != prog.samplerBindings.end()) texUnit = sIt->second;
        GLenum sampType = GL_SAMPLER_2D;
        if (auto tIt = prog.samplerTypes.find(binding); tIt != prog.samplerTypes.end()) sampType = tIt->second;

        GLuint texId = 0;
        if (sampType == GL_SAMPLER_CUBE || sampType == 0x8DC5) {
            if (texUnit < boundTexturesCubeMap_.size()) texId = boundTexturesCubeMap_[texUnit];
        } else if (sampType == 0x8DC1 || sampType == 0x8DC4) {
            if (texUnit < boundTextures2DArray_.size()) texId = boundTextures2DArray_[texUnit];
        } else if (sampType == 0x8B5F) {
            if (texUnit < boundTextures3D_.size()) texId = boundTextures3D_[texUnit];
        } else if (texUnit < boundTextures2D_.size()) {
            texId = boundTextures2D_[texUnit];
        }

        VkImageView view = dummyView_;
        VkSampler sampler = dummySampler_;
        auto tIt = texId != 0 ? textures_.find(texId) : textures_.end();
        if (tIt != textures_.end() && tIt->second.isValid() &&
            tIt->second.currentLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            updateTextureSampler(tIt->second);
            view = tIt->second.view;
            sampler = tIt->second.sampler;
        }
        if (texUnit < boundSamplers_.size() && boundSamplers_[texUnit] != 0) {
            if (auto smpIt = samplers_.find(boundSamplers_[texUnit]); smpIt != samplers_.end()) {
                updateSamplerObject(smpIt->second);
                sampler = smpIt->second.sampler;
            }
        }
        images[binding] = {sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        key.push_back(handleBits(view));
        key.push_back(handleBits(sampler));
    }

    for (uint32_t i = 0; i < kMaxUniformBlockBindings; ++i) {
        const uint32_t binding = kFirstUniformBlockBinding + i;
        VkDescriptorBufferInfo info{dummyUniformBuffer_, 0, 256};
        for (const auto& ub : prog.uniformBlocks) {
            if (ub.descriptorBinding != binding) continue;
            GLuint bindingPoint = ub.binding;
            if (auto itBind = prog.uniformBlockBindings.find(ub.index); itBind != prog.uniformBlockBindings.end())
                bindingPoint = itBind->second;
            if (bindingPoint < boundUniformBuffers_.size() && boundUniformBuffers_[bindingPoint] != 0) {
                auto bIt = buffers_.find(boundUniformBuffers_[bindingPoint]);
                if (bIt != buffers_.end() && bIt->second.isValid())
                    info = {bIt->second.buffer, 0, bIt->second.size};
            }
            break;
        }
        blocks[i] = info;
        key.push_back(handleBits(info.buffer));
        key.push_back(info.offset);
        key.push_back(info.range);
    }
    key.push_back(handleBits(prog.drawUniforms.buffer));
    key.push_back(prog.drawUniforms.offset);

    if (!sameFrame || prog.drawSet == VK_NULL_HANDLE || key != prog.drawBindingKey) {
        VkDescriptorSet set = context_.frames().allocDescriptorSet(descriptorSetLayout_);
        if (set == VK_NULL_HANDLE) {
            setSyntheticError(GL_OUT_OF_MEMORY);
            return false;
        }
        VkWriteDescriptorSet writes[kMaxSamplerBindings + kMaxUniformBlockBindings + 1]{};
        uint32_t n = 0;
        for (uint32_t b = 0; b < kMaxSamplerBindings; ++b, ++n) {
            writes[n] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, b, 0, 1,
                         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &images[b], nullptr, nullptr};
        }
        for (uint32_t i = 0; i < kMaxUniformBlockBindings; ++i, ++n) {
            writes[n] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, kFirstUniformBlockBinding + i, 0, 1,
                         VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &blocks[i], nullptr};
        }
        writes[n++] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, kDefaultUniformBinding, 0, 1,
                       VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &prog.drawUniforms, nullptr};
        vkUpdateDescriptorSets(context_.device(), n, writes, 0, nullptr);
        prog.drawSet = set;
        prog.drawBindingKey = std::move(key);
    }
    prog.drawFrameSerial = serial;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1, &prog.drawSet, 0, nullptr);
    return true;
}

// Everything a draw needs bound before its vkCmdDraw*: the render pass, the
// pipeline for the current state, viewport/scissor, descriptors and vertex
// buffers. Returns false (with the GL error set) when the draw cannot happen.
bool WebGLVkContext::prepareDraw(GLenum mode, VkProgramResource& prog) {
    beginRendering();
    if (!inRenderPass_) return false;
    VkCommandBuffer cmd = currentCmd_;

    PipelineKey key{};
    VkExtent2D extent{};
    buildPipelineKey(mode, prog, key, extent);
    std::vector<VkBuffer> vbos;
    std::vector<VkDeviceSize> offsets;
    bindVertexInputs(prog, key, vbos, offsets);

    VkPipeline pipeline = pipelineCache_.getOrCreatePipeline(key, pipelineLayout_);
    if (pipeline == VK_NULL_HANDLE) {
        LOG_ERROR("WebGLVkContext: Failed to obtain graphics pipeline");
        setSyntheticError(GL_INVALID_OPERATION);
        return false;
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    // Negative viewport height maps GL's bottom-up NDC onto the top-down target.
    VkViewport vp{};
    vp.x = viewport_.x;
    vp.y = static_cast<float>(extent.height) - viewport_.y;
    vp.width = viewport_.width;
    vp.height = -viewport_.height;
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);

    VkRect2D sc{};
    if (scissorTest_) {
        int32_t x0 = std::max(0, scissor_.offset.x);
        int32_t top = static_cast<int32_t>(extent.height) -
                      (scissor_.offset.y + static_cast<int32_t>(scissor_.extent.height));
        int32_t y0 = std::max(0, top);
        int32_t x1 = std::min(static_cast<int32_t>(extent.width),
                              scissor_.offset.x + static_cast<int32_t>(scissor_.extent.width));
        int32_t y1 = std::min(static_cast<int32_t>(extent.height),
                              top + static_cast<int32_t>(scissor_.extent.height));
        sc.offset = {x0, y0};
        sc.extent = {static_cast<uint32_t>(std::max(0, x1 - x0)), static_cast<uint32_t>(std::max(0, y1 - y0))};
    } else {
        sc.extent = extent;
    }
    vkCmdSetScissor(cmd, 0, 1, &sc);

    if (!bindProgramResources(cmd, prog)) return false;
    if (!vbos.empty())
        vkCmdBindVertexBuffers(cmd, 0, static_cast<uint32_t>(vbos.size()), vbos.data(), offsets.data());
    return true;
}

void WebGLVkContext::drawArrays(GLenum mode, GLint first, GLsizei count) {
    drawArraysInstanced(mode, first, count, 1);
}

void WebGLVkContext::drawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instanceCount) {
    if (count <= 0 || instanceCount <= 0) return;
    VkProgramResource* prog = drawProgram("drawArrays");
    if (!prog) return;
    if (!prepareDraw(mode, *prog)) return;
    vkCmdDraw(currentCmd_, static_cast<uint32_t>(count), static_cast<uint32_t>(instanceCount),
              static_cast<uint32_t>(first), 0);
}

void WebGLVkContext::drawElements(GLenum mode, GLsizei count, GLenum type, uintptr_t offset) {
    drawElementsInstanced(mode, count, type, offset, 1);
}

void WebGLVkContext::drawElementsInstanced(GLenum mode, GLsizei count, GLenum type,
                                          uintptr_t offset, GLsizei instanceCount) {
    if (count <= 0 || instanceCount <= 0) return;
    if (currentProgramId_ == 0) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }

    GLuint iboId = vaos_[currentVaoId_].elementArrayBufferId;
    if (iboId == 0) {
        LOG_ERROR("WebGLVkContext: drawElements called with no element array buffer bound");
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    auto iboIt = buffers_.find(iboId);
    if (iboIt == buffers_.end() || !iboIt->second.isValid()) {
        LOG_ERROR("WebGLVkContext: Bound element array buffer %u is invalid", iboId);
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    VkProgramResource* prog = drawProgram("drawElements");
    if (!prog) return;

    // Vulkan has no 8-bit indices (without an extension): widen them into
    // this draw's own slice of the upload ring.
    VkBuffer indexBuf = iboIt->second.buffer;
    VkDeviceSize indexOffset = offset;
    VkIndexType idxType = (type == GL_UNSIGNED_SHORT) ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
    if (type == GL_UNSIGNED_BYTE) {
        const auto& shadow = iboIt->second.shadowData;
        if (offset + static_cast<size_t>(count) > shadow.size()) {
            setSyntheticError(GL_INVALID_OPERATION);
            return;
        }
        render::UploadSlice slice = stage(nullptr, static_cast<VkDeviceSize>(count) * sizeof(uint16_t), 4);
        if (!slice) return;
        auto* expanded = static_cast<uint16_t*>(slice.mapped);
        for (GLsizei i = 0; i < count; ++i) expanded[i] = shadow[offset + i];
        indexBuf = slice.buffer;
        indexOffset = slice.offset;
        idxType = VK_INDEX_TYPE_UINT16;
    }

    if (!prepareDraw(mode, *prog)) return;
    vkCmdBindIndexBuffer(currentCmd_, indexBuf, indexOffset, idxType);
    vkCmdDrawIndexed(currentCmd_, static_cast<uint32_t>(count), static_cast<uint32_t>(instanceCount), 0, 0, 0);
}

} // namespace bro::webgl::vk
