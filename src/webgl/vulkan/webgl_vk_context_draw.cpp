#include "webgl/vulkan/webgl_vk_context.h"
#include "render/vulkan_util.h"
#include "util/log.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bro::webgl::vk {

namespace {

// What a sampler uniform's type reads: float (or depth, uncompared),
// signed or unsigned integer texels, or a depth comparison.
enum class SamplerKind : uint8_t { Float, Int, Uint, Shadow };

SamplerKind samplerKind(GLenum type) {
    switch (type) {
        case GL_INT_SAMPLER_2D: case GL_INT_SAMPLER_3D: case GL_INT_SAMPLER_CUBE: case GL_INT_SAMPLER_2D_ARRAY:
            return SamplerKind::Int;
        case GL_UNSIGNED_INT_SAMPLER_2D: case GL_UNSIGNED_INT_SAMPLER_3D: case GL_UNSIGNED_INT_SAMPLER_CUBE:
        case GL_UNSIGNED_INT_SAMPLER_2D_ARRAY:
            return SamplerKind::Uint;
        case GL_SAMPLER_2D_SHADOW: case GL_SAMPLER_CUBE_SHADOW: case GL_SAMPLER_2D_ARRAY_SHADOW:
            return SamplerKind::Shadow;
        default: return SamplerKind::Float;
    }
}

// WebGL 2 5.22: a complete texture whose texels the sampler type cannot
// read (an integer texture through a float sampler, a comparing texture
// through a non-shadow one, ...) makes the draw INVALID_OPERATION.
bool samplerMatches(SamplerKind want, const TexFormat& tf, const SamplerState& state) {
    const bool compare = tf.isDepthOrStencil() && state.compareMode == GL_COMPARE_REF_TO_TEXTURE;
    switch (want) {
        case SamplerKind::Int: return tf.kind == TexKind::Int;
        case SamplerKind::Uint: return tf.kind == TexKind::Uint;
        case SamplerKind::Shadow: return compare;
        default: return !tf.isInteger() && !compare;
    }
}

} // namespace

// Whether the open pass renders into a level of `tex` in [base, base +
// count): sampling it would be a feedback loop (WebGL 2 5.18), and the
// level is in attachment layout besides.
bool WebGLVkContext::sampledByPass(const VkTextureResource& tex, uint32_t base, uint32_t count) const {
    if (!inRenderPass_) return false;
    auto renders = [&](const Surface& s) {
        return s.source == Surface::Source::Texture && s.tex == &tex && s.level >= base && s.level < base + count;
    };
    for (uint32_t i = 0; i < pass_.colorCount; ++i)
        if (renders(pass_.color[i])) return true;
    return renders(pass_.depth) || renders(pass_.stencil);
}

// The first target row of GL's bottom-up scissor box: counted down from the
// top on the top-down canvas, as is on a framebuffer object.
int32_t WebGLVkContext::scissorTop(VkExtent2D extent) const {
    if (!pass_.topDown) return scissor_.offset.y;
    return static_cast<int32_t>(extent.height) - (scissor_.offset.y + static_cast<int32_t>(scissor_.extent.height));
}

VkProgramResource* WebGLVkContext::drawProgram(const char* what) {
    if (!currentProgram_ || !currentProgram_->linkStatus) {
        LOG_ERROR("WebGLVkContext: %s called with unlinked program (%u)", what, currentProgramId_);
        setSyntheticError(GL_INVALID_OPERATION);
        return nullptr;
    }
    VkProgramResource& prog = *currentProgram_;
    if (prog.vertModule == VK_NULL_HANDLE || prog.fragModule == VK_NULL_HANDLE) {
        LOG_ERROR("WebGLVkContext: Program %u missing valid shader module", currentProgramId_);
        setSyntheticError(GL_INVALID_OPERATION);
        return nullptr;
    }
    if (samplerUnitConflict(prog)) {
        setSyntheticError(GL_INVALID_OPERATION);
        return nullptr;
    }
    return &prog;
}

