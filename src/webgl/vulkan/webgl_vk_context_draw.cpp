#include "webgl/vulkan/webgl_vk_context.h"
#include "render/vulkan_util.h"
#include "util/log.h"

#include <algorithm>
#include <cstring>

namespace bro::webgl::vk {

namespace {

uint64_t handleBits(const void* h) { return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(h)); }

} // namespace

// The first target row of GL's bottom-up scissor box: counted down from the
// top on the top-down canvas, as is on a framebuffer object.
int32_t WebGLVkContext::scissorTop(VkExtent2D extent) const {
    if (!pass_.topDown) return scissor_.offset.y;
    return static_cast<int32_t>(extent.height) - (scissor_.offset.y + static_cast<int32_t>(scissor_.extent.height));
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
    // Drawn unflipped, a framebuffer object sees GL's geometry mirrored in
    // Vulkan's y-down window space, so its winding reads the other way.
    if (!pass_.topDown)
        key.frontFace = key.frontFace == VK_FRONT_FACE_CLOCKWISE ? VK_FRONT_FACE_COUNTER_CLOCKWISE
                                                                 : VK_FRONT_FACE_CLOCKWISE;

    // Without a depth (stencil) buffer GL's depth (stencil) test always passes.
    key.depthTestEnable = depthTestEnabled_ && pass_.depth ? VK_TRUE : VK_FALSE;
    key.depthWriteEnable = key.depthTestEnable && depthMask_ ? VK_TRUE : VK_FALSE;
    key.depthCompareOp = key.depthTestEnable ? glCompareOpToVk(depthFunc_) : VK_COMPARE_OP_ALWAYS;

    key.stencilTestEnable = stencilTestEnabled_ && pass_.stencil ? VK_TRUE : VK_FALSE;
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

    extent = pass_.extent;
    key.colorAttachmentCount = pass_.colorCount;
    for (uint32_t i = 0; i < 8; ++i)
        key.colorAttachmentFormats[i] = i < pass_.colorCount && pass_.color[i] ? pass_.color[i].format
                                                                              : VK_FORMAT_UNDEFINED;
    key.depthAttachmentFormat = pass_.depth ? pass_.depth.format : VK_FORMAT_UNDEFINED;
    key.stencilAttachmentFormat = pass_.stencil ? pass_.stencil.format : VK_FORMAT_UNDEFINED;
    key.samples = pass_.samples;
}

void WebGLVkContext::bindVertexInputs(const VkProgramResource& prog, PipelineKey& key,
                                      std::vector<VkBuffer>& vbos, std::vector<VkDeviceSize>& offsets) {
    VkVAOResource& vao = vaos_[currentVaoId_];
    uint32_t count = 0;
    for (const VkVertexInput& in : prog.iface.vertexInputs) {
        const uint32_t loc = in.location;
        if (loc >= vao.attributes.size()) continue;
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
            // stride binding over the segment's copy of the generic values.
            if (genericAttribSerial_ != stream_.segmentSerial() || !genericAttribSlice_) {
                genericAttribSlice_ = stage(genericAttribs_.data(), sizeof(genericAttribs_));
                genericAttribSerial_ = stream_.segmentSerial();
            }
            if (!genericAttribSlice_) continue;
            VkFormat format = VK_FORMAT_R32G32B32A32_SFLOAT;
            if (in.kind == VkVertexInput::Kind::Int) format = VK_FORMAT_R32G32B32A32_SINT;
            if (in.kind == VkVertexInput::Kind::Uint) format = VK_FORMAT_R32G32B32A32_UINT;
            key.attributes[count] = {loc, count, format, 0};
            key.bindings[count] = {count, 0, VK_VERTEX_INPUT_RATE_VERTEX};
            vbos.push_back(genericAttribSlice_.buffer);
            offsets.push_back(genericAttribSlice_.offset + loc * 16);
        }
        count++;
    }
    key.attributeCount = count;
    key.bindingCount = count;
}

// The texture a sampler of `type` reads from texture unit `unit`.
GLuint WebGLVkContext::textureForSampler(GLenum type, uint32_t unit) const {
    if (unit >= boundTextures2D_.size()) return 0;
    switch (type) {
        case GL_SAMPLER_CUBE: case GL_SAMPLER_CUBE_SHADOW: case GL_INT_SAMPLER_CUBE: case GL_UNSIGNED_INT_SAMPLER_CUBE:
            return boundTexturesCubeMap_[unit];
        case GL_SAMPLER_2D_ARRAY: case GL_SAMPLER_2D_ARRAY_SHADOW: case GL_INT_SAMPLER_2D_ARRAY:
        case GL_UNSIGNED_INT_SAMPLER_2D_ARRAY:
            return boundTextures2DArray_[unit];
        case GL_SAMPLER_3D: case GL_INT_SAMPLER_3D: case GL_UNSIGNED_INT_SAMPLER_3D:
            return boundTextures3D_[unit];
        default:
            return boundTextures2D_[unit];
    }
}