// Every GL state read here is set through a setter that calls
// pipelineStateChanged() (webgl_vk_context.cpp), and pass_ only by
// beginRendering / endRendering, which call it too.
const PipelineKey& WebGLVkContext::fixedPipelineKey() {
    if (fixedKeySerial_ == pipelineStateSerial_) return fixedKey_;
    fixedKeySerial_ = pipelineStateSerial_;
    PipelineKey& key = fixedKey_;
    key = PipelineKey{};
    key.depthBiasEnable = polygonOffsetFillEnabled_ ? VK_TRUE : VK_FALSE;

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

    key.colorAttachmentCount = pass_.colorCount;
    for (uint32_t i = 0; i < 8; ++i)
        key.colorAttachmentFormats[i] = i < pass_.colorCount && pass_.color[i] ? pass_.color[i].format
                                                                              : VK_FORMAT_UNDEFINED;
    key.depthAttachmentFormat = pass_.depth ? pass_.depth.format : VK_FORMAT_UNDEFINED;
    key.stencilAttachmentFormat = pass_.stencil ? pass_.stencil.format : VK_FORMAT_UNDEFINED;
    key.samples = pass_.samples;
    for (uint32_t i = 0; i < pass_.colorCount; ++i)
        if (pass_.color[i].alphaOne) key.alphaOneMask |= static_cast<uint8_t>(1u << i);

    // SAMPLE_ALPHA_TO_COVERAGE and SAMPLE_COVERAGE only act on multisampled
    // targets; the coverage value keeps round(value * samples) samples.
    if (pass_.samples > VK_SAMPLE_COUNT_1_BIT) {
        key.alphaToCoverageEnable = sampleAlphaToCoverageEnabled_ ? VK_TRUE : VK_FALSE;
        if (sampleCoverageEnabled_) {
            const uint32_t samples = static_cast<uint32_t>(pass_.samples);
            const uint32_t all = samples >= 32 ? ~0u : (1u << samples) - 1;
            const uint32_t kept = static_cast<uint32_t>(std::lround(sampleCoverageValue_ * samples));
            const uint32_t mask = kept >= 32 ? ~0u : (1u << kept) - 1;
            key.sampleMask = (sampleCoverageInvert_ ? ~mask : mask) & all;
        }
    }
    return key;
}

// The pipeline for this draw: the program's last one when nothing it was
// made for has changed, else from the pipeline cache by the full key.
VkPipeline WebGLVkContext::drawPipeline(GLenum mode, VkProgramResource& prog, const DrawShape& shape,
                                        const VaoInputCache& inputs) {
    const VkPrimitiveTopology topology = glTopologyToVk(mode);
    const VkBool32 restart = shape.restart ? VK_TRUE : VK_FALSE;
    VkProgramResource::PipelineMemo& memo = prog.pipelineMemo;
    if (memo.pipeline != VK_NULL_HANDLE && memo.stateSerial == pipelineStateSerial_ &&
        memo.layoutId == inputs.layoutId && memo.topology == topology && memo.restart == restart &&
        memo.cacheGeneration == pipelineCache_.generation())
        return memo.pipeline;

    PipelineKey key = fixedPipelineKey();
    key.vertShader = prog.vertModule;
    key.fragShader = prog.fragModule;
    key.topology = topology;
    key.primitiveRestartEnable = restart;
    const VertexLayout& layout = inputs.layout;
    key.attributeCount = layout.count;
    key.bindingCount = layout.count;
    std::memcpy(key.attributes, layout.attributes, sizeof(key.attributes));
    std::memcpy(key.bindings, layout.bindings, sizeof(key.bindings));
    std::memcpy(key.divisors, layout.divisors, sizeof(key.divisors));
    const VkPipeline pipeline = pipelineCache_.getOrCreatePipeline(key, prog.pipelineLayout);
    if (pipeline == VK_NULL_HANDLE) {
        memo = {};
        return VK_NULL_HANDLE;
    }
    memo = {pipeline, pipelineStateSerial_, inputs.layoutId, pipelineCache_.generation(), topology, restart};
    return pipeline;
}