// The range of the buffer bound to `block`'s binding point, or false (with
// INVALID_OPERATION) when WebGL forbids the draw: no buffer there, or a
// range smaller than the block.
bool WebGLVkContext::uniformBlockRange(const VkProgramResource& prog, size_t block, VkDescriptorBufferInfo& out) {
    const VkUniformBlockInfo& info = prog.iface.uniformBlocks[block];
    const GLuint point = prog.blockBindings[block];
    const IndexedBuffer& binding = point < boundUniformBuffers_.size() ? boundUniformBuffers_[point] : IndexedBuffer{};
    auto bIt = binding.buffer != 0 ? buffers_.find(binding.buffer) : buffers_.end();
    if (bIt == buffers_.end() || !bIt->second.isValid()) {
        LOG_ERROR("WebGLVkContext: no buffer bound for uniform block '%s'", info.name.c_str());
        setSyntheticError(GL_INVALID_OPERATION);
        return false;
    }
    const VkDeviceSize bufferSize = bIt->second.size;
    const VkDeviceSize offset = static_cast<VkDeviceSize>(binding.offset);
    VkDeviceSize range = offset < bufferSize ? bufferSize - offset : 0;
    if (binding.size > 0) range = std::min(range, static_cast<VkDeviceSize>(binding.size));
    if (range < info.dataSize) {
        LOG_ERROR("WebGLVkContext: uniform block '%s' needs %u bytes, its buffer range has %llu", info.name.c_str(),
                  info.dataSize, static_cast<unsigned long long>(range));
        setSyntheticError(GL_INVALID_OPERATION);
        return false;
    }
    range = std::min<VkDeviceSize>(range, context_.deviceProperties().limits.maxUniformBufferRange);
    out = {bIt->second.buffer, offset, range};
    return true;
}