// The fixed-function values every pipeline takes as dynamic state.
void WebGLVkContext::setDynamicState(VkCommandBuffer cmd, VkExtent2D extent, bool discard) {
    // The canvas is drawn top-down (a negative viewport height maps GL's
    // bottom-up NDC onto it); a framebuffer object in GL's own row order.
    const bool flipped = pass_.topDown;
    VkViewport vp{};
    vp.x = viewport_.x;
    vp.y = flipped ? static_cast<float>(extent.height) - viewport_.y : viewport_.y;
    vp.width = viewport_.width;
    vp.height = flipped ? -viewport_.height : viewport_.height;
    vp.minDepth = depthNear_;
    vp.maxDepth = depthFar_;
    bound_.setViewport(cmd, vp);

    VkRect2D sc{};
    if (discard) {
        // RASTERIZER_DISCARD while capturing: the vertex stage runs, no
        // fragment does (an empty scissor; the pipeline keeps its fragment
        // state).
    } else if (scissorTest_) {
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
    bound_.setScissor(cmd, sc);

    // GL offsets depth by factor * DZ + units * r; Vulkan by its slope and
    // constant factors, the same terms.
    bound_.setDepthBias(cmd, polygonOffsetUnits_, polygonOffsetFactor_);
    bound_.setBlendConstants(cmd, blendColor_);
    if (context_.wideLines()) {
        const float* range = context_.deviceProperties().limits.lineWidthRange;
        bound_.setLineWidth(cmd, std::clamp(lineWidth_, range[0], range[1]));
    }
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

namespace {

bool sameImages(const std::vector<VkDescriptorImageInfo>& a, const std::vector<VkDescriptorImageInfo>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].sampler != b[i].sampler || a[i].imageView != b[i].imageView || a[i].imageLayout != b[i].imageLayout)
            return false;
    return true;
}

bool sameBuffers(const std::vector<VkDescriptorBufferInfo>& a, const std::vector<VkDescriptorBufferInfo>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].buffer != b[i].buffer || a[i].offset != b[i].offset || a[i].range != b[i].range) return false;
    return true;
}

} // namespace