// Bind the program's samplers, uniform blocks and default-block uniforms as
// one descriptor set. The set comes from the stream segment and is written
// once, before any command uses it; a draw whose bindings match the previous
// draw of the same program in the same segment reuses that set.
bool WebGLVkContext::bindProgramResources(VkCommandBuffer cmd, VkProgramResource& prog) {
    const ProgramInterface& iface = prog.iface;
    const uint64_t serial = stream_.segmentSerial();
    const bool sameSegment = prog.drawSegmentSerial == serial;

    std::vector<VkDescriptorBufferInfo> blocks(iface.uniformBlocks.size());
    for (size_t i = 0; i < blocks.size(); ++i)
        if (!uniformBlockRange(prog, i, blocks[i])) return false;

    if (iface.defaultBlockBinding >= 0 &&
        (!sameSegment || prog.drawUniforms.buffer == VK_NULL_HANDLE || prog.drawUniformBytes != prog.uniformBytes)) {
        render::UploadSlice slice = stage(prog.uniformBytes.data(), prog.uniformBytes.size(),
                                          context_.deviceProperties().limits.minUniformBufferOffsetAlignment);
        if (!slice) return false;
        prog.drawUniforms = {slice.buffer, slice.offset, prog.uniformBytes.size()};
        prog.drawUniformBytes = prog.uniformBytes;
    }

    std::vector<VkDescriptorImageInfo> images(iface.samplerUnitCount);
    std::vector<uint64_t> key;
    key.reserve(images.size() * 2 + blocks.size() * 3 + 2);
    for (const VkSamplerBinding& s : iface.samplers) {
        for (uint32_t e = 0; e < s.count; ++e) {
            const uint32_t unit = prog.samplerUnits[s.firstUnit + e];
            VkImageView view = placeholderView(s.type);
            VkSampler sampler = placeholderSampler_;
            const GLuint texId = textureForSampler(s.type, unit);
            auto tIt = texId != 0 ? textures_.find(texId) : textures_.end();
            if (tIt != textures_.end() && tIt->second.isValid() &&
                tIt->second.currentLayout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
                // Only the open pass leaves a texture in attachment layout:
                // sampling what the draw writes is WebGL's feedback loop.
                LOG_ERROR("WebGLVkContext: draw samples texture %u, which the draw framebuffer renders into", texId);
                setSyntheticError(GL_INVALID_OPERATION);
                return false;
            }
            if (tIt != textures_.end() && tIt->second.isValid()) {
                updateTextureSampler(tIt->second);
                view = tIt->second.view;
                sampler = tIt->second.sampler;
                if (unit < boundSamplers_.size() && boundSamplers_[unit] != 0) {
                    if (auto smpIt = samplers_.find(boundSamplers_[unit]); smpIt != samplers_.end()) {
                        updateSamplerObject(smpIt->second);
                        sampler = smpIt->second.sampler;
                    }
                }
            }
            images[s.firstUnit + e] = {sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            key.push_back(handleBits(view));
            key.push_back(handleBits(sampler));
        }
    }
    for (const VkDescriptorBufferInfo& b : blocks) {
        key.push_back(handleBits(b.buffer));
        key.push_back(b.offset);
        key.push_back(b.range);
    }
    key.push_back(handleBits(prog.drawUniforms.buffer));
    key.push_back(prog.drawUniforms.offset);

    if (!sameSegment || prog.drawSet == VK_NULL_HANDLE || key != prog.drawBindingKey) {
        VkDescriptorSet set = stream_.allocDescriptorSet(prog.setLayout);
        if (set == VK_NULL_HANDLE) {
            setSyntheticError(GL_OUT_OF_MEMORY);
            return false;
        }
        std::vector<VkWriteDescriptorSet> writes;
        writes.reserve(iface.samplers.size() + blocks.size() + 1);
        for (const VkSamplerBinding& s : iface.samplers)
            writes.push_back({VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, s.binding, 0, s.count,
                              VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &images[s.firstUnit], nullptr, nullptr});
        for (size_t i = 0; i < blocks.size(); ++i) {
            const VkUniformBlockInfo& b = iface.uniformBlocks[i];
            writes.push_back({VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, b.descriptorBinding,
                              b.arrayElement, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &blocks[i], nullptr});
        }
        if (iface.defaultBlockBinding >= 0)
            writes.push_back({VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set,
                              static_cast<uint32_t>(iface.defaultBlockBinding), 0, 1,
                              VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &prog.drawUniforms, nullptr});
        vkUpdateDescriptorSets(context_.device(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        prog.drawSet = set;
        prog.drawBindingKey = std::move(key);
    }
    prog.drawSegmentSerial = serial;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, prog.pipelineLayout, 0, 1, &prog.drawSet, 0,
                            nullptr);
    return true;
}

// Everything a draw needs bound before its vkCmdDraw*: the render pass, the
// pipeline for the current state, viewport/scissor, descriptors and vertex
// buffers. Returns false (with the GL error set) when the draw cannot happen.
bool WebGLVkContext::prepareDraw(GLenum mode, VkProgramResource& prog) {
    beginRendering();
    if (!inRenderPass_) return false;
    VkCommandBuffer cmd = commands();

    PipelineKey key{};
    VkExtent2D extent{};
    buildPipelineKey(mode, prog, key, extent);
    std::vector<VkBuffer> vbos;
    std::vector<VkDeviceSize> offsets;
    bindVertexInputs(prog, key, vbos, offsets);

    VkPipeline pipeline = pipelineCache_.getOrCreatePipeline(key, prog.pipelineLayout);
    if (pipeline == VK_NULL_HANDLE) {
        LOG_ERROR("WebGLVkContext: Failed to obtain graphics pipeline");
        setSyntheticError(GL_INVALID_OPERATION);
        return false;
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    // The canvas is drawn top-down (a negative viewport height maps GL's
    // bottom-up NDC onto it); a framebuffer object in GL's own row order.
    const bool flipped = pass_.topDown;
    const FragmentPush push = flipped ? FragmentPush{static_cast<float>(extent.height), -1.0f} : FragmentPush{};
    vkCmdPushConstants(cmd, prog.pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);

    VkViewport vp{};
    vp.x = viewport_.x;
    vp.y = flipped ? static_cast<float>(extent.height) - viewport_.y : viewport_.y;
    vp.width = viewport_.width;
    vp.height = flipped ? -viewport_.height : viewport_.height;
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);

    VkRect2D sc{};
    if (scissorTest_) {
        int32_t x0 = std::max(0, scissor_.offset.x);
        int32_t top = scissorTop(extent);
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
    flushIfOverBudget();
    VkProgramResource* prog = drawProgram("drawArrays");
    if (!prog) return;
    if (!prepareDraw(mode, *prog)) return;
    vkCmdDraw(commands(), static_cast<uint32_t>(count), static_cast<uint32_t>(instanceCount),
              static_cast<uint32_t>(first), 0);
}

void WebGLVkContext::drawElements(GLenum mode, GLsizei count, GLenum type, uintptr_t offset) {
    drawElementsInstanced(mode, count, type, offset, 1);
}

void WebGLVkContext::drawElementsInstanced(GLenum mode, GLsizei count, GLenum type,
                                          uintptr_t offset, GLsizei instanceCount) {
    if (count <= 0 || instanceCount <= 0) return;
    flushIfOverBudget();
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
    VkCommandBuffer cmd = commands();
    vkCmdBindIndexBuffer(cmd, indexBuf, indexOffset, idxType);
    vkCmdDrawIndexed(cmd, static_cast<uint32_t>(count), static_cast<uint32_t>(instanceCount), 0, 0, 0);
}

} // namespace bro::webgl::vk