// The program's uniform block ranges and sampler images from the current
// bindings, with WebGL's draw-time errors (false). `changed` is set when
// they differ from what the program's descriptor set was written with.
bool WebGLVkContext::resolveProgramBindings(VkProgramResource& prog, bool& changed) {
    changed = false;
    const ProgramInterface& iface = prog.iface;
    auto& blocks = drawBlocks_;
    blocks.resize(iface.uniformBlocks.size());
    for (size_t i = 0; i < blocks.size(); ++i)
        if (!uniformBlockRange(prog, i, blocks[i])) return false;

    auto& images = drawImages_;
    images.resize(iface.samplerUnitCount);
    for (const VkSamplerBinding& s : iface.samplers) {
        const SamplerKind want = samplerKind(s.type);
        for (uint32_t e = 0; e < s.count; ++e) {
            const uint32_t unit = prog.samplerUnits[s.firstUnit + e];
            VkImageView view = placeholderView(s.type);
            VkSampler sampler = want == SamplerKind::Shadow ? placeholderShadowSampler_ : placeholderSampler_;
            const GLuint texId = textureForSampler(s.type, unit);
            auto tIt = texId != 0 ? textures_.find(texId) : textures_.end();
            VkTextureResource* tex = tIt != textures_.end() ? &tIt->second : nullptr;
            // A bound sampler object's state replaces the texture's own.
            const GLuint samplerId = unit < boundSamplers_.size() ? boundSamplers_[unit] : 0;
            auto smpIt = samplerId != 0 ? samplers_.find(samplerId) : samplers_.end();
            uint32_t base = 0, count = 0;
            if (tex && tex->isValid()) {
                SamplerState state = smpIt != samplers_.end() ? smpIt->second.state : tex->sampler;
                if (textureComplete(*tex, state, base, count)) {
                    if (!samplerMatches(want, tex->tf, state) || sampledByPass(*tex, base, count)) {
                        setSyntheticError(GL_INVALID_OPERATION);
                        return false;
                    }
                    if (want != SamplerKind::Shadow) state.compareMode = GL_NONE;
                    view = sampledView(*tex, base, count);
                    sampler = samplerFor(state, tex->tf, state.mipmapped());
                    if (view == VK_NULL_HANDLE || sampler == VK_NULL_HANDLE) return false;
                }
            }
            images[s.firstUnit + e] = {sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        }
    }

    if (!sameBuffers(blocks, prog.drawBlocks) || !sameImages(images, prog.drawImages)) {
        prog.drawBlocks.assign(blocks.begin(), blocks.end());
        prog.drawImages.assign(images.begin(), images.end());
        changed = true;
    }
    return true;
}

// Bind the program's samplers, uniform blocks and default-block uniforms as
// one descriptor set. The set comes from the stream segment and is written
// once, before any command uses it; a draw whose bindings match the previous
// draw of the same program in the same segment reuses that set. The samplers
// and blocks are resolved again only when bindingSerial_ has moved since the
// program's last successful draw (nothing they are resolved from changed
// otherwise, so they would resolve the same, without error). The default
// block is a dynamic uniform buffer over the upload chunk its slice is in:
// new uniform values stage a new slice and change only the dynamic offset,
// unless the slice lands in another chunk. The set is bound only when it,
// the pipeline layout or that offset differs from what the pass has bound.
bool WebGLVkContext::bindProgramResources(VkCommandBuffer cmd, VkProgramResource& prog, DrawShape& shape) {
    const ProgramInterface& iface = prog.iface;
    const uint64_t serial = stream_.segmentSerial();
    const bool sameSegment = prog.drawSegmentSerial == serial;
    // A null drawSet is rewritten below: whatever changes what the set holds
    // nulls it on the spot, so a draw that fails after the change still
    // leaves the next one to write it.
    if (!sameSegment) prog.drawSet = VK_NULL_HANDLE;

    if (prog.drawResolvedSerial != bindingSerial_) {
        bool changed = false;
        if (!resolveProgramBindings(prog, changed)) return false;
        if (changed) prog.drawSet = VK_NULL_HANDLE;
        prog.drawResolvedSerial = bindingSerial_;
    }

    if (iface.defaultBlockBinding >= 0 &&
        (!sameSegment || prog.drawUniforms.buffer == VK_NULL_HANDLE || prog.drawUniformVersion != prog.uniformVersion)) {
        // The stream aligns every slice to minUniformBufferOffsetAlignment at
        // least, as a dynamic offset must be; the slice lies inside its chunk,
        // so offset + range stays within the buffer.
        render::UploadSlice slice = stage(prog.uniformBytes.data(), prog.uniformBytes.size(),
                                          context_.deviceProperties().limits.minUniformBufferOffsetAlignment);
        if (!slice) return false;
        if (slice.buffer != prog.drawUniforms.buffer || prog.drawUniforms.range != prog.uniformBytes.size()) {
            prog.drawUniforms = {slice.buffer, 0, prog.uniformBytes.size()};
            prog.drawSet = VK_NULL_HANDLE;
        }
        prog.drawUniformOffset = static_cast<uint32_t>(slice.offset);
        prog.drawUniformVersion = prog.uniformVersion;
    }

    if (iface.feedbackBuffers > 0) {
        auto& captures = drawCaptures_;
        captures.clear();
        feedbackDescriptors(prog, shape, captures);
        if (!sameBuffers(captures, prog.drawCaptures)) {
            prog.drawCaptures.assign(captures.begin(), captures.end());
            prog.drawSet = VK_NULL_HANDLE;
        }
    }

    if (prog.drawSet == VK_NULL_HANDLE) {
        VkDescriptorSet set = stream_.allocDescriptorSet(prog.setLayout);
        if (set == VK_NULL_HANDLE) {
            setSyntheticError(GL_OUT_OF_MEMORY);
            return false;
        }
        const auto& blocks = prog.drawBlocks;
        const auto& captures = prog.drawCaptures;
        auto& writes = drawWrites_;
        writes.clear();
        writes.reserve(iface.samplers.size() + blocks.size() + captures.size() + 1);
        for (const VkSamplerBinding& s : iface.samplers)
            writes.push_back({VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, s.binding, 0, s.count,
                              VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &prog.drawImages[s.firstUnit], nullptr,
                              nullptr});
        for (size_t i = 0; i < blocks.size(); ++i) {
            const VkUniformBlockInfo& b = iface.uniformBlocks[i];
            writes.push_back({VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, b.descriptorBinding,
                              b.arrayElement, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &blocks[i], nullptr});
        }
        if (iface.defaultBlockBinding >= 0)
            writes.push_back({VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set,
                              static_cast<uint32_t>(iface.defaultBlockBinding), 0, 1,
                              VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, nullptr, &prog.drawUniforms, nullptr});
        for (uint32_t b = 0; b < captures.size(); ++b)
            writes.push_back({VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, kFeedbackBinding + b, 0, 1,
                              VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &captures[b], nullptr});
        vkUpdateDescriptorSets(context_.device(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        prog.drawSet = set;
    }
    prog.drawSegmentSerial = serial;
    // One dynamic offset (the default block, the set's only dynamic
    // descriptor) or none.
    const uint32_t dynamicCount = iface.defaultBlockBinding >= 0 ? 1 : 0;
    const uint32_t dynamicOffset = dynamicCount ? prog.drawUniformOffset : 0;
    if (bound_.descriptorSet != prog.drawSet || bound_.pipelineLayout != prog.pipelineLayout ||
        bound_.dynamicOffset != dynamicOffset) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, prog.pipelineLayout, 0, 1, &prog.drawSet,
                                dynamicCount, &dynamicOffset);
        bound_.descriptorSet = prog.drawSet;
        bound_.dynamicOffset = dynamicOffset;
    }
    return true;
}

// Everything a draw needs bound before its vkCmdDraw*: the render pass, the
// pipeline for the current state, dynamic state, descriptors and vertex
// buffers. Returns false (with the GL error set) when the draw cannot happen.
bool WebGLVkContext::prepareDraw(GLenum mode, VkProgramResource& prog, DrawShape& shape) {
    auto& vbos = drawVbos_;
    auto& offsets = drawOffsets_;
    vbos.clear();
    offsets.clear();
    if (rasterizerDiscardEnabled_ && !shape.feedback) {
        // Nothing reaches the framebuffer: the draw only has its errors.
        bindVertexInputs(prog, shape, vbos, offsets);
        return false;
    }
    beginRendering();
    if (!inRenderPass_) return false;
    VkCommandBuffer cmd = commands();

    const VaoInputCache* inputs = bindVertexInputs(prog, shape, vbos, offsets);
    if (!inputs) return false;
    VkPipeline pipeline = drawPipeline(mode, prog, shape, *inputs);
    if (pipeline == VK_NULL_HANDLE) {
        LOG_ERROR("WebGLVkContext: Failed to obtain graphics pipeline");
        setSyntheticError(GL_INVALID_OPERATION);
        return false;
    }
    if (!bindProgramResources(cmd, prog, shape)) return false;
    if (bound_.pipeline != pipeline) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        bound_.pipeline = pipeline;
    }
    const VkExtent2D extent = pass_.extent;
    const FragmentPush push =
        pass_.topDown ? FragmentPush{static_cast<float>(extent.height), -1.0f} : FragmentPush{};
    if (!bound_.fragmentPushSet || bound_.pipelineLayout != prog.pipelineLayout ||
        std::memcmp(&bound_.fragmentPush, &push, sizeof(push)) != 0) {
        vkCmdPushConstants(cmd, prog.pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
        bound_.fragmentPush = push;
        bound_.fragmentPushSet = true;
    }
    bound_.pipelineLayout = prog.pipelineLayout;
    if (!prog.iface.feedbackVaryings.empty()) {
        const FeedbackPush capture = shape.feedback ? shape.feedbackPush : FeedbackPush{};
        vkCmdPushConstants(cmd, prog.pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, kFeedbackPushOffset,
                           sizeof(capture), &capture);
    }
    setDynamicState(cmd, extent, rasterizerDiscardEnabled_);
    if (!vbos.empty())
        bound_.bindVertexBuffers(cmd, static_cast<uint32_t>(vbos.size()), vbos.data(), offsets.data());
    return true;
}

} // namespace bro::webgl::vk
